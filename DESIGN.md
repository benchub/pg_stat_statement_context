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
- Complement, not replace, `pg_stat_statements` (join on `queryid`).

### Non-goals (v1)

- Storing query text (use `pg_stat_statements` for that).
- Plan capture, histograms, wait-event sampling.
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
                                     │ HTAB: key(bucket, db, user, queryid, toplevel,      │
                                     │           canonical tag set) → counters             │
                                     │ ring of time buckets + per-bucket member lists,     │
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
4. **Shared store** (`store.c`): fixed-size shared hash table plus a ring of
   time buckets with per-bucket membership lists. It handles locking, expiry,
   and eviction.
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
| `shmem_startup_hook` | Create or attach the shared hash table and bucket ring. |
| `post_parse_analyze_hook` | Nothing in v1. |
| `ExecutorStart` | If enabled and `queryId != 0`: create the executor frame in `es_query_cxt` and resolve its tags eagerly (scan, or inherit from the active frame). Set up `queryDesc->totaltime`, as pgss does. |
| `ExecutorRun` / `ExecutorFinish` | Make this frame active and increment `nesting_level`. Restore both in `PG_FINALLY`. |
| `ExecutorEnd` | Accumulate counters into the store under the frame's key, then drop the frame. |
| `ProcessUtility` | Before chaining, snapshot `queryId`, statement bounds, and tags into a utility frame. Activate it (and bump nesting, except for `EXECUTE`/`PREPARE`) around the chained call. Record from the snapshot afterwards. Never touch `pstmt` after chaining (§6.7). |
| `planner_hook` (optional, `track_planning`) | Planning time, if wanted later. |

**Why the executor hooks, not `post_parse_analyze`?** Parse analysis is skipped
when a cached plan or prepared statement is re-executed, but the executor hooks
fire on every execution. Tags are resolved at `ExecutorStart` because children
must be able to inherit them while the parent is still running. Counters are
recorded at `ExecutorEnd`, where timing and row counts are final.

**Load order.** pgss saves the utility `queryId`, then sets `pstmt->queryId = 0`
**before** calling the next `ProcessUtility` hook. It does this whenever it is
enabled and `track_utility` is on, and it also warns that `pstmt` may be freed
by `ROLLBACK`. This extension's hook must therefore run outside pgss's:
`shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'`
(the library loaded last installs the outermost hook). `_PG_init` checks the
order in `shared_preload_libraries` and logs a `WARNING` if it is wrong. At
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
    store_record(frame.key, counters)     -- §5.4
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
| `pg_stat_statement_context.max_entries` | `10000` | postmaster | Total hash entries across all buckets. |
| `pg_stat_statement_context.bucket_count` | `12` | postmaster | Number of time buckets in the ring. |
| `pg_stat_statement_context.bucket_interval` | `300s` | postmaster | Width of each bucket. 12 × 5 min = 1 hour history. Fixed at startup so all backends agree on bucket IDs (§5.2). |
| `pg_stat_statement_context.max_tags` | `8` | postmaster | Max tags stored per entry. |
| `pg_stat_statement_context.max_tag_value_len` | `64` | postmaster | Bytes per tag value. Longer values are truncated on a character boundary. Keys are limited to 63 bytes, and longer keys are dropped. |
| `pg_stat_statement_context.max_tagset_bytes` | `512` | postmaster | Hard cap on the serialized tag set, which is part of the hash key (§5.1). Tags that don't fit are dropped in allowlist order and counted. |
| `pg_stat_statement_context.scan_window` | `2kB` | sighup | Max bytes from the head/tail searched for comments (see §6.2). |
| `pg_stat_statement_context.extractors` | `'sqlcommenter, marginalia'` | sighup | Extractor DSL (§4.2). |
| `pg_stat_statement_context.tags` | `'action, controller, job'` | sighup | Allowlist of tag keys to keep, applied after `rename`. Tags not listed are discarded. `'*'` keeps all tags (not recommended, see §6.1). |
| `pg_stat_statement_context.exclude_tags` | `'traceparent, tracestate, request_id'` | sighup | Denylist (high-cardinality). Only relevant when `tags = '*'`. |
| `pg_stat_statement_context.untagged` | `record` | sighup | `record` (empty tag set) / `skip` statements without tags. |

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
| `position` | `append` / `prepend` / `any` | Where the comment is expected. `append` = last comment before optional trailing `;`/whitespace. |
| `keys` | `a\|b\|c` | Per-extractor allowlist. |
| `rename` | `old:new\|...` | Normalize key names across formats (`controller` vs `route`). |
| `merge` | `on` / `off` | Union tags with earlier extractors instead of stopping. |

