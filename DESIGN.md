# pg_stat_statement_context — Design Document

> Status: Draft / proposal
> Target: PostgreSQL 14, 15, 16, 17, 18

## 1. Summary

`pg_stat_statement_context` is a PostgreSQL extension that attributes query
execution statistics to **application context** carried in SQL comments, such as
those emitted by [marginalia], Rails `query_log_tags`, and [SQLCommenter].

Where `pg_stat_statements` answers *"which query fingerprints are expensive?"*,
this extension answers *"which parts of my application are running query
fingerprint X, how often, and at what cost?"*

```
 queryid  | controller | action |  route        | calls | total_exec_time
----------+------------+--------+---------------+-------+-----------------
 -8812... | users      | show   | /users/:id    | 41022 |        1833.20
 -8812... | admin/users| index  | /admin/users  |   210 |         912.75
```

### Goals

- Aggregate per **(query fingerprint × extracted tag set)** in shared memory.
- Low, predictable overhead on the hot path; no extra SQL parse.
- Declarative configuration for which tags to extract, from which comment
  format, and where in the query to look.
- Bounded memory with a rolling, time-bucketed history.
- Track DML/SELECT **and** utility statements (DDL, etc.).
- Support PostgreSQL 14+ from a single source tree.
- Complement, not replace, `pg_stat_statements` (join on `queryid`). The
  extension is a **companion** to pgss: per (query × context) it stores only
  `calls` and `total_exec_time` (§5.1, decided 2026-10-05).

### Non-goals (v1)

- Storing query text (use `pg_stat_statements` for that).
- Storing any per-statement counter other than `calls` and `total_exec_time`
  (rows, buffers, WAL, I/O timing, JIT, min/max/mean/stddev, planning time).
  pgss already tracks these per `queryid`; join to it (§7).
- Plan capture, histograms, wait-event sampling, OS-level resource usage,
  error counts (§8 "Rejected").
- Being a general-purpose `pg_stat_statements` replacement (that is what
  `pg_stat_monitor` is).

## 2. Background: why this needs an extension

The core lexer (`src/backend/parser/scan.l`) discards comments the same way it
discards whitespace. They never become tokens and are absent from both the raw
parse tree and the analyzed `Query`. However, **every hook that runs after
parsing still has access to the original source string**:

| Hook | Source text | Query ID |
|------|-------------|----------|
| `post_parse_analyze_hook` | `pstate->p_sourcetext` | `query->queryId` |
| `ExecutorStart/End_hook` | `queryDesc->sourceText` | `queryDesc->plannedstmt->queryId` |
| `ProcessUtility_hook` | `queryString` argument | `pstmt->queryId` |

So the design does **not** hook in before parsing and does **not** parse twice.
Postgres parses once; the extension does a cheap lexical scan of the raw string
to locate comments, then extracts tags from only those bytes.

Since PG14, core computes `queryId` itself (`compute_query_id`). The extension
calls `EnableQueryId()` in `_PG_init`, which makes `compute_query_id = auto`
behave as `on`. Fingerprints are therefore identical to those in
`pg_stat_statements`, so the two views can be joined. The extension never
replaces or rewrites the core `queryId`. This includes PG14/15 utility statements
(§6.6).

## 3. Architecture

```
                 ┌──────────────────────────── backend process ────────────────────────────┐
 client SQL ───► │ parse ─► analyze ─► plan ─► ExecutorStart ─► Run/Finish ─► ExecutorEnd  │
                 │                                   │               │             │       │
                 │                                   ▼               ▼             ▼       │
                 │                             scan + extract  activate frame  record into │
                 │                             (or inherit)    (PG_FINALLY)    store ──┐   │
                 │                                                                     │   │
                 │ ProcessUtility: snapshot id/range/tags ─► frame ─► chain ─► record ─┤   │
                 └─────────────────────────────────────────────────────────────────────┼───┘
                                                                                       ▼
                                     ┌─────────────────── shared memory ───────────────────┐
                                     │ HTAB: key(db, user, queryid, toplevel,              │
                                     │           canonical tag set) →                      │
                                     │       per-bucket ring of (calls, total_exec_time)   │
                                     │ LWLock + per-entry spinlocks                        │
                                     └──────────────────────────┬──────────────────────────┘
                                                                ▼
                                        SQL: pg_stat_statement_context views / functions
```

### 3.1 Components

1. **Comment scanner** (`scan.c`): a single-pass state machine that finds comment
   regions in a `const char *`, following `scan.l`'s lexical rules. It correctly
   skips `'strings'`, including escape semantics when
   `standard_conforming_strings = off`. It also skips `E''` and `U&''` strings,
   `"quoted identifiers"`, `$tag$dollar quotes$tag$`, and `$1` parameters. A `$`
   inside an identifier (`a$b$`) does not start a dollar quote. It handles nested
   `/* /* */ */` and `--` line comments. Output: a small array of `(offset, len)`
   comment spans. No allocation in the common case.
2. **Extractors** (`extract.c`): built-in parsers for SQLCommenter and marginalia
   formats, plus an optional regex extractor. They turn comment spans into a
   canonical **tag set**: decoded, validated, and truncated tag pairs sorted by
   key, serialized within a bounded size (§5.1, §6.11).
3. **Execution frames** (`context.c`): backend-local state that is owned per
   executor instance (`QueryDesc`) and per utility call. Each frame holds the
   resolved tag set and statement metadata. A backend-local *active frame*
   pointer is set only while a hook is executing (§3.3), so nested statements
   (PL/pgSQL, triggers, SPI) can inherit their parent's tags.
4. **Shared store** (`store.c`): fixed-size shared hash table with one entry
   per (query × context). Each entry holds a small ring of per-bucket counters
   (§5.1, §5.2). It handles locking, expiry, and eviction.
5. **SQL interface** (`pg_stat_statement_context--1.0.sql`): set-returning
   function, views, reset function, and info function.
6. **Config** (`guc.c`): GUC definitions. The `check_hook` parses and fully
   validates the extractor DSL, including regex syntax, and returns a flat,
   immutable `extra` blob. The `assign_hook` only installs that pointer and bumps
   a config generation, so it cannot fail (§4.2).

### 3.2 Hook placement

| Hook | Responsibility |
|------|----------------|
| `shmem_request_hook` (PG15+) / `RequestAddinShmemSpace` in `_PG_init` (PG14) | Reserve shared memory and LWLock tranche. |
| `shmem_startup_hook` | Create or attach the shared header and hash table. |
| `post_parse_analyze_hook` | Nothing in v1. |
| `ExecutorStart` | If enabled and `queryId != 0`: create the executor frame in `es_query_cxt` and resolve its tags eagerly (scan, or inherit from the active frame). Set up `queryDesc->totaltime`, as pgss does. |
| `ExecutorRun` / `ExecutorFinish` | Make this frame active and increment `nesting_level`. Restore both in `PG_FINALLY`. |
| `ExecutorEnd` | Add one call and the elapsed time from `queryDesc->totaltime` to the store under the frame's key, then drop the frame. |
| `ProcessUtility` | Before chaining, snapshot `queryId`, statement bounds, and tags into a utility frame. Activate it and bump nesting (`EXECUTE`/`PREPARE` get no frame and no nesting bump; on PG14–16 nesting mirrors pgss, §6.7) around the chained call, timing it. Record from the snapshot afterwards. Never touch `pstmt` after chaining (§6.7). |

No `planner_hook` is used for timing: planning time is left to pgss
(`track_planning`), see §8 "Rejected". On PG17+ only, a minimal `planner_hook`
adds one nesting level around the chained planner and restores it in
`PG_FINALLY`. It does no timing and activates no frame, so SQL run during
planning (constant-folded functions) is not top-level, matching pgss, which
counts planner nesting only from PG17.0. On PG14–16 pgss ignores planning when
deciding `toplevel`, so no hook is installed there (decided 2026-10-05, item
-16).

Frame details (item -16):
- **User and nesting refresh:** `pssc_frame_refresh()` updates `userid`,
  nesting level, `toplevel` and recordability at recording time (ExecutorEnd,
  or after a utility returns), because pgss reads them then. For example, a
  cursor closed under a different role is recorded under the closing role.
- **PG18 statement-boundary cache (§6.5):** used and updated only when the
  statement's source is the client query string (`debug_query_string`), at
  nesting level 0, with no active frame. Planning-time, nested and
  `EXECUTE`'d sources neither read nor overwrite it.
- **Statements that get no frame:**
  - `DECLARE CURSOR`'s inner query has `queryId` 0.
  - PL/pgSQL simple expressions (`x := expr`) skip the executor.