Format-specific parameters:

- **sqlcommenter**: `/*key='value',key2='value2'*/`. Values are URL-decoded
  and `\'` is unescaped. Parameters: `url_decode=on|off`.
- **marginalia**: `/*application:Foo,controller:users,action:show*/`. Splits
  on the first `kv_sep` only, because values like `line:app/models/u.rb:12`
  contain colons. Parameters: `kv_sep=':'`, `pair_sep=','`.
- **regex**: `regex(pattern='...', keys='k1|k2', position=any)`. Uses the core
  regex engine (`pg_regcomp`/`pg_regexec`) with `REG_ADVANCED` and the C
  collation (`C_COLLATION_OID`, which needs no catalog access). Capture group
  *n* maps to key *n*. Applied **only to comment text**, never to the full
  query. The engine works on `pg_wchar`, so comment bytes are converted first and
  capture offsets are mapped back to bytes. v1 limits: pattern ≤ 1 kB, captures
  ≤ `max_tags`, and patterns with back-references are rejected
  (`re_info & REG_UBACKREF`).

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
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\w+)\s+op=(\w+)'', keys=service|operation)'
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

Compiled regexes are not part of `extra`, because GUC frees only the top-level
block. Each backend compiles them lazily on first use after a generation change,
into a private memory context that it owns, and frees the old ones with
`pg_regfree`. Regex allocation uses `malloc` on PG14/15 and `palloc` on PG16+. If
lazy compilation fails (for example, out of memory), that extractor is disabled
for the backend and the failure is counted. The statement itself is not failed.

> **Alternative considered:** a config table (`pg_stat_statement_context.rules`).
> Rejected for v1 because reading catalogs from `ExecutorEnd` adds overhead,
> is database-local while the extension is cluster-wide, and gets complicated
> inside aborted transactions. GUCs are already reloadable, permissioned, and
> shown in `pg_settings`.

## 5. Storage and the rolling buffer

### 5.1 Key and entry

```c
typedef struct ctxKey {
    int64   bucket_id;     /* absolute bucket number, see §5.2 */
    Oid     dbid;
    Oid     userid;
    int64   queryid;
    bool    toplevel;
    uint16  tags_len;      /* bytes used in tags[] */
    uint32  tags_hash;     /* precomputed hash of tags[0..tags_len) */
    char    tags[FLEXIBLE];/* canonical "k\0v\0k\0v\0", max_tagset_bytes */
} ctxKey;

typedef struct ctxEntry {
    ctxKey   key;          /* keysize fixed at startup */
    slock_t  mutex;
    dlist_node bucket_link;/* membership list of its bucket (§5.2) */
    ctxCounters counters;  /* calls, total/min/max/mean/sum_var exec time, rows,
                              shared/local/temp blks hit/read/written, wal_*,
                              usage */
    int      encoding;     /* encoding of tags[] (that of dbid) */
} ctxEntry;
```

The **full canonical tag set is part of the key**, so distinct tag sets can
never be merged and no collision probing is needed. Like pgss, dynahash chains
entries and compares the actual keys. The table uses custom `HASH_FUNCTION` and
`HASH_COMPARE` callbacks. The hash combines the fixed fields with `tags_hash`.
The compare checks the fixed fields and `tags_len`, then runs `memcmp` on only
the used bytes. Keys are still built by `memset`-ing the whole key to zero first
(pgss does the same), so padding and unused tag bytes are always defined.

`keysize` and `entrysize` are computed at startup from `max_tagset_bytes`.
Shared memory is sized as `hash_estimate_size(max_entries, entrysize)` plus the
header and bucket ring, using `add_size`/`mul_size` overflow checks. The table
is created with `init_size = max_size = max_entries`, so all entries are
preallocated. `ShmemInitHash`'s `max_size` is only an estimate, not a limit, so
the `max_entries` cap is enforced by the extension under the exclusive lock.
With the defaults, an entry is on the order of 1 KB. `_info()` reports the exact
`shmem_bytes` value.

**Capacity is measured in (query × context) combinations × occupied buckets**,
not in combinations alone. For example, 5,000 recurring combinations active in
all 12 buckets need 60,000 entries. With `max_entries = 10000`, only about 833
combinations can be live in every bucket at once. The documentation should give
this sizing rule.

### 5.2 Time buckets