- **PL/pgSQL `INTO`:** PL/pgSQL blanks out only the `INTO target` clause, so a
  comment anywhere else in the statement, including after `INTO`, is seen
  (verified on PG14 and PG17 in item -28; corrects an earlier note). PL/pgSQL
  does drop a comment that ends a `PERFORM` statement or an expression such as
  `RETURN (SELECT ...)`.

Executor hooks (item -17):
- **`totaltime`:** allocated exactly as pgss does (14–18):
  `InstrAlloc(1, INSTRUMENT_ALL, false)` in `es_query_cxt`, only when it is
  still NULL and the statement is tracked at the current level. It uses
  `INSTRUMENT_ALL` rather than a bare timer because whichever hook allocates
  first decides what pgss gets.
- **Recording rule:** record at `ExecutorEnd` when a frame exists, it is
  recordable after `pssc_frame_refresh()`, and `totaltime` is set. This is
  pgss's rule plus the `untagged` policy.
- **Stats flush:** backend-local extraction counters are flushed into the
  shared header on every `ExecutorEnd`, before chaining.
- **Load order:** the executor hooks work with pgss loaded before or after
  this extension; only the utility hook (-18) depends on the order.
- **Plain `EXPLAIN`:** the inner statement is nested under the `EXPLAIN`
  utility (item -18), matching pgss.

**Why the executor hooks, not `post_parse_analyze`?** Parse analysis is skipped
when a cached plan or prepared statement is re-executed, but the executor hooks
fire on every execution. Tags are resolved at `ExecutorStart` because children
must be able to inherit them while the parent is still running. Counters are
recorded at `ExecutorEnd`, where the timing is final.

**Load order.** pgss saves the utility `queryId`, then sets `pstmt->queryId = 0`
**before** calling the next `ProcessUtility` hook. It does this whenever it is
enabled and `track_utility` is on, and it also warns that `pstmt` may be freed
by `ROLLBACK`. This extension's hook must therefore run outside pgss's:
`shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'`
(the library loaded last installs the outermost hook). `_PG_init` checks the
order in `shared_preload_libraries` and logs a `WARNING` if it is wrong. (Item -19: the list is parsed with `SplitDirectoriesString`, as the
postmaster does; entries match by basename, case-insensitively on every
platform, ignoring `.so`/`.dylib`/`.dll`/`.sl`; only each library's first entry
counts; the check is skipped when `IsUnderPostmaster`, so EXEC_BACKEND children
don't repeat it. The HINT reads
`shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'`.) The
warning is the only action: utility tracking is not disabled (decided
2026-10-05, §11 Q7). At
runtime, utility calls that arrive with `queryId = 0` are counted in
`_info().utility_missing_queryid` rather than being recorded. The extension
itself never modifies `pstmt->queryId`, because pgss inside it depends on that
value.

**Frame lifetime.** A frame is not pushed at Start and popped at End. An
extended-protocol Bind calls `ExecutorStart` via `PortalStart`, suspended portals
interleave, and `PortalCleanup` skips `ExecutorEnd` for failed portals. Frames
are allocated in the executor's `es_query_cxt` and registered in a small
backend-local list. A `MemoryContextCallback` unlinks the frame when that context
is destroyed, which also covers abort paths. The active-frame pointer is only
ever saved and restored around hook calls.

### 3.3 Hot path (per statement)

```
ExecutorStart:
  chain                                   -- creates estate / es_query_cxt
  if !enabled || IsParallelWorker() || queryId == 0: return
  frame = new frame in es_query_cxt (unlinked by memory-context callback)
  frame.tags = (active_frame && nested_tags == inherit) ? active_frame.tags
             : extract(sourceText, stmt_location, stmt_len)   -- before any lock

ExecutorRun/Finish:
  save = active_frame; active_frame = frame; nesting_level++
  PG_TRY: chain  PG_FINALLY: nesting_level--; active_frame = save

ExecutorEnd:
  if frame recordable (track, toplevel, untagged policy):
    store_record(frame.key, 1 call, totaltime)   -- §5.4
  chain
```

There is no cross-execution memoization in v1. Extraction runs once per
executor instance, and a portal fetched repeatedly is a single instance (one
Start, many Runs, one End). Any future cache would need to key on source
content, statement bounds, config generation, and lexical settings, and would
need explicit ownership. A raw `sourceText` pointer is borrowed from the portal
or plan cache and can dangle.

## 4. Configuration

All configuration uses GUCs, so it can be set in `postgresql.conf`, with
`ALTER SYSTEM`, and is reloadable on SIGHUP unless marked otherwise. Settings
that size shared memory require a restart.

### 4.1 Core GUCs