- `bucket_interval` is fixed at startup (postmaster GUC) and stored in the
  shared header along with an epoch. Every backend computes
  `bucket_id = floor((now - epoch) / interval)` as an `int64`, so all backends
  agree and stored IDs always map back to the right timestamps.
- The header holds `current_bucket`, which only changes under the exclusive
  lock. A writer whose computed ID is newer than `current_bucket` releases its
  shared lock, takes the exclusive lock, **re-checks** the header, and advances
  the ring if it is still behind. Advancing drops each expired bucket by walking
  only that bucket's membership list.
- The ID actually written is chosen while the lock is held. An ID older than the
  oldest live bucket (the backend stalled across a rollover) or newer than
  `current_bucket` (another backend has not yet advanced the ring) is clamped to
  `current_bucket`. Because the header is stable while any lock is held, no
  entry can be inserted into an expired bucket.
- If the wall clock moves backwards, `current_bucket` never decreases, and
  writes clamp to it. A forward jump larger than the ring expires everything.
  Bucket arithmetic is signed, so the cutoff can't underflow.
- Rollover happens lazily on the write path, so no background worker is needed.
  Readers do not depend on writers, because the SRF compares each entry to the
  clock-derived current bucket and hides expired entries even if nothing has
  advanced the ring.
- Semantics: an execution is attributed to the bucket in which its
  `ExecutorEnd` (or utility completion) runs. Its whole accumulated work goes
  there, even for a long-lived cursor that started much earlier. Buckets
  therefore show **completions per interval**, not work done per interval.

### 5.3 Eviction under pressure

If the table is at `max_entries`:

1. Drop the **oldest** live bucket in full, using its membership list.
2. If only the current bucket is left, evict its lowest-usage ~5% of entries,
   pgss-style. This sorts only that bucket's members, not the whole table.
3. Increment `dealloc` and `evicted_entries` counters exposed by `_info()`,
   so users can tell that `max_entries` is too small.

### 5.4 Locking

The locking is modeled on `pg_stat_statements`, but the bucketed workload has a
different miss pattern, so latency is measured rather than assumed (§9):

- One LWLock for the hash table. Shared mode is enough to look up an existing
  entry and update its counters under that entry's spinlock. Exclusive mode is
  only needed to insert, evict, or roll over a bucket.
- LWLocks cannot be upgraded. On a miss, the backend releases the shared lock,
  acquires the exclusive lock, and then re-validates everything. It re-checks
  the bucket (§5.2) and repeats the `HASH_ENTER` lookup, because another backend
  may have inserted the entry in the meantime. Only then does it evict, if
  needed, and insert.
- At every bucket boundary, each active combination misses once and inserts.
  This is a periodic burst of exclusive-lock acquisitions that a
  persistent-entry design like pgss does not have. Per-bucket membership lists
  keep rollover and eviction proportional to the affected bucket. A background
  worker would move this work off the query path but would not remove
  exclusive-lock stalls.
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
`max_tag_value_len` truncation, and `_info()` counters for evictions. Roadmap: per-key cardinality caps that
collapse overflow values to `<other>`, plus exemplar storage (§8).

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
  statement. Such results are counted in `_info().heuristic_scans` so operators
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

**Costs are inclusive.** With `track = all`, a parent's time and buffer usage
already include its children's work. Executor instrumentation wraps Run and
Finish, including AFTER triggers. Summing parent and child rows therefore
double-counts. Per-application cost totals should filter on `toplevel` (§7).

### 6.5 Multi-statement query strings
`SELECT 1 /*a*/; SELECT 2 /*b*/` arrives as a single `sourceText`. Each
statement's range comes from `stmt_location`/`stmt_len`. The first statement
starts at byte 0, and each later one starts just after the preceding `;`. A
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
PG14/15, so joins still work. The fragmentation is documented. If grouping
across tag sets is needed there, a separately named column (e.g.
`utility_textid`) can be added later. It would hash the statement with comments
removed by the lexer. Literal and quoted-identifier contents stay unchanged,
and whitespace is collapsed only outside tokens. It never replaces `queryid`
(§11).

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
  it. All other utilities increment nesting and activate a frame, even when
  `track_utility = off`. This matches pgss on PG17+. pgss on PG14–16 does not
  bump nesting for untracked utilities, so with pgss `track_utility = off` there,
  nested statements under `CALL`/`DO` may have a different `toplevel` value in
  the two views.
- `ProcessUtility` copies everything it needs before chaining and never reads
  `pstmt` afterwards, because `ROLLBACK` can free it.

### 6.8 Parallel workers
Parallel workers run executor hooks too.
**Mitigation:** skip when `IsParallelWorker()`, as pgss does. The leader's
instrumentation already includes worker buffer usage.

### 6.9 Errors and cancellations
`ExecutorEnd` is not reached when a statement errors, so failed statements are
not counted. This is the same as pgss. Frames of failed executors are cleaned
up by their memory-context callback (§3.2). The active-frame and nesting
changes are wrapped in `PG_TRY`/`PG_FINALLY`, so they can't leak.
**Roadmap:** count errors per tag set at the hook exception boundaries
(`PG_CATCH` in Run/Finish/ProcessUtility), while the frame is still known. The
error would be noted in backend-local memory and rethrown, then flushed to
shared memory later, for example from an abort callback. An error should be
attributed only to the innermost recorded frame, and errors caught inside
PL/pgSQL `EXCEPTION` blocks should not be counted against the outer statement.
Parse and planning errors happen before any frame exists and are out of scope.
`emit_log_hook` is *not* suitable as the counter source. It only sees messages
selected for the server log (so counts would depend on `log_min_messages`), it
runs after unwinding when the frame is already gone, and it never sees errors
that are caught internally.

### 6.10 Version-specific API differences (PG14–18)
| Area | Difference |
|------|------------|
| Shared memory request | PG14: `RequestAddinShmemSpace` in `_PG_init`. PG15+: `shmem_request_hook`. |
| `ExecutorRun` signature | PG14–17 take `bool execute_once`. PG18 removed it. |
| `ProcessUtility` signature | `readOnlyTree` parameter (PG14+). Check each major version for further changes. |
| `queryId` jumbling | PG16 moved to node-generated jumbling (utility statements are jumbled by node from PG16). PG18 squashes constant lists. |
| pgss utility handling | PG14–16 exclude `EXECUTE`/`PREPARE`/`DEALLOCATE` and bump nesting only for tracked utilities. PG17+ exclude only `EXECUTE`/`PREPARE` and bump nesting for all other utilities (§6.7). |
| Row counts | pgss uses `es_processed` (last `ExecutorRun` only) on PG14/15 and `es_total_processed` on PG16+. v1 mirrors this so `rows` matches pgss. |
| GUC `extra` allocation | PG14/15: `malloc`, freed with `free()` (`guc_malloc` is static there). PG16+: `guc_malloc` in the GUC memory context (§4.2). |
| Regex allocator | PG14/15 `malloc`, PG16+ `palloc` in `CurrentMemoryContext` (§4.2). |
| Buffer/WAL/JIT counters | Fields added across versions (e.g. `shared_blk_read_time` in PG17). |

**Mitigation:** a `compat.h` with `PG_VERSION_NUM` macros, and a CI matrix that
builds and runs regression tests against every supported major version.

### 6.11 Security and privacy
- Tag values are untrusted client input and are never interpreted. Processing
  order: decode (URL/`\'`), then reject values that contain NUL or are invalid
  in the database encoding (`pg_verify_mbstr`), then rename, then apply the
  allowlist, then truncate on a character boundary (`pg_mbcliplen`), then sort
  and serialize within `max_tagset_bytes`. Malformed tags are dropped and
  counted in `_info().invalid_tags`. They never raise an error in the user's
  statement.
- Tags are stored in the originating database's encoding, which is recorded per
  entry as pgss does, and converted with `pg_any_to_server` when read. For a
  `SQL_ASCII` origin, non-ASCII bytes are escaped on output instead of being
  converted.
- Tags may contain PII, for example user emails in a route. Visibility is
  **at least as strict as pgss**. For rows owned by another role, both `queryid`
  and `tags` are `NULL` unless the caller has the privileges of
  `pg_read_all_stats`. The check runs inside the C SRF, so `showtags = false`
  doesn't bypass it. Future activity and exemplar views use the same rule.
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
    OUT calls bigint, OUT total_exec_time float8, OUT min_exec_time float8,
    OUT max_exec_time float8, OUT mean_exec_time float8,
    OUT stddev_exec_time float8, OUT rows bigint,
    OUT shared_blks_hit bigint, OUT shared_blks_read bigint, ...)
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
    OUT buckets int, OUT oldest_bucket timestamptz, OUT shmem_bytes bigint,
    OUT invalid_tags bigint, OUT heuristic_scans bigint,
    OUT utility_missing_queryid bigint, OUT stats_reset timestamptz) ...;