| GUC | Default | Context | Description |
|-----|---------|---------|-------------|
| `pg_stat_statement_context.enabled` | `on` | superuser | Master switch. |
| `pg_stat_statement_context.track` | `top` | superuser | `none` / `top` / `all`, as in pgss. |
| `pg_stat_statement_context.track_utility` | `on` | superuser | Record utility/DDL statements. |
| `pg_stat_statement_context.nested_tags` | `inherit` | superuser | `inherit` (use top-level tags) / `scan` (scan nested source) / `none`. |
| `pg_stat_statement_context.max_entries` | `10000` | postmaster | Max (query × context) combinations. Each entry holds a counter ring of `bucket_count` slots, so this is independent of `bucket_count` (§5.1). |
| `pg_stat_statement_context.bucket_count` | `12` | postmaster | Number of time buckets (slots in each entry's counter ring). |
| `pg_stat_statement_context.bucket_interval` | `300s` | postmaster | Width of each bucket. 12 × 5 min = 1 hour history. Fixed at startup so all backends agree on bucket IDs (§5.2). |
| `pg_stat_statement_context.max_tags` | `8` | postmaster | Max tags stored per entry. |
| `pg_stat_statement_context.max_tag_value_len` | `64` | postmaster | Bytes per tag value. Longer values are truncated on a character boundary. Keys are limited to 63 bytes, and longer keys are dropped. |
| `pg_stat_statement_context.max_tagset_bytes` | `512` | postmaster | Hard cap on the serialized tag set, which is part of the hash key (§5.1). Tags are kept greedily in priority order (allowlist order, or sorted keys for `'*'`); a tag that doesn't fit is dropped and counted, and smaller lower-priority tags may still be kept (§6.11). |
| `pg_stat_statement_context.scan_window` | `2kB` | sighup | Max bytes from the head/tail searched for comments (see §6.2). |
| `pg_stat_statement_context.extractors` | `'sqlcommenter, marginalia'` | sighup | Extractor DSL (§4.2). |
| `pg_stat_statement_context.tags` | `'action, controller, job'` | sighup | Allowlist of tag keys to keep, applied after `rename`. Tags not listed are discarded. `'*'` keeps all tags (not recommended, see §6.1). |
| `pg_stat_statement_context.exclude_tags` | `'traceparent, tracestate, request_id'` | sighup | Denylist (high-cardinality). Only relevant when `tags = '*'`. |
| `pg_stat_statement_context.untagged` | `skip` | sighup | `skip` statements without tags (default, decided 2026-10-05, §11 Q1) / `record` them with an empty tag set. |

The configuration lives in GUCs only; there is no separate config file
(decided 2026-10-05, §11 Q3). To change it from SQL, use
`ALTER SYSTEM SET pg_stat_statement_context.extractors = '...';` followed by
`SELECT pg_reload_conf();`. This works for every `sighup` setting, including
`extractors`, `tags`, and `exclude_tags`.

### 4.2 Extractor DSL

A comma-separated list of extractors, each with optional parameters. The first
extractor that produces at least one tag wins, unless `merge=on` is set.

```
extractor   := name [ '(' param { ',' param } ')' ]
name        := 'sqlcommenter' | 'marginalia' | 'regex'
param       := key '=' value
```

Common parameters:

| Param | Values | Meaning |
|-------|--------|---------|
| `position` | `append` / `prepend` / `any` | Where the comment is expected. `append` = the trailing run of comments (the last comment plus any immediately preceding comments separated only by whitespace) before optional trailing `;`/whitespace; `prepend` = the leading run of comments before the first token. A run, not a single comment, so that marginalia's `with_annotation` comment after the context comment doesn't hide it (decided 2026-10-05). The extractor chain decides which comments in the run to parse. Default when omitted: `append` for `sqlcommenter` and `marginalia`, `any` for `regex` (decided 2026-10-05). |
| `keys` | `a\|b\|c` | Per-extractor allowlist. It matches the **original** key names as they appear in the comment, and is applied before `rename` (decided 2026-10-05, §6.11). |
| `rename` | `old:new\|...` | Normalize key names across formats (`controller` vs `route`). |
| `merge` | `on` / `off` | Union tags with earlier extractors instead of stopping. |

Format-specific parameters:

- **sqlcommenter**: `/*key='value',key2='value2'*/`. Values are URL-decoded
  and `\'` is unescaped. Parameters: `url_decode=on|off`. With `url_decode=on`,
  keys and values are both decoded. `%XX` is decoded and a raw `+` becomes a
  space, so Go and Java emitters (form encoding) and Python and Node emitters
  (`%20`) give the same value; `%2B` is a literal `+`. An invalid `%` escape is
  kept literally and flagged. Only `\'` and `\\` are unescaped. Values must
  be single-quoted, and a `,` inside quotes doesn't split.
- **marginalia**: `/*application:Foo,controller:users,action:show*/`. Splits
  on the first `kv_sep` only, because values like `line:app/models/u.rb:12`
  contain colons. Parameters: `kv_sep=':'`, `pair_sep=','`. Pairs are split
  first, so `pair_sep` wins when the separators overlap. No decoding is done.
- **Both parsers:** ASCII whitespace is trimmed around the body, each pair,
  and each key and value. Empty segments are ignored. A segment without a
  separator, or a key that contains whitespace, is malformed: it is skipped
  and counted. That keeps free-text annotations such as marginalia's
  `with_annotation` from turning into tags. Decoded NUL bytes are flagged and
  rejected by the pipeline (§6.11). The parsers live in `src/pairs.c`, have
  no backend dependencies and never allocate (decided 2026-10-05).
- **regex**: `regex(pattern='...', keys='k1|k2', position=any)`. Uses the core
  regex engine (`pg_regcomp`/`pg_regexec`) with `REG_ADVANCED` and the C
  collation (`C_COLLATION_OID`, which needs no catalog access). Capture group
  *n* maps to key *n*. Applied **only to comment text**, never to the full
  query. The engine works on `pg_wchar`, so comment bytes are converted first and
  capture offsets are mapped back to bytes. v1 limits: pattern ≤ 1 kB, captures
  ≤ `max_tags`, and patterns with back-references are rejected
  (`re_info & REG_UBACKREF`).

  Matching rules (decided 2026-10-05, item -10):
  - Every non-overlapping match in the comment body is used, as with
    `regexp_matches(..., 'g')`. After an empty match the search moves on one
    character, and `^` anchors only at the start of the body.
  - An unmatched optional group produces no pair.
  - The first occurrence of a key wins, and matching stops once every key has
    a value.
  - A comment with invalid encoding or a NUL byte produces no pairs.
  - A pattern that is invalid in the database's encoding counts as a compile
    failure.

  Engine errors:
  - Engine calls run in `PG_TRY` without a subtransaction, because the engine
    holds only memory.
  - Cancel, timeout, shutdown, deadlock and serialization errors are re-thrown.
  - Other errors (e.g. OOM) disable the extractor for the backend: a compile
    error until the next config change, counted in `regex_compile_failures`.
    A match error simply yields no further pairs and is not counted.
- **appname** (roadmap, §8): `appname(format=sqlcommenter|marginalia|regex)`
  parses `application_name` instead of comment text, using the named format's
  rules (and `pattern`/`keys` for `regex`). Tags from comments win over
  `appname`-derived tags on key conflicts.

Examples:

```ini
# Rails app (marginalia or query_log_tags legacy format, appended).
# The default allowlist (action, controller, job) is used; this adds application.
pg_stat_statement_context.extractors = 'marginalia(position=append)'
pg_stat_statement_context.tags       = 'application,controller,action,job'

# Polyglot shop: Django/Flask via SQLCommenter, Rails via marginalia prepended.
# The allowlist is applied after rename, so it must name the renamed key.
pg_stat_statement_context.extractors = 'sqlcommenter(position=append, rename=route:endpoint), marginalia(position=prepend, rename=controller:endpoint)'
pg_stat_statement_context.tags       = 'endpoint,action,job'

# Custom house format: /* svc=billing op=charge */
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=service|operation)'
pg_stat_statement_context.tags       = 'service,operation'
```

Validation happens in the GUC `check_hook`, so a malformed config is rejected
when it is set or reloaded and the previous config stays active. The
`check_hook` parses the DSL and test-compiles and frees every regex, then
returns the parsed form as one flat, pointer-free `extra` blob. That blob is
allocated with `malloc` on PG14/15, where guc.c frees it with `free()`, and with
`guc_malloc` on PG16+, where it must live in the GUC memory context. A `compat.h`
helper hides this difference. The `assign_hook` only stores the pointer and
bumps a config generation number. GUC assign hooks must not fail, because they
also run during transaction rollback.

The `check_hook` rejects the following (decided 2026-10-05, item -8):

- unknown extractors or parameters, and duplicate parameters
- more than 16 extractors
- keys that are empty or longer than 63 bytes
- empty separators, or separators longer than 8 bytes
- a `kv_sep` that contains `pair_sep`, because pairs are split on `pair_sep` first, so such a `kv_sep` could never match

Separators that start with whitespace are allowed (for example `kv_sep=' :'`).
For `regex`, the number of `keys` must equal the number of capture groups.
The generation number is bumped only when the parsed blob changes.

Note: values in `postgresql.conf` undergo backslash-escape processing, so a
regex there must double its backslashes (`\\w`). `ALTER SYSTEM` writes the
escaping for you.

Compiled regexes are not part of `extra`, because GUC frees only the top-level
block. Each backend compiles them lazily on first use after a generation change,
into a private memory context that it owns, and frees the old ones with
`pg_regfree`. Regex allocation uses `malloc` on PG14/15 and `palloc` on PG16+. If
lazy compilation fails (for example, out of memory), that extractor is disabled
for the backend and the failure is counted. The statement itself is not failed.

> **Alternatives considered:** a config table (`pg_stat_statement_context.rules`).
> Rejected for v1 because reading catalogs from `ExecutorEnd` adds overhead,
> is database-local while the extension is cluster-wide, and gets complicated
> inside aborted transactions. GUCs are already reloadable, permissioned, and
> shown in `pg_settings`. A separate `config_file` GUC for complex setups was
> also rejected (§11 Q3): `ALTER SYSTEM` plus `pg_reload_conf()` (§4.1) already
> gives a from-SQL path with the same all-or-nothing validation.

## 5. Storage and the rolling buffer

### 5.1 Key and entry

The extension is a **companion** to `pg_stat_statements`, not a replacement
(decided 2026-10-05). Each (query × context) entry stores only two counters
per time bucket: `calls` and `total_exec_time`. Everything else that pgss
tracks per `queryid` (rows, shared/local/temp blocks, WAL, I/O timing, JIT,
min/max/mean/stddev, planning time) is left to pgss, and users join to it on
`(userid, dbid, queryid, toplevel)` (§7).

*Why `total_exec_time` and not just `calls`:* `calls` alone can't apportion
load across contexts when the per-context cost of the same `queryid` differs
(for example, one controller passes a selective parameter and another a
non-selective one). `total_exec_time` is the minimum needed to attribute
cost to a context. Other pgss metrics can be apportioned approximately by
each context's share of `total_exec_time` (§7).

The key layout is **one entry per (query × context) holding a per-bucket
counter ring**. `bucket_id` is *not* part of the key (decided 2026-10-05,
§11 Q5). Because an entry holds only two counters per bucket, the ring costs
about 24 bytes per bucket (288 bytes with the default 12 buckets), which is
cheap compared with the key itself.

```c
typedef struct ctxKey {
    Oid     dbid;
    Oid     userid;
    int64   queryid;
    bool    toplevel;
    uint16  tags_len;      /* bytes used in tags[] */
    uint32  tags_hash;     /* precomputed hash of tags[0..tags_len) */
    char    tags[FLEXIBLE];/* canonical "k\0v\0k\0v\0", max_tagset_bytes */
} ctxKey;

typedef struct ctxSlot {
    int64   bucket_id;     /* absolute bucket number this slot holds (§5.2) */
    int64   calls;
    double  total_exec_time;   /* ms */
} ctxSlot;

typedef struct ctxEntry {
    ctxKey   key;          /* keysize fixed at startup */
    slock_t  mutex;        /* protects slots[], last_bucket, usage */
    int64    last_bucket;  /* newest bucket_id written; drives reclamation */
    double   usage;        /* pgss-style usage for eviction (not exposed) */
    int      encoding;     /* encoding of tags[] (that of dbid) */
    ctxSlot  slots[FLEXIBLE]; /* bucket_count slots; index = bucket_id mod bucket_count */
} ctxEntry;
```

`ctxSlot` is implemented as `PsscSlot` in `src/counters.[ch]` (item -12).

- An unwritten slot has `bucket_id = PSSC_BUCKET_NONE` (`INT64_MIN`), which is
  older than any real bucket and is skipped by merges.
- Bucket ids are signed `int64`, so "older" is a plain `<`.
- Usage follows pgss: it starts at 1.0, each call adds 1.0, and each eviction
  pass multiplies it by 0.99.
- There are no "sticky" entries, because an entry is only created when a call
  is recorded.

The **full canonical tag set is part of the key**, so distinct tag sets can
never be merged and no collision probing is needed. Like pgss, dynahash chains
entries and compares the actual keys. The table uses custom `HASH_FUNCTION` and
`HASH_COMPARE` callbacks. The hash combines the fixed fields with `tags_hash`.
The compare checks the fixed fields and `tags_len`, then runs `memcmp` on only
the used bytes. Keys are still built by `memset`-ing the whole key to zero first
(pgss does the same), so padding and unused tag bytes are always defined.

`keysize` is computed at startup from `max_tagset_bytes` (`MAXALIGN(24 +
max_tagset_bytes)`; 24 is the fixed key header), and `entrysize` from `keysize`
and `bucket_count` (`keysize + MAXALIGN(entry header) + bucket_count × 24`). Shared memory is sized as
`hash_estimate_size(max_entries, entrysize)` plus the header, using
`add_size`/`mul_size` overflow checks. The table is created with
`init_size = max_size = max_entries`, so all entries are preallocated.
`ShmemInitHash`'s `max_size` is only an estimate, not a limit, so the
`max_entries` cap is enforced by the extension under the exclusive lock.
With the defaults, an entry is on the order of 1 KB, dominated by the tag set.
`_info()` reports the exact `shmem_bytes` value.

A record is dropped and counted in the header counter `dropped_records` only
when an eviction pass (§5.3) freed nothing: no entry was dead and the sort
array could not be allocated. Testing (§9) uses
a forced-collision mode, set only through the test module and only while the
table is empty. The effective hash is chosen under the table lock.

**Capacity is measured in (query × context) combinations**, independent of
`bucket_count`: 5,000 recurring combinations need 5,000 entries whether they
are active in one bucket or all of them. A combination keeps its entry while
any of its slots is live; once all its slots have expired it is *dead* and is
reclaimed first under pressure (§5.3). The documentation should give this
sizing rule.

### 5.2 Time buckets

- `bucket_interval` is fixed at startup (postmaster GUC) and stored in the
  shared header along with an epoch. Every backend computes
  `bucket_id = floor((now - epoch) / interval)` as an `int64`, so all backends
  agree and stored IDs always map back to the right timestamps.
- `current_bucket` in the header is a shared, **monotonic watermark**: the
  newest bucket any writer or reader has observed. It is stored as an `int64`
  in a `pg_atomic_uint64` and raised lock-free with a compare-and-swap max loop,
  so it never decreases (decided 2026-10-05, item -14). Advancing it touches no
  entries, because each entry's ring rolls over lazily (below). The epoch is the
  shared-memory init time rounded down to a multiple of `bucket_interval` since
  2000-01-01, so bucket boundaries fall on wall-clock multiples.
- Writers, once they hold the table lock (shared fast path or exclusive
  insert), re-read the clock and raise `current_bucket` to
  `max(current_bucket, clock bucket, computed id)`. The call is written to
  `current_bucket` as read under the entry spinlock. Older ids (a stalled
  backend, a backward clock step) and newer ones are thus clamped, and every
  write lands in a slot that is live at that moment. Calls are attributed to
  the bucket current when the lock is acquired.
- **Per-entry ring rollover:** under the entry spinlock, the writer picks
  `slot = bucket_id mod bucket_count`. If that slot holds an older
  `bucket_id`, the slot is zeroed and relabeled before the counters are added.
  `last_bucket` is set to the written ID. Since written IDs never exceed
  `current_bucket`, a slot never holds a newer ID than the one being written.
- If the wall clock moves backwards, `current_bucket` never decreases, and
  writes clamp to it. A forward jump larger than the ring makes every slot
  stale at once. Bucket arithmetic is signed, so the cutoff can't underflow.
- Rollover happens lazily on the write path, so no background worker is needed.
  Readers do not depend on writers: the SRF first raises `current_bucket` to
  the clock bucket, then hides slots outside the live window
  `[current - bucket_count + 1, current]`, using the watermark read after
  copying each entry. Once a slot has been seen as expired it stays expired,
  even if the clock later steps back. Reclamation (§5.3) uses the same
  watermark.
- Semantics: an execution is attributed to the bucket in which its
  `ExecutorEnd` (or utility completion) runs. Its whole accumulated time goes
  there, even for a long-lived cursor that started much earlier. Buckets
  therefore show **completions per interval**, not work done per interval.

### 5.3 Eviction under pressure

If an insert finds the table at `max_entries`, then under the exclusive lock:

1. Reclaim **dead** entries first: those whose `last_bucket` is older than the
   live window, so every slot has expired.
2. If that frees less than ~5% of `max_entries`, evict further live entries,
   pgss-style: order by `last_bucket` (least recently written first), then by
   `usage` (lowest first), and evict until ~5% is free. `usage` decays as in
   pgss.
3. Increment `dealloc` (once per eviction pass) and `evicted_entries` (per
   entry, live or dead), both exposed by `_info()`, so users can tell that
   `max_entries` is too small.

As in pgss, an eviction pass scans and sorts the whole table. That cost is paid
only when the table is full; the benchmarks measure it (§9).

Details (decided 2026-10-05, item -15):
- **Target:** each pass aims to free `max(1, max_entries * 5 / 100)` entries.
- **Dead entries:** the pass first raises `current_bucket` to the clock, as
  readers do. One scan then reclaims *all* dead entries, even beyond the
  target, without allocating anything.
- **Live entries:** these are evicted only when the dead entries fall short of
  the target.
- **Decay:** every surviving entry's `usage` is multiplied by 0.99 on every
  pass, as in pgss.
- **No entry spinlocks:** every path that takes an entry spinlock holds the
  table lock (shared), so the pass reads and writes `last_bucket` and `usage`
  without spinlocks while it holds the exclusive lock.
- **Out of memory:** the sort array is allocated with `MCXT_ALLOC_NO_OOM`. If
  that fails, only dead entries are reclaimed. The user's statement never
  fails, and `dealloc` still counts the pass.

### 5.4 Locking

The locking is modeled on `pg_stat_statements`, and latency is measured rather
than assumed (§9):

- One LWLock for the hash table. Shared mode is enough to look up an existing
  entry and update its ring under that entry's spinlock, including the lazy
  slot rollover (§5.2). Exclusive mode is only needed to insert, evict, or
  advance `current_bucket`.
- LWLocks cannot be upgraded. On a miss, the backend releases the shared lock,
  acquires the exclusive lock, and then re-validates everything. It re-checks
  the bucket (§5.2) and repeats the `HASH_ENTER` lookup, because another backend
  may have inserted the entry in the meantime. Only then does it evict, if
  needed, and insert.
- Because the key does not include the bucket, a recurring combination misses
  only once, when it is first seen (or after it was evicted), as in pgss. The
  only per-interval exclusive acquisition is the single header advance at each
  bucket boundary.
- Tag extraction and hashing happen **before** any lock is taken.

## 6. Gotchas and mitigations

### 6.1 Tag cardinality explosion
`traceparent`, `request_id`, and per-user values make every statement unique,
which can empty the table within seconds.
This is mostly an operator choice: only allowlisted keys are stored, and the
default allowlist is `action, controller, job`. New tags that application
developers add are therefore ignored until an operator opts in. The table can
still flood if an allowed tag has unexpectedly high-cardinality values (for
example, an unnormalized route like `/users/123`), or if a buggy or malicious
client sends random values for an allowed key.
**Mitigation:** the restrictive default allowlist, a denylist of known
high-cardinality keys for anyone who opts into `tags = '*'`,
`max_tag_value_len` truncation, and `_info()` counters for evictions. Roadmap
(§8): per-key value normalization rules, per-key cardinality caps that
collapse overflow values to JSON `null`, plus exemplar storage. `null` cannot
collide with a real value, because a client can only send strings.

### 6.2 Long queries (e.g., 10k-element `IN` lists)
Even a linear scan costs something on a 1 MB query string.
**Mitigation:** if the statement range fits within `scan_window`, it is always
lexed exactly from the front. For longer statements, `position=append|prepend`
limits work to the first or last `scan_window` bytes:
- The statement end comes from `stmt_len`. A `strlen` is needed only when
  `stmt_len == 0` (rest of string), and that cost is included in the overhead
  benchmarks.
- `append` trims whitespace and `;` **within the window only**, expects a
  closing `*/`, and walks backwards to the matching `/*` while tracking nesting
  depth. A trailing `--` comment, or a comment that crosses the window start,
  yields no tags. The scan never parses half a comment.
- Starting a lexer at an arbitrary offset means its state is unknown. A string
  literal ending in `*/` can therefore fool the tail path into misattributing a
  statement, and a `--` line comment opened before the window can make text
  inside it look like a tagged comment (fake tags; covered by the extract
  regress test). Such results are counted in `_info().heuristic_scans` so operators
  can see how often the inexact path is used.
- `position=any` does a full forward lexical scan.

The scanner uses the *current* `standard_conforming_strings` setting. The hooks
don't expose the setting that was in effect at parse time, so changing it
between PREPARE and EXECUTE can mis-scan plain strings that contain
backslashes. This is documented as a limitation.

### 6.3 Prepared statements carry stale comments
Comments are read from the source text saved at **Parse/PREPARE** time.
**Bind/Execute** and SQL `EXECUTE ... /*tags*/` supply no new statement text,
and the executor runs the saved source. If a client prepares once and executes
many times from different code paths, every execution is attributed to the
first caller's tags. This applies to named *and* unnamed statements: an unnamed
statement only gets fresh context if the driver sends a new Parse for each use.
pgss has the same limitation for query text.
**Mitigation:** document this clearly. Drivers and ORMs that include the
comment in their statement-cache key, or that re-Parse each time, are
unaffected. Before v1, check the behavior of the target drivers (Rails/PG,
pgx, JDBC, psycopg, and pgbouncer in transaction mode). If they reuse
prepared plans across contexts, move the session/transaction override
(`tags_override`, §8) into v1.

**Result (2026-10-05, item -27, `research/driver-prepared-statements/`):** no
target driver reuses a prepared statement across different comments. The
statement cache is keyed by SQL text that includes the comment, and pgbouncer
shares statements by query text. Tested with pgx 5.11, pgjdbc 42.7.13,
psycopg 3.3.6, ActiveRecord 7.0–8.1 with pg 1.7 and marginalia 1.11, and
pgbouncer 1.26. Native Rails `query_log_tags` (Rails 7.1+) disables prepared
statements. Stale context appears only when the application itself reuses one
prepared handle across requests. **Decision: `tags_override` stays on the
roadmap (no-go for v1).** Notes for the user docs:
- Each distinct comment value creates a separate prepared statement, so
  high-cardinality values in comments (such as request IDs) defeat statement
  caching.
- On Rails 8, marginalia does not annotate some ORM paths (`pick`, `find_by`);
  prefer `query_log_tags`.

### 6.4 Nested statements
PL/pgSQL and trigger bodies have their own `sourceText` (the function body),
where comments are code comments rather than request context.
**Mitigation:** with `nested_tags = inherit` (the default), a nested executor or
utility frame copies the tags of the *active* frame (§3.2). Utility statements
such as `CALL` and `DO` always create a frame so their children can inherit,
even when the utility itself is not recorded. This lets users answer "which
controller caused this trigger's query to run?", which is a useful feature in
its own right. Statements run during the *planning* of a parent, such as
constant-folded function calls, have no active frame. They get only their own
tags.

**Costs are inclusive.** With `track = all`, a parent's time already includes
its children's work. Executor instrumentation wraps Run and Finish, including
AFTER triggers. Summing parent and child `total_exec_time` therefore
double-counts. Per-application cost totals should filter on `toplevel` (§7).

### 6.5 Multi-statement query strings
`SELECT 1 /*a*/; SELECT 2 /*b*/` arrives as a single `sourceText`. Each
statement's range comes from `stmt_location`/`stmt_len`. The first statement
starts at byte 0, and each later one starts just after the preceding `;`
(PG14–17). **PG18** instead sets `stmt_location` to the statement's first
token, so leading comments fall *before* the range. The scanner therefore
extends a statement's owned range backwards over leading trivia, back to the
previous `;` token boundary or the string start, found lexically, never by a
naive search for `;`. A
`stmt_len` of 0 means "to the end of the string", and a location of -1 means
unknown, in which case the whole string is used, as in `CleanQuerytext`. A
comment after the final `;` lies outside every range. A hook only knows its
own statement's range.
**Mitigation:** extraction is statement-local. As a single fallback, a
statement whose own range has no tags may use a trailing comment after its
range, but only if the rest of the string contains nothing except `;`,
whitespace, and comments. That proves the statement is the last one. So in
`SELECT 1; SELECT 2; /*controller:x*/`, only `SELECT 2` gets the tags. Comments
are never attached to statements whose ranges don't own them.

### 6.6 Utility statement query IDs differ by version
- **PG14/15:** core sets a utility statement's `queryId` by hashing the
  **statement text** (`compute_utility_query_id` after `CleanQuerytext`), which
  includes comments. Every distinct tag set therefore produces a different query
  ID, and DDL fingerprints fragment.
- **PG16+:** utility statements are jumbled from the parse tree, so comments
  have no effect.

**Decision:** the `queryid` column always equals the core and pgss value on
every version, because it is the join key. pgss sees the same fragmented IDs on
PG14/15, so joins still work. The fragmentation is documented. A separate
comment-insensitive `utility_textid` column was considered and rejected
(decided 2026-10-05, §11 Q4): pgss has the same PG14/15 behavior, so it is not
worth the extra column.

### 6.7 `EXECUTE`/`PREPARE` and utility nesting
SQL `EXECUTE` goes through `ProcessUtility`, and then the executor runs the
prepared plan. Recording eligibility and nesting are separate decisions:
- **Recording** mirrors pgss so rows join one-to-one. `EXECUTE` and `PREPARE`
  are never recorded as utilities, because the executor records the underlying
  plan. `DEALLOCATE` is excluded on PG14–16 and recorded on PG17+, where pgss
  changed its list. That exclusion was not about double counting, since
  `DEALLOCATE` runs no plan.
- **Nesting:** `EXECUTE` and `PREPARE` do **not** increment `nesting_level`, so
  the plan run by `EXECUTE` still counts as top-level and `track = top` records
  it. All other utilities always activate a frame, even when
  `track_utility = off`, so `CALL`/`DO` children inherit tags. Nesting is a
  separate decision that mirrors pgss, so `toplevel` matches on every version:
  - PG17+: every other utility increments nesting.
  - PG14–16: nesting is incremented only when pgss itself would track the
    utility (its `track_utility`, its `track` at this level, its exclusion
    list, not in a parallel worker). pgss's GUCs
    (`pg_stat_statements.track_utility`, `pg_stat_statements.track`) are read by
    name on each utility statement. When pgss isn't loaded, or its GUCs are only
    placeholders, this extension's own `track`/`track_utility` are used instead
    (decided 2026-10-05, item -18).
  - Recording eligibility always follows this extension's own settings.
- A tracked utility that arrives with `queryId = 0` (wrong load order)
  increments `utility_missing_queryid`, regardless of the `untagged` policy.
  It also rises in the correct load order for a utility re-executed from a plan
  cache (a named extended-protocol statement, or a PL/pgSQL `CREATE`/`DROP`):
  pgss zeroes such a statement's `queryId` on its first execution, so pgss
  itself counts only that first execution per backend, and so do we. Parity
  holds; the counter shows the later executions (found in item -22).
- **PG18 boundary cache (§6.5):** every top-level statement of the client
  string that gets no frame (`PREPARE`, `EXECUTE`, untracked utilities, or
  statements skipped at `ExecutorStart`) still advances the boundary cache via
  `pssc_context_note_stmt_boundary()`, so later statements keep their leading
  comments (item 181131-1).
- `ProcessUtility` copies everything it needs before chaining and never reads
  `pstmt` afterwards, because `ROLLBACK` can free it.

### 6.8 Parallel workers
Parallel workers run executor hooks too.
**Mitigation:** skip when `IsParallelWorker()`, as pgss does, so each
statement is counted once. The leader's elapsed time already covers the
workers' execution.

### 6.9 Errors and cancellations
`ExecutorEnd` is not reached when a statement errors, so failed statements are
not counted. This is the same as pgss. Frames of failed executors are cleaned
up by their memory-context callback (§3.2). The active-frame and nesting
changes are wrapped in `PG_TRY`/`PG_FINALLY`, so they can't leak.
Per-tag-set error and cancellation counts were considered for the roadmap and
rejected (decided 2026-10-05): they are out of scope for a pgss companion, so
§11 Q6 (error deduplication rules) is moot.

### 6.10 Version-specific API differences (PG14–18)
| Area | Difference |
|------|------------|
| Shared memory request | PG14: `RequestAddinShmemSpace` in `_PG_init`. PG15+: `shmem_request_hook`. |
| `ExecutorRun` signature | PG14–17 take `bool execute_once`. PG18 removed it. |
| `ProcessUtility` signature | `readOnlyTree` parameter (PG14+). Check each major version for further changes. |
| `queryId` jumbling | PG16 moved to node-generated jumbling (utility statements are jumbled by node from PG16). PG18 squashes constant lists. |
| pgss utility handling | PG14–16 exclude `EXECUTE`/`PREPARE`/`DEALLOCATE` and bump nesting only for tracked utilities. PG17+ exclude only `EXECUTE`/`PREPARE` and bump nesting for all other utilities (§6.7). |
| compat macros | `PSSC_PGSS_RECORDS_DEALLOCATE` (1 on PG17+), `PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES` (1 on PG14–16). |
| GUC `extra` allocation | PG14/15: `malloc`, freed with `free()` (`guc_malloc` is static there). PG16+: `guc_malloc` in the GUC memory context (§4.2). |
| Regex allocator | PG14/15 `malloc`, PG16+ `palloc` in `CurrentMemoryContext` (§4.2). |

**Mitigation:** a `compat.h` with `PG_VERSION_NUM` macros, and a CI matrix that
builds and runs regression tests against every supported major version.

**Counter availability.** The stored counters, `calls` and `total_exec_time`,
exist on every supported version, so v1 needs no per-version counter shims, and
`compat.h` contains none (item 103941-1 removed the speculative ones). A future
column that needs a shim adds it in the same change, with a case in
`002_compat.pl`.
Row counts, buffer/WAL/I/O-timing and JIT fields, whose availability varies by
version (for example `shared_blk_read_time` in PG17), are not stored at all
(§5.1). General policy, following pgss: if a future column is unavailable on
some server version, it is omitted on that version rather than exposed as
`NULL`.

### 6.11 Security and privacy
- Tag values are untrusted client input and are never interpreted. Processing
  order:
  1. decode (URL/`\'`)
  2. reject values that contain NUL or are invalid in the database encoding
     (`pg_verify_mbstr`)
  3. apply the per-extractor `keys` allowlist, matching **original** key names
     (decided 2026-10-05)
  4. rename. The renamed key is checked against the database encoding, because
     the GUC is cluster-wide but databases may have different encodings
  5. apply the global allowlist (or the denylist when `tags = '*'`), and drop
     keys longer than 63 bytes
  6. *(roadmap)* value normalization: per-key regex-replace rules (§8)
  7. truncate on a character boundary (`pg_mbcliplen`)
  8. *(roadmap)* per-key cardinality caps, collapsing overflow values to
     JSON `null` (§8)
  9. sort and serialize within `max_tags` and `max_tagset_bytes`, using greedy
     fill (decided 2026-10-05):
     - Tags are considered in priority order: allowlist order, or sorted-key
       order when `tags = '*'`.
     - A tag is kept if it still fits within both `max_tags` and
       `max_tagset_bytes`; one that doesn't is dropped and counted in
       `dropped_tags`, and the next tag is tried.
     - So an oversized tag never evicts smaller lower-priority tags.

  Extractor-chain semantics:
  - An extractor "produces" if at least one pair survives steps 1–7, before
    the `max_tags`/`max_tagset_bytes` limits of step 9. If step 9 then drops
    every tag, skipped extractors are not retried, and the statement counts
    as untagged (found while reviewing the user docs, item -28).
  - Once one has produced, later non-`merge` extractors are skipped, but later
    `merge=on` extractors still run.
  - The first occurrence of a key wins: by chain order, then comment order,
    then pair order.
  - The trailing-footer fallback (§6.5) is used only when the statement's own
    range yields no tags.

  Tags from `tags_override` (roadmap, §8) go through the same steps except
  step 3, since no extractor is involved. `appname` (roadmap) is an extractor,
  so its tags follow every step.
  Malformed tags are dropped and counted in `_info().invalid_tags`. This
  includes:
  - NUL or invalid encoding
  - keys longer than 63 bytes, when `tags = '*'`. With an allowlist they can
    never match (allowlist keys are at most 63 bytes), so they are dropped
    silently like any other unlisted key
  - parser-malformed segments, counted only for a comment from which the same
    parser obtained at least one well-formed pair, so probing another format's
    comment isn't counted (decided 2026-10-05) They never raise an error in the user's
  statement.
- Tags are stored in the originating database's encoding, which is recorded per
  entry as pgss does, and converted with `pg_any_to_server` when read. For a
  `SQL_ASCII` origin, non-ASCII bytes are escaped on output instead of being
  converted: each byte ≥ 0x80 becomes `\xHH` (lowercase hex) and `\` becomes
  `\\`, in both keys and values, so the escaping is reversible and distinct
  keys stay distinct (decided 2026-10-05, item -11). All tag output goes through
  the shared helper `pssc_tags_push_jsonb()` (`src/tagout.c`); a tag set from
  a non-`SQL_ASCII` origin that can't be converted falls back to the same
  `\xHH` escaping for that whole entry, using the conversion's no-error mode
  (no `PG_TRY`), so the SRF never fails on one bad entry (decided 2026-10-05,
  item -20). In a `SQL_ASCII` server, tags from other encodings are validated
  and passed through unconverted, as `pg_any_to_server` does.
- Tags may contain PII, for example user emails in a route. Visibility is
  **at least as strict as pgss**. For rows owned by another role, both `queryid`
  and `tags` are `NULL` unless the caller has the privileges of
  `pg_read_all_stats`. The check runs inside the C SRF, so `showtags = false`
  doesn't bypass it. The check is `has_privs_of_role(GetUserId(), pg_read_all_stats)`
  on every version, which on PG14 is slightly stricter than pgss there
  (`is_member_of_role` also admitted NOINHERIT members). Future activity and exemplar views use the same rule.
- Regex patterns are superuser-only (GUC context). An input bound alone is not
  a CPU bound. v1 therefore also rejects back-references, caps the pattern
  length and capture count (§4.2), and caps the number of comments examined per
  statement (16) and their total size (`scan_window`). The core regex engine
  already checks for interrupts, which allows cancellation but does not bound
  complexity.

### 6.12 Deployment requirement
The extension must be listed in `shared_preload_libraries`, **after**
`pg_stat_statements` if both are used (§3.2), so enabling it requires a
restart. Managed providers (RDS, Cloud SQL, Azure) only allow
extensions on their allowlists, so users there cannot install it until a
provider adds it. This is the main obstacle to adoption.

## 7. SQL interface (v1)

```sql
CREATE FUNCTION pg_stat_statement_context(
    showtags boolean DEFAULT true,
    merge_buckets boolean DEFAULT false,
    OUT bucket_start timestamptz, OUT userid oid, OUT dbid oid,
    OUT queryid bigint, OUT toplevel bool, OUT tags jsonb,
    OUT calls bigint, OUT total_exec_time float8)
RETURNS SETOF record ...;

CREATE VIEW pg_stat_statement_context AS
    SELECT * FROM pg_stat_statement_context(true, false);

-- One row per (db, user, queryid, toplevel, tags) across live buckets;
-- bucket_start is the oldest contributing bucket.
CREATE VIEW pg_stat_statement_context_totals AS
    SELECT * FROM pg_stat_statement_context(true, true);

CREATE FUNCTION pg_stat_statement_context_reset() RETURNS void ...;
REVOKE ALL ON FUNCTION pg_stat_statement_context_reset() FROM PUBLIC;

CREATE FUNCTION pg_stat_statement_context_info(
    OUT entries bigint, OUT max_entries bigint, OUT dealloc bigint,
    OUT evicted_entries bigint,
    OUT buckets int, OUT oldest_bucket timestamptz, OUT shmem_bytes bigint,
    OUT invalid_tags bigint, OUT dropped_tags bigint,
    OUT heuristic_scans bigint, OUT regex_compile_failures bigint,
    OUT utility_missing_queryid bigint, OUT stats_reset timestamptz) ...;
```

Debug function (item -11, ships in 1.0):

```sql
CREATE FUNCTION pg_stat_statement_context_extract(
    query text, stmt_location int DEFAULT -1, stmt_len int DEFAULT 0)
RETURNS jsonb VOLATILE STRICT ...;
REVOKE ALL ON FUNCTION pg_stat_statement_context_extract(text, int, int) FROM PUBLIC;
```

- Runs the hooks' extraction pipeline with the current GUC config and returns
  `tags`, `ntags`, `tagset_bytes`, `footer`, `heuristic`, `oom`, `stmt_start`,
  `stmt_end` (byte offsets), and this call's `invalid_tags`, `dropped_tags`,
  `heuristic_scans`, `regex_compile_failures`.
- Restricted because it runs the regex engine on arbitrary input (CPU cost) and
  reveals the extractor configuration; superusers may `GRANT` it.
- Works when `enabled = off`; errors if the library isn't preloaded.
  `stmt_location = -1` means the whole string; out-of-range offsets error.
- Records nothing and doesn't touch pending stats, except regex compile
  failures, which are per-backend state and go to the shared counter.
- Statement ownership uses the hooks' `pssc_stmt_owned_range()` (same
  `scan_window` budget), so the result equals the hooks' for a single statement
  or the first statement of a string. Exception: for a later statement starting
  more than `scan_window` bytes in, the PG18 boundary cache (§6.5) can let the
  hooks see leading comments that this function doesn't.
- Output is escaped in `SQL_ASCII` databases (§6.11).

The column set is deliberately minimal (§5.1, decided 2026-10-05): `calls` and
`total_exec_time` only. Rows, buffers, WAL, I/O timing, JIT, and
min/max/mean/stddev come from `pg_stat_statements`, joined on
`(userid, dbid, queryid, toplevel)`. `tags` is `jsonb` (decided 2026-10-05,
§11 Q2), with string values (and `null` for values collapsed by the roadmap
cardinality caps, §8).

`_info()` columns added on 2026-10-05: `evicted_entries` (§5.3),
`dropped_tags` (tags dropped because the tag set would exceed `max_tags` or
`max_tagset_bytes`, §4.1), and `regex_compile_failures` (lazy-compile failures,
§4.2). Regex failures happen per backend, so they are flushed into a shared
counter in the header.

SRF implementation (item -20): materialize mode, `STRICT VOLATILE PARALLEL
SAFE`, C symbol `pg_stat_statement_context_1_0`. Under the shared lock only raw
bytes are copied (key fields, encoding, live slots, and tags only when shown);
encoding conversion, jsonb building and merging happen after the lock is
released. Reading changes no entry data: it may only advance the
`current_bucket` watermark (§5.2). Expired slots keep their contents until a
writer rolls them over. Non-merged rows of an entry come out in bucket order.

`_info()` and `_reset()` (item -21):
- `buckets` is the configured `bucket_count`. `oldest_bucket` is the start of
  the oldest live slot of any entry (it equals `min(bucket_start)` in the
  view), or `NULL` when no slot is live. All values come from one snapshot
  under the shared lock; `shmem_bytes` is the exact size requested at startup.
- `_info()` first flushes the caller's pending extraction counters, so a
  session sees its own activity. It is callable by `PUBLIC`, like
  `pg_stat_statements_info`.
- `_reset()` (superuser-only by default) takes the exclusive lock, clears all
  entries and header counters, sets `stats_reset`, and discards the caller's
  own pending counters. Counts from statements running elsewhere land after
  the reset. A backend whose regex failed before the reset keeps that
  extractor disabled and doesn't count the failure again.
- Diagnostic counter flushes hold the store's shared lock, so a flush lands
  entirely before or after a reset. When nothing is pending, no lock is taken.
  At `ExecutorEnd` the flush reuses the record's lock hold
  (`pssc_store_record_with_stats()`).
- Both functions are `VOLATILE PARALLEL RESTRICTED`, because pending counters
  live only in the leader backend.

Bucket merging (`merge_buckets = true`) sums `calls` and `total_exec_time`
across an entry's live slots (§5.2). Because the key has no bucket, each entry
yields exactly one merged row, and `bucket_start` is its oldest live slot.

Counter semantics: `calls` counts completed executor instances, meaning
`ExecutorEnd` was reached. That is not the number of Execute or FETCH messages.
`total_exec_time` is in milliseconds and is measured the same way pgss measures
it (`queryDesc->totaltime` for plannable statements, elapsed time around the
chained call for utilities).

Typical use, joined to `pg_stat_statements` for query text. Filtering on
`toplevel` avoids double-counting nested work (§6.4):

```sql
SELECT c.tags->>'controller' AS controller, c.tags->>'action' AS action,
       sum(c.calls) AS calls, sum(c.total_exec_time) AS ms,
       left(s.query, 80) AS query
FROM pg_stat_statement_context_totals c
LEFT JOIN pg_stat_statements s USING (userid, dbid, queryid, toplevel)
WHERE c.toplevel
GROUP BY 1, 2, s.query
ORDER BY ms DESC LIMIT 20;
```

Other pgss metrics can be apportioned to a context approximately by its share
of the statement's execution time, for example
`s.shared_blks_read * c.total_exec_time / nullif(s.total_exec_time, 0)`. This
is an estimate: it assumes the metric is proportional to time, and pgss
accumulates since its last reset, while this extension covers only the live
bucket window.

`toplevel` is only present in `pg_stat_statements` from PG14 onwards, which
matches this extension's minimum supported version.

## 8. Roadmap

**v1.x — hardening**
- **Per-key cardinality caps.** Values beyond a key's cap collapse to JSON
  `null` before the key is built (§6.11 step 8). `null` can't collide with a
  real value, since clients can only send strings. Adopted configuration
  (2026-10-05): a global default cap GUC plus optional per-key overrides, with
  distinct values counted globally per key (not per bucket or per `queryid`).
  Collapses are counted in `_info()`.
- **Exemplars:** store the most recent value of a high-cardinality key, such
  as `traceparent`, per entry, so users can jump from an aggregate to a real
  trace without the key exploding. Exemplar keys are an explicit list in a
  dedicated GUC; the `exclude_tags` denylist does not double as that list.
  Total exemplar storage is bounded by a configurable memory cap (decided
  2026-10-05).
- Optional background worker that reclaims dead entries (all slots expired)
  on idle systems. This is not needed for correctness, since readers filter
  expired slots (§5.2).
- **Persist stats across clean restarts** (dump/load like
  `pg_stat_statements.save`), following pgss's lead (decided 2026-10-05): the
  saved file is discarded on a file-format or extension-version mismatch; if
  `max_entries` shrank, load what fits and evict the rest (§5.3); if
  `bucket_interval` or `bucket_count` changed, discard the file (pgss has no
  bucket analogue). Slots that expired during the downtime are dropped.

**v2 — more context sources**
- **`tags_override`:** context from a session or transaction GUC, e.g.
  `SET LOCAL pg_stat_statement_context.tags_override = 'controller=''users'',action=''show'''`.
  The value uses sqlcommenter syntax (`k='v',k2='v2'`, URL-encoded values). It
  **merges** with comment tags, and the override wins on key conflicts.
  Override tags go through the §6.11 pipeline (rename, allowlist/denylist,
  truncation). This works with prepared statements and with drivers that can't
  add comments. It moves into v1 if driver validation (§6.3) shows prepared
  plans being reused across contexts.
- **Context from `application_name`:** a DSL extractor
  `appname(format=sqlcommenter|marginalia|regex)` (§4.2). Tags from comments
  win over `appname`-derived tags on key conflicts.
- A `pg_stat_statement_context_activity` view showing the **current** tags of
  each backend, as a context-aware companion to `pg_stat_activity`. It follows
  the same visibility rules as §6.11.
- **Value normalization rules:** per-key regex-replace rules, e.g.
  `/users/\d+` → `/users/:id`. They run after rename and the allowlist/denylist,
  and before truncation and cardinality caps (§6.11 step 6).

**v3 — ecosystem**
- **Integrations (done, item -42):** `docs/integrations/` ships recipes for
  sql_exporter, postgres_exporter (its custom queries are deprecated upstream)
  and the OTel Collector contrib `sql_query` receiver; the `postgresql` receiver
  cannot run custom queries. It also has a Grafana dashboard and a
  least-privilege monitoring role. `scripts/test-integrations.sh` tests them
  end to end in Docker. Buckets expire and entries are evicted, so the recipes
  export per-second gauges over the last closed bucket rather than Prometheus
  counters. Only the `_info()` counters are exported as `_total`. Selected tag
  keys become `tag_<key>` labels, and the dashboard defaults to
  `toplevel = true`. Monotonic counters and bucket metadata are proposed in
  20261006-010149-1.
- Packaging: PGXN, PGDG apt/yum, Homebrew, Docker images. Engage managed-cloud
  providers about adding the extension to their allowlists.
- Upstream conversation: propose a core hook or field for "statement comments"
  or a query-tag mechanism, which would benefit pgss and any similar
  extension.

**Rejected (2026-10-05).** Out of scope for a pgss companion, which stores only
`calls` and `total_exec_time` per context (§5.1):
- `track_planning` (planning time): pgss already tracks planning per `queryid`.
- `utility_textid` for PG14/15: pgss has the same fragmented utility IDs (§6.6).
- Error and cancellation counts per tag set: adds hook-exception machinery for
  a metric outside the companion scope (§6.9).
- Wait-event sampling attributed to tags: needs a sampling worker and a
  separate store; not a per-statement counter.
- OS-level CPU and I/O (`getrusage`) per tag set: that is `pg_stat_kcache`'s
  job.
- Extractor `config_file`: GUCs plus `ALTER SYSTEM`/`pg_reload_conf()` suffice
  (§4.1, §11 Q3).

## 9. Testing strategy

- **Unit-ish regression (`pg_regress`)**: comment scanner edge cases (nested
  comments, dollar quotes, `$` inside identifiers, `E''`/`U&''` strings,
  `standard_conforming_strings = off`, unterminated comments, multibyte text,
  multi-statement ranges and trailing footers) exposed through a debug SQL
  function such as `pg_stat_statement_context_extract(text)`. Also cover each
  extractor format, DSL and regex errors (including back-references), the
  allowlist and denylist, `%00` and invalid encodings, and truncation on
  character boundaries.
- **TAP tests**: restarts, SIGHUP reconfiguration, bucket rollover (short
  `bucket_interval` set at startup), parallel queries, and
  prepared statements over the extended protocol (`\bind` in psql 16+, or
  pgbench `-M prepared`). Lifecycle and adversarial cases:
  - overlapping and suspended portals, cursors that are never run or are
    closed early
  - SPI errors caught in PL/pgSQL followed by successful work, failed portals
  - utility statements that end the transaction (`ROLLBACK`, `COMMIT` in
    procedures)
  - both `shared_preload_libraries` orders, and pgss/extension `track` and
    `track_utility` settings that differ
  - nested `toplevel` parity with pgss per version
  - stale-bucket insertion across a rollover, per-entry slot rollover, clock
    steps
  - a forced hash collision (debug hash override) followed by eviction and
    reinsertion
  - small-`max_entries` churn, with dead entries reclaimed before live ones
  - cross-database encodings, including `SQL_ASCII`
  - visibility for unprivileged roles, and `REVOKE` on reset
- **pg_regress suite** (`make installcheck`: smoke, guc, extract) runs in a
  UTF8, no-locale database. Server-level GUCs are changed with `ALTER SYSTEM` +
  `pg_reload_conf()` and an include file that waits until the new values are
  visible. TAP 013 checks that `_extract()` leaves the store and counters
  unchanged, the `SQL_ASCII` escaping, and debug/hook parity. Regression
  output that contains characters whose psql display width varies between
  minor releases (for example emoji outside the last column) uses
  `\pset format unaligned`, which still compares the exact bytes.
- **Harness source builds** (`docker/Dockerfile.source`) install
  `pg_stat_statements` too, so pgss parity checks run on them. They install
  bison/flex (needed by PG17+ tarballs), download the release tarball and its
  official `.sha256` to files, and verify the checksum before extracting; any
  download or checksum failure fails the build.
- **pgss parity** (`017_lifecycle.pl`, item -22): with matching settings, the
  per-`(userid, dbid, queryid, toplevel)` `calls` equal pgss exactly.
  `total_exec_time` is exactly equal for plannable statements, which read the
  same `totaltime`. For utilities ours is at least pgss's, because our hook
  wraps theirs. Other behaviour it pins down:
  - A cursor left open is recorded as top level at transaction end, as pgss does.
  - A bound portal that is never executed counts one call.
  - With differing settings, each side records according to its own settings.
  - Releases before upstream `8700851352a8` (14.0–14.9, 15.0–15.4, 16.0)
    re-parse a cached utility statement when its saved search_path no longer
    matches. That happens, for example, after the session's first temp table,
    and the re-parsed statement gets a fresh queryId that both pgss and this
    extension count. The test detects this at runtime instead of checking minor
    versions.
- **Store and SQL surface through the hooks** (items -23/-24):
  `018_store_reconfig.pl` exercises restart/resize, SIGHUP reconfiguration,
  rollover, stale buckets, clock steps, eviction and pgbench stress through
  real statements, pinning the debug clock wherever short buckets would
  otherwise make reads racy. `019_sql_surface.pl` runs the SQL examples and
  column tables from `docs/sql-interface.md` and §7 against the catalog and
  pgss, so incompatible doc changes fail the tests.
- **CI matrix** (`.github/workflows/ci.yml`, on push/PR): the Linux cells run
  the local harness itself:
  - `scripts/docker-test.sh N` (PGDG, 14–18).
  - `--assert N`: a source build with `--enable-cassert --enable-tap-tests`,
    14–18.
  - `--valgrind 18`: the server runs under Valgrind with
    `src/tools/valgrind.supp` and `-DUSE_VALGRIND` for the LOAD checks and the
    pg_regress suite; any Valgrind error fails the cell.

  macOS builds 14–18 with `docker/build-postgres.sh` and runs
  `docker/run-tests.sh` on the host. Source builds are cached, keyed on
  release, flavor and a hash of the build scripts. A separate job runs
  `scripts/check-version-guards.sh`. TAP tests use the PG15+
  `PostgreSQL::Test::*` names; PG14 installs them as aliases only from 14.6, so
  14.0–14.5 cannot run the TAP suite and the harness says so explicitly.
- **Benchmarks**: `pgbench -S` with and without the extension, with and
  without comments, and with large `IN` lists. Report p99 and maximum latency at
  bucket boundaries and under sustained eviction, not just average throughput.
  Publish overhead numbers relative to `pg_stat_statements` alone.
- **Fuzzing**: a standalone libFuzzer harness for the pure-C comment scanner and
  the SQLCommenter/marginalia parsers. The regex extractor depends on backend
  allocators, `pg_wchar`, and collation code, so it is fuzzed through a
  backend-aware harness, such as a SQL-level fuzz driver against the debug
  function.

## 10. Repository layout (proposed)

```
pg_stat_statement_context/
├── Makefile / meson.build          # PGXS
├── pg_stat_statement_context.control
├── sql/pg_stat_statement_context--1.0.sql
├── src/
│   ├── pg_stat_statement_context.c # _PG_init, hooks
│   ├── compat.h                    # PG14–18 shims
│   ├── guc.c                       # GUCs + DSL parser
│   ├── scan.c                      # comment scanner (backend-independent)
│   ├── extract.c                   # sqlcommenter / marginalia / regex
│   ├── context.c                   # execution frames, active-frame tracking
│   └── store.c                     # shmem HTAB, buckets, eviction
├── test/{sql,expected,t}/          # pg_regress + TAP
├── fuzz/
└── DESIGN.md
```

## 11. Open questions

All seven questions below were resolved by the project owner on 2026-10-05.
They are kept, with their resolutions, for the record. Remaining undecided
points are tracked as open questions on individual tasks in `BACKLOG.md`.

1. ~~Should an empty tag set be recorded by default?~~ **Resolved: no.**
   Untagged statements are skipped by default (`untagged = skip`, §4.1), so
   untagged traffic doesn't consume entries. `untagged = record` remains
   available.
2. ~~`jsonb` for `tags`, or fixed columns for a configured set of keys?~~
   **Resolved: `jsonb`** (§7).
3. ~~Should the extractor DSL live in one GUC or in a separate config
   file?~~ **Resolved: GUCs only.** The from-SQL path is
   `ALTER SYSTEM SET ...; SELECT pg_reload_conf();` (§4.1). The `config_file`
   idea is rejected (§8).
4. ~~Is a separate `utility_textid` (§6.6) worth adding for PG14/15?~~
   **Resolved: not worth it**; pgss has the same PG14/15 behavior (§6.6, §8).
5. ~~Key layout: `bucket_id` in the key versus one entry per
   (query × context) holding a ring of per-bucket counters?~~
   **Resolved: per-entry counter ring** (§5.1–§5.4). With only `calls` and
   `total_exec_time` stored, the ring costs about 24 bytes per bucket, and the
   layout avoids re-inserting entries at every boundary and makes capacity
   count combinations.
6. ~~Error counting (§6.9): deduplication rules and separate cancellation
   counts?~~ **Moot:** per-tag-set error counts were dropped as out of scope
   (§6.9, §8).
7. ~~Should a load-order violation (§3.2) be a `WARNING` or disable utility
   tracking?~~ **Resolved: `WARNING` only** (§3.2).

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