```

Bucket merging happens in C because the statistics must be merged with
weights. Sums add up, `min`/`max` take the min of the mins and the max of the
maxes, and the mean is weighted by calls. Variance is pooled from the per-bucket
`sum_var` (Chan et al.):
`M2 = M2a + M2b + δ²·na·nb/(na+nb)`. Averaging per-bucket means would be wrong.
For example, one 100 ms call plus 100 one-ms calls has a mean of about 1.98 ms,
not 50.5 ms.

Counter semantics: `calls` counts completed executor instances, meaning
`ExecutorEnd` was reached. That is not the number of Execute or FETCH messages.
`rows` matches pgss on each version (§6.10).

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

`toplevel` is only present in `pg_stat_statements` from PG14 onwards, which
matches this extension's minimum supported version.

## 8. Roadmap

**v1.x — hardening**
- Per-key cardinality caps that collapse overflow values to `<other>`.
- **Exemplars:** keep the last-seen value of excluded high-cardinality keys
  such as `traceparent` per entry, so users can jump from an aggregate to a
  real trace without the key exploding.
- Optional background worker for bucket rollover on idle systems. This is not
  needed for correctness, since readers filter expired buckets (§5.2).
- Persist stats across clean restarts (dump/load like `pg_stat_statements.save`).
- `track_planning` support (planning time).
- Optional `utility_textid` column for comment-insensitive utility grouping on
  PG14/15 (§6.6).

**v2 — more context sources**
- Context from a session or transaction GUC, e.g.
  `SET LOCAL pg_stat_statement_context.tags_override = 'controller=users'`. This works
  with prepared statements and with drivers that can't add comments. It moves
  into v1 if driver validation (§6.3) shows prepared plans being reused across
  contexts.
- Context from `application_name` parsing.
- A `pg_stat_statement_context_activity` view showing the **current** tags of
  each backend, as a context-aware companion to `pg_stat_activity`. It follows
  the same visibility rules as §6.11.
- Error and cancellation counts per tag set, captured at hook exception
  boundaries (§6.9). An `emit_log_hook` integration might be added only as an
  optional way to enrich server log lines with tags.
- Value normalization rules, e.g. regex rewrite `/users/\d+` → `/users/:id`.

**v3 — ecosystem**
- Prometheus or OpenTelemetry exporter recipes (postgres_exporter queries, OTel
  Collector `postgresql` receiver config).
- Grafana dashboard.
- Wait-event sampling attributed to tags, via a background worker sampling
  backends together with the activity view.
- CPU and I/O at the OS level (`getrusage`, as in `pg_stat_kcache`) per tag set.
- Packaging: PGXN, PGDG apt/yum, Homebrew, Docker images. Engage managed-cloud
  providers about adding the extension to their allowlists.
- Upstream conversation: propose a core hook or field for "statement comments"
  or a query-tag mechanism, which would benefit pgss and any similar
  extension.

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
  - stale-bucket insertion across a rollover, clock steps
  - a forced hash collision (debug hash override) followed by eviction and
    reinsertion
  - small-`max_entries` churn
  - cross-database encodings, including `SQL_ASCII`
  - visibility for unprivileged roles, and `REVOKE` on reset
- **CI matrix**: PG14–18 × {Linux, macOS}, plus a Valgrind and
  `-DUSE_ASSERT_CHECKING` build.
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

1. Should an empty tag set be recorded by default? Doing so gives a complete
   picture, but untagged traffic can then take up a large share of the entries.
2. `jsonb` for `tags`, or fixed columns for a configured set of keys? `jsonb`
   is more flexible, while fixed columns are faster to filter and simpler to
   index in an exporter.
3. Should the extractor DSL live in one GUC or in a separate config file
   (`pg_stat_statement_context.config_file`) for complex setups?
4. Is a separate `utility_textid` (§6.6) worth adding for PG14/15, given that
   `queryid` must stay equal to core/pgss?
5. Key layout: `bucket_id` in the key (v1, simple) versus one entry per
   (query × context) holding a small ring of per-bucket counters. The ring
   layout avoids re-inserting entries at every boundary and makes capacity
   count combinations, but it costs `bucket_count`× counter space even for
   sparse entries.
6. Error counting (§6.9): what exact rules should deduplicate nested and
   subtransaction-caught errors, and should cancellations be separate from
   errors?
7. Should a load-order violation (§3.2) be a `WARNING` (v1) or should it
   disable utility tracking until the order is fixed?

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
