# pg_stat_statement_context — Design Document

> Status: Draft / proposal
> Target: PostgreSQL 14, 15, 16, 17, 18

## 1. Summary

`pg_stat_statement_context` is a PostgreSQL extension that attributes query execution statistics to **application context** carried in SQL comments, such as those emitted by [marginalia], Rails `query_log_tags`, and [SQLCommenter].

Where `pg_stat_statements` answers *"which query fingerprints are expensive?"*, this extension answers *"which parts of my application are running query fingerprint X, how often, and at what cost?"*

```
 queryid  | controller | action |  route        | calls | total_exec_time
----------+------------+--------+---------------+-------+-----------------
 -8812... | users      | show   | /users/:id    | 41022 |        1833.20
 -8812... | admin/users| index  | /admin/users  |   210 |         912.75
```

### Goals

- Aggregate per **(query fingerprint × extracted tag set)** in shared memory.
- Low, predictable overhead on the hot path; no extra SQL parse.
- Declarative configuration for which tags to extract, from which comment format, and where in the query to look.
- Bounded memory with a rolling, time-bucketed history.
- Track DML/SELECT **and** utility statements (DDL, etc.).
- Support PostgreSQL 14+ from a single source tree.
- Complement, not replace, `pg_stat_statements` (join on `queryid`). The extension is a **companion** to pgss: per (query × context) it stores only `calls` and `total_exec_time` (§5.1, decided 2026-10-05).

### Non-goals (v1)

- Storing query text (use `pg_stat_statements` for that).
- Storing any per-statement counter other than `calls` and `total_exec_time` (rows, buffers, WAL, I/O timing, JIT, min/max/mean/stddev, planning time). pgss already tracks these per `queryid`; join to it (§7).
- Plan capture, histograms, wait-event sampling, OS-level resource usage, error counts (§8 "Rejected").
- Being a general-purpose `pg_stat_statements` replacement (that is what `pg_stat_monitor` is).

## 2. Background: why this needs an extension

The core lexer (`src/backend/parser/scan.l`) discards comments the same way it discards whitespace. They never become tokens and are absent from both the raw parse tree and the analyzed `Query`. However, **every hook that runs after parsing still has access to the original source string**:

| Hook | Source text | Query ID |
|------|-------------|----------|
| `post_parse_analyze_hook` | `pstate->p_sourcetext` | `query->queryId` |
| `ExecutorStart/End_hook` | `queryDesc->sourceText` | `queryDesc->plannedstmt->queryId` |
| `ProcessUtility_hook` | `queryString` argument | `pstmt->queryId` |

So the design does **not** hook in before parsing and does **not** parse twice. Postgres parses once; the extension does a cheap lexical scan of the raw string to locate comments, then extracts tags from only those bytes.

Since PG14, core computes `queryId` itself (`compute_query_id`). The extension calls `EnableQueryId()` in `_PG_init`, which makes `compute_query_id = auto` behave as `on`. Fingerprints are therefore identical to those in `pg_stat_statements`, so the two views can be joined. The extension never replaces or rewrites the core `queryId`. This includes PG14/15 utility statements (§6.6).

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

1. **Comment scanner** (`scan.c`): a single-pass state machine that finds comment regions in a `const char *`, following `scan.l`'s lexical rules. It correctly skips `'strings'`, including escape semantics when `standard_conforming_strings = off`. It also skips `E''` and `U&''` strings, `"quoted identifiers"`, `$tag$dollar quotes$tag$`, and `$1` parameters. A `$` inside an identifier (`a$b$`) does not start a dollar quote. It handles nested `/* /* */ */` and `--` line comments. Output: a small array of `(offset, len)` comment spans. No allocation in the common case.
2. **Extractors** (`extract.c`): built-in parsers for SQLCommenter and marginalia formats, plus an optional regex extractor. They turn comment spans into a canonical **tag set**: decoded, validated, and truncated tag pairs sorted by key, serialized within a bounded size (§5.1, §6.11).
3. **Execution frames** (`context.c`): backend-local state that is owned per executor instance (`QueryDesc`) and per utility call. Each frame holds the resolved tag set and statement metadata. A backend-local *active frame* pointer is set only while a hook is executing (§3.3), so nested statements (PL/pgSQL, triggers, SPI) can inherit their parent's tags.
4. **Shared store** (`store.c`): fixed-size shared hash table with one entry per (query × context). Each entry holds a small ring of per-bucket counters (§5.1, §5.2). It handles locking, expiry, and eviction.
5. **SQL interface** (`pg_stat_statement_context--1.0.sql`): set-returning function, views, reset function, and info function.
6. **Config** (`guc.c`): GUC definitions. The `check_hook` parses and fully validates the extractor DSL, including regex syntax, and returns a flat, immutable `extra` blob. The `assign_hook` only installs that pointer and bumps a config generation, so it cannot fail (§4.2).

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

No `planner_hook` is used for timing: planning time is left to pgss (`track_planning`), see §8 "Rejected". On PG17+ only, a minimal `planner_hook` adds one nesting level around the chained planner and restores it in `PG_FINALLY`. It does no timing and activates no frame, so SQL run during planning (constant-folded functions) is not top-level, matching pgss, which counts planner nesting only from PG17.0. On PG14–16 pgss ignores planning when deciding `toplevel`, so no hook is installed there (decided 2026-10-05, item -16).

Frame details (item -16):
- **User and nesting refresh:** `pssc_frame_refresh()` updates `userid`, nesting level, `toplevel` and recordability at recording time (ExecutorEnd, or after a utility returns), because pgss reads them then. For example, a cursor closed under a different role is recorded under the closing role.
- **PG18 statement-boundary cache (§6.5):** used and updated only when the statement's source is the client query string (`debug_query_string`), at nesting level 0, with no active frame. Planning-time, nested and `EXECUTE`'d sources neither read nor overwrite it.
- **Statements that get no frame:**
  - `DECLARE CURSOR`'s inner query has `queryId` 0.
  - PL/pgSQL simple expressions (`x := expr`) skip the executor.
- **PL/pgSQL `INTO`:** PL/pgSQL blanks out only the `INTO target` clause, so a comment anywhere else in the statement, including after `INTO`, is seen (verified on PG14 and PG17 in item -28; corrects an earlier note). PL/pgSQL does drop a comment that ends a `PERFORM` statement or an expression such as `RETURN (SELECT ...)`.

Executor hooks (item -17):
- **`totaltime`:** allocated exactly as pgss does (14–18): `InstrAlloc(1, INSTRUMENT_ALL, false)` in `es_query_cxt`, only when it is still NULL and the statement is tracked at the current level. It uses `INSTRUMENT_ALL` rather than a bare timer because whichever hook allocates first decides what pgss gets.
- **Recording rule:** record at `ExecutorEnd` when a frame exists, it is recordable after `pssc_frame_refresh()`, and `totaltime` is set. This is pgss's rule plus the `untagged` policy.
- **Stats flush:** backend-local extraction counters are flushed into the shared header on every `ExecutorEnd`, before chaining.
- **Load order:** the executor hooks work with pgss loaded before or after this extension; only the utility hook (-18) depends on the order.
- **Plain `EXPLAIN`:** the inner statement is nested under the `EXPLAIN` utility (item -18), matching pgss.

**Why the executor hooks, not `post_parse_analyze`?** Parse analysis is skipped when a cached plan or prepared statement is re-executed, but the executor hooks fire on every execution. Tags are resolved at `ExecutorStart` because children must be able to inherit them while the parent is still running. Counters are recorded at `ExecutorEnd`, where the timing is final.

**Load order.** pgss saves the utility `queryId`, then sets `pstmt->queryId = 0` **before** calling the next `ProcessUtility` hook. It does this whenever it is enabled and `track_utility` is on, and it also warns that `pstmt` may be freed by `ROLLBACK`. This extension's hook must therefore run outside pgss's: `shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'` (the library loaded last installs the outermost hook). `_PG_init` checks the order in `shared_preload_libraries` and logs a `WARNING` if it is wrong. (Item -19: the list is parsed with `SplitDirectoriesString`, as the postmaster does; entries match by basename, case-insensitively on every platform, ignoring `.so`/`.dylib`/`.dll`/`.sl`; only each library's first entry counts; the check is skipped when `IsUnderPostmaster`, so EXEC_BACKEND children don't repeat it. The HINT reads `shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'`.) The warning is the only action: utility tracking is not disabled (decided 2026-10-05, §11 Q7). At runtime, utility calls that arrive with `queryId = 0` are counted in `_info().utility_missing_queryid` rather than being recorded. The extension itself never modifies `pstmt->queryId`, because pgss inside it depends on that value.

**Frame lifetime.** A frame is not pushed at Start and popped at End. An extended-protocol Bind calls `ExecutorStart` via `PortalStart`, suspended portals interleave, and `PortalCleanup` skips `ExecutorEnd` for failed portals. Frames are allocated in the executor's `es_query_cxt` and registered in a small backend-local list. A `MemoryContextCallback` unlinks the frame when that context is destroyed, which also covers abort paths. The active-frame pointer is only ever saved and restored around hook calls.

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
  refresh userid, level; recap tags if userid changed (role-scoped caps, §6.1)
  if frame recordable (track, toplevel, untagged policy):
    store_record(frame.key, 1 call, totaltime)   -- §5.4
  chain
```

There is no cross-execution memoization in v1. Extraction runs once per executor instance, and a portal fetched repeatedly is a single instance (one Start, many Runs, one End). Any future cache would need to key on source content, statement bounds, config generation, and lexical settings, and would need explicit ownership. A raw `sourceText` pointer is borrowed from the portal or plan cache and can dangle.

## 4. Configuration

All configuration uses GUCs, so it can be set in `postgresql.conf`, with `ALTER SYSTEM`, and is reloadable on SIGHUP unless marked otherwise. Settings that size shared memory require a restart. `superuser` settings can also be set per database, per role or per session (`ALTER DATABASE/ROLE ... SET`, `SET`, a function's `SET` clause) by a superuser or, on PG 15+, a role granted `SET` on them.

### 4.1 Core GUCs

| GUC | Default | Context | Description |
|-----|---------|---------|-------------|
| `pg_stat_statement_context.bucket_count` | `12` | postmaster | Number of time buckets (slots in each entry's counter ring). |
| `pg_stat_statement_context.bucket_interval` | `300s` | postmaster | Width of each bucket. 12 × 5 min = 1 hour history. Fixed at startup so all backends agree on bucket IDs (§5.2). |
| `pg_stat_statement_context.cardinality_cap` | `0` | sighup | Default cap on distinct values per kept key, counted per `cardinality_cap_scope` (item -32, §6.1). `0` = off; range 0..1000000. Once a key has had its cap of values, any other value is stored as JSON `null`. |
| `pg_stat_statement_context.cardinality_cap_overrides` | `''` | sighup | Per-key caps `key:N[, key:N…]` that take precedence over `cardinality_cap` (the key is everything before the last `:`, matched after `rename`). `N = 0` exempts the key; an override applies even when the default is 0. |
| `pg_stat_statement_context.cardinality_cap_scope` | `'role'` | postmaster | What the caps count per (item 20261007-070036-1, §6.1, §6.11): `role` = per (userid, dbid), the pgss entry key; `database` = per dbid; `server` = server-wide. `database` and `server` let roles observe and use up each other's caps. |
| `pg_stat_statement_context.cardinality_cap_slots` | `16384` | postmaster | Value slots in the shared cap-tracking table (about 9 bytes each, plus key slots). Always allocated, so caps can be turned on by a reload. One table for all scopes: narrower scopes fill it faster. |
| `pg_stat_statement_context.enabled` | `on` | superuser | Master switch. |
| `pg_stat_statement_context.exclude_tags` | `'traceparent, tracestate, request_id'` | superuser | Denylist (high-cardinality). Only relevant when `tags = '*'`. |
| `pg_stat_statement_context.exemplar_keys` | `''` | postmaster | Keys whose most recent value is stored with each entry as an exemplar (item -33, §6.13), e.g. `'traceparent'`. Comma-separated, case-sensitive, at most 8 keys of at most 63 bytes, no `*`. Matched after `rename`, before the allowlist/denylist, so a key may be both in `exclude_tags` (not grouped by) and here. Empty (default): off, no memory. |
| `pg_stat_statement_context.exemplar_memory` | `2MB` | postmaster | Shared memory for the exemplar values of all entries (range 0 – 2^31-1 kB, unit kB). Split evenly: each entry gets `exemplar_memory / max_entries` bytes and each key an equal share of that, minus 2 bytes of length, at most 256 bytes per value (`_info().exemplar_value_bytes`). Longer values are dropped and counted (§6.13). |
| `pg_stat_statement_context.extractors` | `'sqlcommenter, marginalia'` | sighup | Extractor DSL (§4.2). |
| `pg_stat_statement_context.max_entries` | `10000` | postmaster | Max (query × context) combinations. Each entry holds a counter ring of `bucket_count` slots, so this is independent of `bucket_count` (§5.1). |
| `pg_stat_statement_context.max_tag_value_len` | `64` | postmaster | Bytes per tag value. Longer values are truncated on a character boundary. Keys are limited to 63 bytes, and longer keys are dropped. |
| `pg_stat_statement_context.max_tags` | `8` | postmaster | Max tags stored per entry. |
| `pg_stat_statement_context.max_tagset_bytes` | `512` | postmaster | Hard cap on the serialized tag set, which is part of the hash key (§5.1). Tags are kept greedily in priority order (allowlist order, or sorted keys for `'*'`); a tag that doesn't fit is dropped and counted, and smaller lower-priority tags may still be kept (§6.11). |
| `pg_stat_statement_context.nested_tags` | `inherit` | superuser | `inherit` (use top-level tags) / `scan` (scan nested source) / `none`. |
| `pg_stat_statement_context.normalize` | `''` | sighup | Per-key value rewrite rules `key: 'pattern' => 'replacement', …` (item -41). Rules apply in order, each like `regexp_replace(v COLLATE "C", p, r, 'g')`. Patterns may not contain back-references; replacements may use `\1`–`\9`, `\&`, `\\`. Limits: at most 32 rules, 1 kB per pattern or replacement. Validated at SET/reload (§6.11 step 6). |
| `pg_stat_statement_context.reclaim_worker` | `off` | postmaster | Start the background worker that reclaims dead entries on idle systems (§5.3, item 20261005-091225-34). Off: no worker is registered. |
| `pg_stat_statement_context.reclaim_worker_interval` | `10s` | sighup | How often the reclaim worker wakes up (100 ms – 1 day, unit ms). |
| `pg_stat_statement_context.save` | `on` | sighup | Save the store at a clean shutdown and load it at the next start (§5.5, item 20261005-091225-35), as `pg_stat_statements.save` (same default and context). Off: nothing is saved, and a saved file found at startup is discarded. |
| `pg_stat_statement_context.scan_window` | `2kB` | superuser | Max bytes from the head/tail searched for comments (see §6.2). |
| `pg_stat_statement_context.tags` | `'action, controller, job'` | superuser | Allowlist of tag keys to keep, applied after `rename`. Tags not listed are discarded. `'*'` keeps all tags (not recommended, see §6.1). |
| `pg_stat_statement_context.track` | `top` | superuser | `none` / `top` / `all`, as in pgss. |
| `pg_stat_statement_context.track_utility` | `on` | superuser | Record utility/DDL statements. |
| `pg_stat_statement_context.untagged` | `skip` | superuser | `skip` statements without tags (default, decided 2026-10-05, §11 Q1) / `record` them with an empty tag set. |

The configuration lives in GUCs only; there is no separate config file (decided 2026-10-05, §11 Q3). To change it from SQL, use `ALTER SYSTEM SET pg_stat_statement_context.extractors = '...';` followed by `SELECT pg_reload_conf();`. This works for every `sighup` and `superuser` setting, including `extractors`, `tags`, and `exclude_tags`.

`untagged`, `tags`, `exclude_tags` and `scan_window` are `superuser` rather than `sighup` (item 20261007-133120-1, decided 2026-10-07), so they can differ per database or role, e.g. `ALTER DATABASE canvas SET pg_stat_statement_context.untagged = 'record'`. Each backend reads them when it extracts tags, and entries are keyed by `dbid` and `userid`, so differing values never share an entry. Because SET LOCAL, a function's `SET` clause and a (sub)transaction abort can change `tags`/`exclude_tags` in the middle of a statement, nothing outside one extraction call keeps a pointer to their parsed lists: a frame's tag set and recap candidates (§6.1) are copies, and the GUC machinery frees a replaced list only when no GUC stack level refers to it. Inherited tags (`nested_tags = inherit`) and recaps are not re-filtered by a changed allowlist: they were filtered when the statement that supplied them was extracted. A change bumps the backend's config generation (the `appname`/`tags_override` caches), but not the regex generation, so compiled regexes are kept; only `extractors` and `normalize` changes recompile them. Parallel workers do not extract or record, and get the leader's values from PostgreSQL's GUC state. `extractors`, `normalize` and the cap settings stay `sighup` (mid-statement changes to compiled regexes and caps would need more work), as do the server-wide `save` and `reclaim_worker_interval`.

### 4.2 Extractor DSL

A comma-separated list of extractors, each with optional parameters. The first extractor that produces at least one tag wins, unless `merge=on` is set.

```
extractor   := name [ '(' param { ',' param } ')' ]
name        := 'sqlcommenter' | 'marginalia' | 'regex'
param       := key '=' value
```

Common parameters:

| Param | Values | Meaning |
|-------|--------|---------|
| `keys` | `a\|b\|c` | Per-extractor allowlist. It matches the **original** key names as they appear in the comment, and is applied before `rename` (decided 2026-10-05, §6.11). |
| `merge` | `on` / `off` | Union tags with earlier extractors instead of stopping. |
| `position` | `append` / `prepend` / `any` | Where the comment is expected. `append` = the trailing run of comments (the last comment plus any immediately preceding comments separated only by whitespace) before optional trailing `;`/whitespace; `prepend` = the leading run of comments before the first token. A run, not a single comment, so that marginalia's `with_annotation` comment after the context comment doesn't hide it (decided 2026-10-05). The extractor chain decides which comments in the run to parse. Default when omitted: `append` for `sqlcommenter` and `marginalia`, `any` for `regex` (decided 2026-10-05). |
| `rename` | `old:new\|...` | Normalize key names across formats (`controller` vs `route`). |

Format-specific parameters:

- **sqlcommenter**: `/*key='value',key2='value2'*/`. Values are URL-decoded and `\'` is unescaped. Parameters: `url_decode=on|off`. With `url_decode=on`, keys and values are both decoded. `%XX` is decoded and a raw `+` becomes a space, so Go and Java emitters (form encoding) and Python and Node emitters (`%20`) give the same value; `%2B` is a literal `+`. An invalid `%` escape is kept literally and flagged. Only `\'` and `\\` are unescaped. Values must be single-quoted, and a `,` inside quotes doesn't split.
- **marginalia**: `/*application:Foo,controller:users,action:show*/`. Splits on the first `kv_sep` only, because values like `line:app/models/u.rb:12` contain colons. Parameters: `kv_sep=':'`, `pair_sep=','`. Pairs are split first, so `pair_sep` wins when the separators overlap. No decoding is done.
- **Both parsers:** ASCII whitespace is trimmed around the body, each pair, and each key and value. Empty segments are ignored. A segment without a separator, or a key that contains whitespace, is malformed: it is skipped and counted. That keeps free-text annotations such as marginalia's `with_annotation` from turning into tags. Decoded NUL bytes are flagged and rejected by the pipeline (§6.11). The parsers live in `src/pairs.c`, have no backend dependencies and never allocate (decided 2026-10-05).
- **regex**: `regex(pattern='...', keys='k1|k2', position=any)`. Uses the core regex engine (`pg_regcomp`/`pg_regexec`) with `REG_ADVANCED` and the C collation (`C_COLLATION_OID`, which needs no catalog access). Capture group *n* maps to key *n*. Applied **only to comment text**, never to the full query. The engine works on `pg_wchar`, so comment bytes are converted first and capture offsets are mapped back to bytes. v1 limits: pattern ≤ 1 kB, captures ≤ `max_tags`, and patterns with back-references are rejected (`re_info & REG_UBACKREF`).

  Matching rules (decided 2026-10-05, item -10):
  - Every non-overlapping match in the comment body is used, as with `regexp_matches(..., 'g')`. After an empty match the search moves on one character, and `^` anchors only at the start of the body.
  - An unmatched optional group produces no pair.
  - The first occurrence of a key wins, and matching stops once every key has a value.
  - A comment with invalid encoding or a NUL byte produces no pairs.
  - A pattern that is invalid in the database's encoding counts as a compile failure.

  Engine errors:
  - Engine calls run in `PG_TRY` without a subtransaction, because the engine holds only memory.
  - Cancel, timeout, shutdown, deadlock and serialization errors are re-thrown.
  - Other errors (e.g. OOM) disable the extractor for the backend: a compile error until the next `extractors` or `normalize` change, counted in `regex_compile_failures`. A match error simply yields no further pairs and is not counted.

  Compile time (item 20261006-021334-1): compiling a pattern (regex extractor or `normalize` rule) is limited to 100 ms (`PSSC_REGEX_COMPILE_LIMIT_MS`, a constant). The fuzzer found patterns such as `((?:(?:$)|\Zda|(?<!1)|\S){0,255}` that take tens of seconds.
  - The GUC check hooks reject a slower pattern with an errdetail only when a statement sets the value: `ALTER SYSTEM`, detected by a flag the ProcessUtility hook holds around `AlterSystemStmt` (it validates with `PGC_S_FILE`, like a reload), or a source of `PGC_S_SESSION`/`PGC_S_TEST` (unreachable today: both GUCs are `PGC_SIGHUP`). There the test compile is aborted at the limit.
  - Values read from the configuration file (startup, reload, `pg_file_settings`) are never rejected for time, so that all processes agree on the configuration (item 20261006-092320-1: under VM steal time a backend's reload check could reject a value the postmaster had accepted and silently keep stale extractors). The postmaster has no timer; it compiles to completion and logs an over-limit compile (a hand-edited `postgresql.conf` can still stall a reload). A backend's reload check stays bounded by its timer, and on timeout it accepts the value unchecked (it still rejects more keys than `max_tags`); its own run-time compile then re-checks group counts, back-references and time, and disables the extractor or rule in that backend (`regex_compile_failures`).
  - Parallel workers restoring the leader's settings don't test-compile at all, since they never extract tags (`_extract()` is `PARALLEL RESTRICTED` so it never runs in one).
  - A client backend's lazy compile runs under a `USER_TIMEOUT` that raises a cancel internally; the engine notices it (`REG_CANCEL` on PG14/15, a thrown cancel on PG16+). It counts as a compile failure (`regex_compile_failures`, extractor or rule disabled for the backend), never an error for the statement.
  - Genuine cancels still propagate: while the limit is armed, wrappers on SIGINT and SIGUSR1 (the PG14–16 recovery-conflict path, the only other setter of `QueryCancelPending`) record a foreign cancel; SIGALRM is blocked inside the SIGUSR1 wrapper, and SIGINT/SIGUSR1 while classifying. After our own cancel is consumed `InterruptPending` is set again, so other pending interrupts (`transaction_timeout`, PG17+ recovery conflicts) are not lost.
  - An attempt that hit the limit but used under half of it in CPU time was a scheduling stall and is retried, up to 3 attempts (≤ ~300 ms per pattern). Inside a VM, time the host takes the virtual CPU away can count as CPU time, and the guest often cannot see it (Docker Desktop on macOS reports no steal time), so a heavily overloaded host can still make `ALTER SYSTEM` reject, or a backend disable, a normal pattern (item 20261006-113156-1). Tests that aren't about the limit raise it with `pssc_extract_test_regex_compile_limit()`, and the test module's `sleep` injections use the full limit in CPU time before the deadline fires (bounded by 5 s of wall time and honouring cancels), so they never look like a stall. Each attempt compiles in its own memory context; a failed or interrupted attempt's context is deleted before the next one (item 20261006-080948-1; on PG16+ a thrown cancel skips the engine's own cleanup).
  - With interrupts held off the compile is put off. Processes other than client backends compile without a bound.
- **appname** (done, item -38): `appname(format=sqlcommenter|marginalia|regex)` parses `application_name` instead of comment text, using the named format's rules and parameters (`url_decode`; `kv_sep`/`pair_sep`; `pattern`/`keys` for `regex`). `keys`, `rename` and `merge` work as usual; `position` and other formats' parameters are rejected by the check hook.
  - `appname` extractors form an **independent chain** with the usual first-producer/`merge` rules; the comment chain and the appname chain don't skip each other. Results are combined and the **comment wins** on key conflicts, regardless of list order. A comment pair dropped by the pipeline does not block the appname value for that key.
  - Step-8 fill priority depends only on the key, not on the source. Appname-only tags make a statement count as tagged.
  - The value is `application_name` at **execution start** (`EXECUTE` time for prepared statements; `SET application_name` itself uses the previous value). `nested_tags=inherit` keeps the top-level tags; `scan` re-reads.
  - Each backend caches the parsed result keyed by the exact string, the config generation and limits; the cache is invalidated when the regex hook changes, and results with transient failures (OOM, interrupted compile, match error; `pssc_regex_transient_failures()`) are not cached. Counters are re-added on every statement as if uncached.
  - PG replaces non-ASCII bytes in `application_name` (`?` on 14–15, `\xHH` on 16+), so derived values are ASCII. `_extract()` uses the caller's current value and has no separate field for appname tags.

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

Validation happens in the GUC `check_hook`, so a malformed config is rejected when it is set or reloaded and the previous config stays active. The `check_hook` parses the DSL and test-compiles and frees every regex, then returns the parsed form as one flat, pointer-free `extra` blob. That blob is allocated with `malloc` on PG14/15, where guc.c frees it with `free()`, and with `guc_malloc` on PG16+, where it must live in the GUC memory context. A `compat.h` helper hides this difference. The `assign_hook` only stores the pointer and bumps a config generation number. GUC assign hooks must not fail, because they also run during transaction rollback.

The `check_hook` rejects the following (decided 2026-10-05, item -8):

- unknown extractors or parameters, and duplicate parameters
- more than 16 extractors
- keys that are empty or longer than 63 bytes
- empty separators, or separators longer than 8 bytes
- a `kv_sep` that contains `pair_sep`, because pairs are split on `pair_sep` first, so such a `kv_sep` could never match

Separators that start with whitespace are allowed (for example `kv_sep=' :'`). For `regex`, the number of `keys` must equal the number of capture groups. The generation number is bumped only when the parsed blob changes.

Note: values in `postgresql.conf` undergo backslash-escape processing, so a regex there must double its backslashes (`\\w`). `ALTER SYSTEM` writes the escaping for you.

Compiled regexes are not part of `extra`, because GUC frees only the top-level block. Each backend compiles them lazily on first use after a regex generation change (bumped only by `extractors` and `normalize`, not by `tags` or `exclude_tags`), into a private memory context that it owns, and frees the old ones with `pg_regfree`. Regex allocation uses `malloc` on PG14/15 and `palloc` on PG16+. If lazy compilation fails (for example, out of memory), that extractor is disabled for the backend and the failure is counted. The statement itself is not failed.

> **Alternatives considered:** a config table (`pg_stat_statement_context.rules`). Rejected for v1 because reading catalogs from `ExecutorEnd` adds overhead, is database-local while the extension is cluster-wide, and gets complicated inside aborted transactions. GUCs are already reloadable, permissioned, and shown in `pg_settings`. A separate `config_file` GUC for complex setups was also rejected (§11 Q3): `ALTER SYSTEM` plus `pg_reload_conf()` (§4.1) already gives a from-SQL path with the same all-or-nothing validation.

## 5. Storage and the rolling buffer

### 5.1 Key and entry

The extension is a **companion** to `pg_stat_statements`, not a replacement (decided 2026-10-05). Each (query × context) entry stores only two counters per time bucket: `calls` and `total_exec_time`. Everything else that pgss tracks per `queryid` (rows, shared/local/temp blocks, WAL, I/O timing, JIT, min/max/mean/stddev, planning time) is left to pgss, and users join to it on `(userid, dbid, queryid, toplevel)` (§7).

*Why `total_exec_time` and not just `calls`:* `calls` alone can't apportion load across contexts when the per-context cost of the same `queryid` differs (for example, one controller passes a selective parameter and another a non-selective one). `total_exec_time` is the minimum needed to attribute cost to a context. Other pgss metrics can be apportioned approximately by each context's share of `total_exec_time` (§7).

The key layout is **one entry per (query × context) holding a per-bucket counter ring**. `bucket_id` is *not* part of the key (decided 2026-10-05, §11 Q5). Because an entry holds only two counters per bucket, the ring costs about 24 bytes per bucket (288 bytes with the default 12 buckets), which is cheap compared with the key itself.

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
    slock_t  mutex;        /* protects everything below, and its evictSlot */
    int      encoding;     /* encoding of tags[] (that of dbid) */
    int      evict_index;  /* its slot in the eviction array (§5.3) */
    int64    last_bucket;  /* newest bucket_id written; drives reclamation */
    int64    calls_total;      /* monotonic since stats_since (§7) */
    double   exec_time_total;  /* ms, monotonic since stats_since */
    TimestampTz stats_since;   /* entry creation time */
    ctxSlot  slots[FLEXIBLE]; /* bucket_count slots; index = bucket_id mod bucket_count */
} ctxEntry;

typedef struct evictSlot { /* the compact eviction array, §5.3 */
    int64    last_bucket;  /* copy of the entry's */
    double   usage;        /* pgss-style usage for eviction (not exposed) */
    void    *entry;        /* the hash entry */
} evictSlot;               /* 24 bytes; max_entries of them in the header */
```

**Per-entry monotonic counters** (decided 2026-10-06, item 20261006-010149-1): every recorded call also adds to `calls_total` and `exec_time_total`, whatever slot it lands in. Unlike the ring, they are never decremented by bucket expiry. They are zeroed, and `stats_since` set to the current time (the real clock, like `stats_reset`), when the entry is created, which is also what a reset or the entry's reclamation or eviction amounts to. They give exporters a counter that `rate()` can use (§7). They add 24 bytes to the entry header (24 → 48 bytes after `MAXALIGN` on 64-bit platforms).

`ctxSlot` is implemented as `PsscSlot` in `src/counters.[ch]` (item -12).

- An unwritten slot has `bucket_id = PSSC_BUCKET_NONE` (`INT64_MIN`), which is older than any real bucket and is skipped by merges.
- Bucket ids are signed `int64`, so "older" is a plain `<`.
- Usage follows pgss: it starts at 1.0, each call adds 1.0, and each eviction pass multiplies it by 0.99.
- There are no "sticky" entries, because an entry is only created when a call is recorded.

The **full canonical tag set is part of the key**, so distinct tag sets can never be merged and no collision probing is needed. Like pgss, dynahash chains entries and compares the actual keys. The table uses custom `HASH_FUNCTION` and `HASH_COMPARE` callbacks. The hash combines the fixed fields with `tags_hash`. The compare checks the fixed fields and `tags_len`, then runs `memcmp` on only the used bytes. Keys are still built by `memset`-ing the whole key to zero first (pgss does the same), so padding and unused tag bytes are always defined.

`keysize` is computed at startup from `max_tagset_bytes` (`MAXALIGN(24 + max_tagset_bytes)`; 24 is the fixed key header), and `entrysize` from `keysize` and `bucket_count` (`keysize + MAXALIGN(entry header) + bucket_count × 24`; the header is 48 bytes on 64-bit platforms). Shared memory is sized as `hash_estimate_size(max_entries, entrysize)` plus the header, which ends with the eviction array of `max_entries` 24-byte slots (§5.3): `MAXALIGN(header + max_entries × 24) + hash_estimate_size(max_entries, entrysize)`, using `add_size`/`mul_size` overflow checks. The table is created with `init_size = max_size = max_entries`, so all entries are preallocated. `ShmemInitHash`'s `max_size` is only an estimate, not a limit, so the `max_entries` cap is enforced by the extension under the exclusive lock. With the defaults, an entry is on the order of 1 KB, dominated by the tag set (872 bytes: a 536-byte key, the 48-byte header and 12 × 24-byte slots; the monotonic counters made it 24 bytes larger, about 2.8%). `shmem_bytes` at the defaults is 9,261,288 bytes: 9,021,288 for the table and header, plus 240,000 (about 2.7%) for the eviction array (item 20261006-075124-1; the monotonic counters had raised it from 8,781,288). `_info()` reports the exact `shmem_bytes` value.

**Exemplar slots** (item 20261005-091225-33, §6.13) follow the counter ring at the end of each entry, so they live in the same hash entry and are covered by the same spinlock. With `n` keys in `exemplar_keys` (0: none, no memory), each entry gets `per_entry = exemplar_memory / max_entries` bytes, rounded down to a multiple of `MAXIMUM_ALIGNOF`; each key gets `per_entry / n`, minus a 2-byte length, capped at 256 bytes, as its value length `L` (`_info().exemplar_value_bytes`; 0 when the share is under 3 bytes, and then every value is dropped). The block is `MAXALIGN(n × (2 + L))` bytes and is added to `entrysize`, so `max_entries × block` (`_info().exemplar_shmem_bytes`, part of `shmem_bytes`) never exceeds `exemplar_memory`. A slot is a native `uint16` length (`0xFFFF` = no value) followed by `L` bytes, read and written with `memcpy` (the stride is not aligned). Nothing is allocated dynamically: the layout is fixed at startup like the ring. The default (`exemplar_memory = 2MB`, 10,000 entries) gives 208 bytes per entry, so one key gets 206 bytes and two get 102 each, enough for a 55-byte `traceparent`.

A record is dropped and counted in the header counter `dropped_records` (exposed by `_info()`) only when an eviction pass (§5.3) freed nothing: no entry was dead and the sort array could not be allocated. Testing (§9) uses a forced-collision mode, set only through the test module and only while the table is empty. The effective hash is chosen under the table lock.

**Capacity is measured in (query × context) combinations**, independent of `bucket_count`: 5,000 recurring combinations need 5,000 entries whether they are active in one bucket or all of them. A combination keeps its entry while any of its slots is live; once all its slots have expired it is *dead* and is reclaimed first under pressure (§5.3). The documentation should give this sizing rule.

### 5.2 Time buckets

- `bucket_interval` is fixed at startup (postmaster GUC) and stored in the shared header along with an epoch. Every backend computes `bucket_id = floor((now - epoch) / interval)` as an `int64`, so all backends agree and stored IDs always map back to the right timestamps.
- `current_bucket` in the header is a shared, **monotonic watermark**: the newest bucket any writer or reader has observed. It is stored as an `int64` in a `pg_atomic_uint64` and raised lock-free with a compare-and-swap max loop, so it never decreases (decided 2026-10-05, item -14). Advancing it touches no entries, because each entry's ring rolls over lazily (below). The epoch is the shared-memory init time rounded down to a multiple of `bucket_interval` since 2000-01-01, so bucket boundaries fall on wall-clock multiples. When a saved store is loaded at startup (§5.5), the saved epoch is kept instead, and `current_bucket` resumes at max(saved, clock bucket).
- Writers, once they hold the table lock (shared fast path or exclusive insert), re-read the clock and raise `current_bucket` to `max(current_bucket, clock bucket, computed id)`. The call is written to `current_bucket` as read under the entry spinlock. Older ids (a stalled backend, a backward clock step) and newer ones are thus clamped, and every write lands in a slot that is live at that moment. Calls are attributed to the bucket current when the lock is acquired.
- **Per-entry ring rollover:** under the entry spinlock, the writer picks `slot = bucket_id mod bucket_count`. If that slot holds an older `bucket_id`, the slot is zeroed and relabeled before the counters are added. `last_bucket` is set to the written ID. Since written IDs never exceed `current_bucket`, a slot never holds a newer ID than the one being written.
- If the wall clock moves backwards, `current_bucket` never decreases, and writes clamp to it. A forward jump larger than the ring makes every slot stale at once. Bucket arithmetic is signed, so the cutoff can't underflow.
- Rollover happens lazily on the write path, so no background worker is needed. (The optional reclaim worker of §5.3 only frees dead entries early; it advances `current_bucket` exactly as a reader does.) Readers do not depend on writers: the SRF first raises `current_bucket` to the clock bucket, then hides slots outside the live window `[current - bucket_count + 1, current]`, using the watermark read after copying each entry. Once a slot has been seen as expired it stays expired, even if the clock later steps back. Reclamation (§5.3) uses the same watermark.
- Semantics: an execution is attributed to the bucket in which its `ExecutorEnd` (or utility completion) runs. Its whole accumulated time goes there, even for a long-lived cursor that started much earlier. Buckets therefore show **completions per interval**, not work done per interval.

### 5.3 Eviction under pressure

If an insert finds the table at `max_entries`, then under the exclusive lock:

1. Reclaim **dead** entries first: those whose `last_bucket` is older than the live window, so every slot has expired.
2. If that frees less than ~5% of `max_entries`, evict further live entries, pgss-style: order by `last_bucket` (least recently written first), then by `usage` (lowest first), and evict until ~5% is free. `usage` decays as in pgss.
3. Increment `dealloc` (once per eviction pass), `reclaimed_entries` (per dead entry reclaimed in step 1) and `evicted_entries` (per live entry evicted in step 2), all exposed by `_info()` (split decided 2026-10-06, item 20261005-213120-1). Reclaiming dead entries is normal housekeeping on a table whose combinations change over time and loses no visible history; only `evicted_entries` shows that `max_entries` is too small.
4. If the pass freed nothing (no dead entry, and the candidate buffer could not be allocated), the call is not recorded and `dropped_records` is incremented; any non-zero value means the table is badly undersized (or the backend is short of memory).

An eviction pass scans a **compact eviction array** once, not the entries (item 20261006-075124-1): the shared header holds one 24-byte slot (`last_bucket`, `usage`, entry pointer) per entry, dense in `[0, entries)`, and each entry records its slot's index (`evict_index`). `usage` lives only in the slot; `last_bucket` is a copy of the entry's. A write updates the slot together with the entry, under the entry spinlock; an insert appends a slot; a removal (reclaim, eviction) moves the last slot into the hole and updates the moved entry's `evict_index`; a reset empties it. A pass thus reads 24 bytes per entry instead of the whole entry (about 870 bytes at the defaults), and touches only the entries it removes (and the one whose slot moves into each hole). The live victims are chosen by partial selection (a bounded heap of `target` candidates), not by sorting every entry (item 20261006-043919-1). Ties in (`last_bucket`, `usage`) are broken by the order in which the pass meets the slots (array order), so the victims of a pass are deterministic. That cost is paid only when the table is full; the benchmarks measure it (§9).

Details (decided 2026-10-05, item -15):
- **Target:** each pass aims to free `max(1, max_entries * 5 / 100)` entries.
- **Dead entries:** the pass first raises `current_bucket` to the clock, as readers do. One scan of the eviction array then reclaims *all* dead entries, even beyond the target, without allocating anything (the slot moved into a reclaimed entry's hole is judged next).
- **Live entries:** these are evicted only when the dead entries fall short of the target.
- **Decay:** every surviving entry's `usage` is multiplied by 0.99 on every pass, as in pgss.
- **No entry spinlocks:** every path that takes an entry spinlock holds the table lock (shared), so the pass reads and writes `last_bucket` and `usage` without spinlocks while it holds the exclusive lock. The eviction array needs no lock of its own: a slot is written only under its entry's spinlock by a holder of the shared lock, or by the holder of the exclusive lock; slots are added, moved and removed (and `evict_index` changes) only under the exclusive lock. `pssc_store_check_invariants()` (and an assertion after every pass) checks that the array and the table match one to one.
- **Out of memory:** the candidate buffer (`target` entries, kept per backend when ≤ 64 kB, otherwise allocated per pass) uses `MCXT_ALLOC_NO_OOM`. If that fails, only dead entries are reclaimed. The user's statement never fails, and `dealloc` still counts the pass; if nothing was dead either, the record is dropped (step 4).

**Reclaim worker** (optional, item 20261005-091225-34, decided 2026-10-06). With `reclaim_worker = on` (postmaster GUC, default `off`), `_PG_init` registers one background worker; with it off nothing is registered, so there is no extra process and behaviour is exactly as above. The worker only touches the extension's shared memory, so it has `BGWORKER_SHMEM_ACCESS` but no database connection (it is not in `pg_stat_activity`; its process title is `pg_stat_statement_context reclaim worker`), starts at `BgWorkerStart_ConsistentState`, and is restarted 10 s after an `ERROR` exit. Every `reclaim_worker_interval` (sighup, default 10 s; a reload wakes it) it runs `pssc_store_reclaim_dead()`:
- Under the shared lock, raise `current_bucket` to the clock, as readers do (§5.2). If the watermark equals the one its previous pass used, stop: an entry only dies when the watermark moves past its live window, every later write lands at or above it, and the previous pass left nothing dead. So an idle worker takes the exclusive lock at most once per bucket.
- Otherwise, under the exclusive lock, raise the watermark again and run the same scan of the compact eviction array as step 1 of an eviction pass (one shared helper, `evict_scan()`), removing every dead entry.
- **No decay, no live evictions:** both are pressure-only. Usage decay paces the choice of live victims per eviction pass (as in pgss); tying it to how often an idle worker wakes up would change which entries a later pass evicts.
- **Counters:** `reclaimed_entries` counts what the worker removes, as it does for passes (it is "dead entries reclaimed", whoever reclaims them). `dealloc` is *not* incremented: it counts passes forced by a full table, like pgss's `dealloc`, and an idle worker must not make the table look undersized. `evicted_entries` is untouched since nothing live is removed.

### 5.4 Locking

The locking is modeled on `pg_stat_statements`, and latency is measured rather than assumed (§9):

- One LWLock for the hash table. Shared mode is enough to look up an existing entry and update its ring under that entry's spinlock, including the lazy slot rollover (§5.2). Exclusive mode is only needed to insert, evict, or advance `current_bucket`.
- LWLocks cannot be upgraded. On a miss, the backend releases the shared lock, acquires the exclusive lock, and then re-validates everything. It re-checks the bucket (§5.2) and repeats the `HASH_ENTER` lookup, because another backend may have inserted the entry in the meantime. Only then does it evict, if needed, and insert.
- Because the key does not include the bucket, a recurring combination misses only once, when it is first seen (or after it was evicted), as in pgss. The only per-interval exclusive acquisition is the single header advance at each bucket boundary.
- Tag extraction and hashing happen **before** any lock is taken.

### 5.5 Persistence across clean restarts

Done in item 20261005-091225-35, after `pg_stat_statements` (`pgss_shmem_shutdown()` / `pgss_shmem_startup()`). Controlled by `pg_stat_statement_context.save` (§4.1; `on`, `sighup`, as `pg_stat_statements.save`).

- **File:** `$PGDATA/pg_stat/pg_stat_statement_context.stat` (`PGSTAT_STAT_PERMANENT_DIRECTORY`, like pgss's file). It is written to `<file>.tmp` with `AllocateFile()`, then renamed with `durable_rename()`, so a reader sees the old file or the complete new one.
- **Who saves, and when:** as in pgss, the callback is registered with `on_shmem_exit()` only when `!IsUnderPostmaster`: by the **postmaster** (or a standalone backend), not by every backend. It saves only when (a) the exit code is 0, (b) `save` is on, and (c) `global/pg_control` (read directly, with its CRC checked, because an ERROR inside `proc_exit()` would become FATAL) says `DB_SHUTDOWNED` or `DB_SHUTDOWNED_IN_RECOVERY`. When the postmaster exits after a smart or fast shutdown, every child has already exited and the checkpointer has written the shutdown checkpoint. No other process is attached to shared memory, so the store can no longer change, and it is read without locks. Condition (c) is stricter than pgss: an **immediate** shutdown also exits the postmaster with code 0, but its children were `SIGQUIT`ed at arbitrary points, so the store may be torn. In that case `pg_control` still says "in production", and nothing is saved (LOG "not saving statistics: the server did not shut down cleanly"). The same LOG is emitted for a crash-restart cycle (`shmem_exit(1)`).
- **Format** (native byte order, since the same build reads it):
  1. A header: magic `PSSC`, a binary format version (`PSSC_DUMP_FORMAT`, bumped on any layout change, independent of the SQL version), `PG_MAJORVERSION_NUM`, and the extension version (`default_version`, injected by the Makefile).
  2. The settings that shape the data: `epoch`, `interval_us`, `bucket_count`, `max_entries`, `max_tagset_bytes`.
  3. The state: `current_bucket`, the entry count, `stats_reset`, `dealloc`, `reclaimed_entries`, `evicted_entries`, and the eight diagnostic counters.
  4. One record per entry, in compact-eviction-array order. Each record holds the key fields (including the stored `tags_hash`), `last_bucket`, usage, the monotonic totals, `stats_since`, `encoding`, the tag bytes, and the whole ring.
  5. A CRC-32C over everything, and then nothing (trailing bytes are invalid).
- **Load:** in `shmem_startup_hook`, when the postmaster creates the store, before any other process exists. Like pgss, the file is **unlinked whatever the outcome**, so a crash before the next clean shutdown cannot replay it. The server never fails to start because of the file:
  - **save is off:** the file is discarded (LOG).
  - **Missing file:** start empty, silently.
  - **Unreadable, bad magic, truncated, failed CRC, a malformed record** (ring invariants of §5.2 against the saved watermark, the tag-set shape, the encoding), **duplicate keys, trailing bytes:** LOG "ignoring invalid data" (or "could not read file"), and start empty.
  - **A different format, PostgreSQL major, or extension version:** LOG "discarding ... written by a different version", and start empty.
  - **A different `bucket_interval` or `bucket_count`:** LOG "discarding ... bucket_interval or bucket_count changed", and start empty. Bucket ids would mean other times, and pgss has no bucket analogue.
  - **Out of memory:** loading reads the file twice (validate, then insert) and keeps one fixed-size record per saved entry plus a sort array, so its memory grows with the file's entry count. Every such allocation uses `MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM` (an ERROR here would be FATAL in the postmaster) and grows only as records are actually read. On failure: LOG "discarding ... out of memory", and start empty.
  - **Otherwise, load.** The saved `epoch` is kept, so the saved bucket ids keep their meaning. `current_bucket` becomes max(the saved watermark, the clock's bucket at that epoch); it is never loaded lower, and time has passed. Entries that are dead at that watermark (§5.3) are dropped and counted in `reclaimed_entries`. Expired slots of the other entries are cleared.
  - **Entries whose tag set exceeds the current `max_tagset_bytes`** (it may have shrunk) are skipped, with one LOG line giving the count.
  - **If more live entries remain than the current `max_entries`** (it shrank), the excess is evicted in the §5.3 order: `last_bucket`, then usage, then array order, with no usage decay. This counts as one eviction pass (`dealloc` +1, `evicted_entries` += the excess) and is logged. A larger `max_entries` loads everything.
  - The kept entries are inserted in their saved order, so the compact eviction array keeps its order, and the header counters, `stats_reset`, and usage are restored. A LOG line summarizes the result ("loaded N of M saved entries", with the expired, skipped, and evicted counts).
- **Not saved:**
  - the cardinality-cap tracking table (§6.1), so values seen before the restart do not count against a cap after it;
  - the debug clock and the collision mode (testing aids, which start from their defaults);
  - `bucket_advances` (a diagnostic, which restarts at 0).
  - exemplar values and `exemplar_values_dropped` (§6.13): a restored entry shows `exemplars = {}` until its next call. An exemplar points at a recent trace, so a value from before the restart is of little use, and leaving it out keeps the file format (and its size) unchanged.

## 6. Gotchas and mitigations

### 6.1 Tag cardinality explosion
`traceparent`, `request_id`, and per-user values make every statement unique, which can empty the table within seconds. This is mostly an operator choice: only allowlisted keys are stored, and the default allowlist is `action, controller, job`. New tags that application developers add are therefore ignored until an operator opts in. The table can still flood if an allowed tag has unexpectedly high-cardinality values (for example, an unnormalized route like `/users/123`), or if a buggy or malicious client sends random values for an allowed key. **Mitigation:** the restrictive default allowlist, a denylist of known high-cardinality keys for anyone who opts into `tags = '*'`, `max_tag_value_len` truncation, `_info()` counters for evictions, per-key value normalization rules (`normalize`, item -41), and per-key cardinality caps (item -32, below). Exemplar storage is on the roadmap (§8).

**Cardinality caps** (item -32, landed 2026-10-06): `cardinality_cap` and `cardinality_cap_overrides` (§4.1) bound the distinct values per key, counted per `cardinality_cap_scope` (not per bucket or `queryid`). Values beyond the cap collapse to JSON `null`, which cannot collide with a real value because a client can only send strings; the statements still count, in one `null` entry per query and remaining tags. The tracking table is a separate lock-free shared-memory area: open addressing with CAS on 64-bit words holding a generation number and a 44-bit value fingerprint (false-positive rate about n/2^44). Values never decay: eviction does not free them; only `_reset()` or a restart does. Resets are serialized by an LWLock that lookups and admissions never take; `_reset()` bumps a 64-bit reset count (O(1)), and every admission re-checks that count before updating a key's count, so it never repeats in practice. The 20-bit generation stored in table words is derived from it; once every 2^20−1 resets it wraps, and that reset clears the whole table under the lock (new values collapse to `null` during the clear). `_reset()` clears the caps before the store. A value is admitted when tags are extracted at statement start, so failed statements also use cap space. When the table is full, new values collapse to `null` (fail closed) and are counted in `cap_table_full` as well as `capped_tags`. Known narrow races: a value another backend is inserting at the same moment can briefly collapse, and two backends inserting the same value can collapse a third. The count never exceeds the cap except through fingerprint false positives.

**Cap scope** (item 20261007-070036-1, decided 2026-10-07 after the security reviews): a server-wide cap is a cross-role membership oracle (at the cap, a value another role already sent stays a string while an unseen one becomes `null`, visible in the sender's own rows) and lets any role use up everyone's cap. `cardinality_cap_scope` (postmaster) therefore selects the scope, default `role`: the key hash is seeded with a hash of (userid, dbid) (`role`) or of dbid alone (`database`), keyed with a 64-bit secret; `server` uses seed 0, the earlier unkeyed hash. The value hash is seeded with the key hash, so a key's count word, its admitted values, and their slots and fingerprints are all per scope. The secret (in the table header, from `pg_strong_random()`) is drawn at startup and again by every `_reset()`, which writes it before publishing the new generation; admissions read it after the generation, and one that raced a reset finds the generation changed before any write and restarts. Without the secret, slot positions would follow from public OIDs: a role could fill the probe window after another scope's candidate value with values of its own (under its own cap, in a sparse table) and learn from one more value whether the candidate's slot is taken. Under `server` this probe still works, which is acceptable there since `server` shares the admitted sets anyway. userid and dbid are `GetUserId()` and `MyDatabaseId` when the tags are extracted, re-applied for another user where needed (see "Identity" below); the debug function peeks in the caller's scope. The table and its size (`cardinality_cap_slots`, one shared limit like `pg_stat_statements.max`) do not change: scoping only fills it faster (each scope takes its own key slot and up to the cap of value slots), so busy multi-tenant servers may need more slots. A (nearly) full table collapses new values of every scope to `null`, observable by any role: the accepted residual of a shared limit, as with `pg_stat_statements.max`. `_reset()` empties every scope.

**Identity** (item 20261007-070036-2): every tag set that is recorded or published obeys the caps of the role it is recorded or published under. Under `role` scope that role can differ from the one the caps were applied for at extraction: the recorded userid is refreshed at `ExecutorEnd` and after a utility (pgss's end-time userid, kept unchanged), so a cursor opened under role A can be finished under role B (`SET ROLE`), or a cursor a `SECURITY DEFINER` function opens is fetched and closed by its caller (with `nested_tags = scan`); a tagged `SET ROLE` is recorded under its new role; nested statements under `nested_tags = inherit` copy their caller's tags into a `SECURITY DEFINER` body run as the definer; and the activity row is published under `GetUserId()` when a top-level portal runs, which can follow a `SET ROLE` after the portal was created. In each case the caps are re-applied (a *recap*) for the receiving role first: steps 8 and 9 run again, under that role's caps as configured at that moment, on the input step 8 had at extraction, so the result is the tag set extraction would have produced for that role. Like extraction, a recap admits the values it keeps as strings in the receiving role's scope, and counts its collapses in `capped_tags`/`cap_table_full` again (not its drops, already counted).

To recap, a frame keeps the input of step 8 next to its tags (`cands`) and the role its tags were capped for (`cap_userid`); the check at each of the three points (inheritance into a new frame, activity publication, the refresh before recording) is `cands != NULL && cap_userid != userid`, so the unchanged-identity path costs one comparison, with no copy or allocation. `cands` is only kept when it can matter: when caps were on (`env.cap`) under `role` scope at extraction. Under `database` and `server` scope, or with caps off, it is NULL and nothing changes (a backend's database never changes). Two encodings keep it small:

- When every candidate was kept as a string (the common case: no cap collapsed a value and nothing was dropped for size or `max_tags`), the tag set holds all of them, so `cands` is `P` and one byte per tag, its index in the serialized set in priority order: at most 65 bytes.
- Otherwise, `L` and the candidates (`key\0value\0`, original values) in priority order, skipping those that cannot fit `max_tagset_bytes` even alone, the longest prefix that fits in `1 + 2 × max_tagset_bytes` bytes (possibly none: `L` alone). The fill of a prefix keeps exactly what the full fill keeps of it, so a recap keeps the tags the receiving role's extraction would have kept from the prefix and drops the rest: fewer tags, never one past a cap.
- `cands` is NULL only for an empty tag set, which obeys every cap. Any non-empty set has one, even when the budget left it no candidates (it then recaps to an empty set), so a non-empty set never skips a recap.

Executor frames store `cands` after their tags and exemplars in the frame's allocation; a recap writes the new tags and `cands` into a new chunk in the frame's `es_query_cxt` (freeing the previous recap chunk). Utility frames keep `cands` of up to 72 bytes inline in the stack snapshot and longer ones in `TopMemoryContext`, freed when the utility hook returns or errors. A recap or storage allocation failure fails closed: the frame gets no tags, counted as out of memory, rather than tags that escaped a recap. A recap uses the caps' configuration at its time (a `SIGHUP` between extraction and recap applies), but caps turned on after extraction don't apply to a frame extracted with caps off. Inherited tags are recapped for the child's role and a new `cands` stored with them, so a chain of definers recaps at each change of role. A recap runs only steps 8 and 9: it does not apply `tags` or `exclude_tags` again, which can have changed since extraction (they are `superuser` settings, §4.1, so a function's `SET` clause or `SET LOCAL` can change them mid-statement). The candidates were filtered when they were extracted and are a copy, so a recap never reads the parsed lists.

### 6.2 Long queries (e.g., 10k-element `IN` lists)
Even a linear scan costs something on a 1 MB query string. **Mitigation:** if the statement range fits within `scan_window`, it is always lexed exactly from the front. For longer statements, `position=append|prepend` limits work to the first or last `scan_window` bytes:
- The statement end comes from `stmt_len`. A `strlen` is needed only when `stmt_len == 0` (rest of string), and that cost is included in the overhead benchmarks.
- `append` trims whitespace and `;` **within the window only**, expects a closing `*/`, and walks backwards to the matching `/*` while tracking nesting depth. A trailing `--` comment, or a comment that crosses the window start, yields no tags. The scan never parses half a comment.
- Starting a lexer at an arbitrary offset means its state is unknown. A string literal ending in `*/` can therefore fool the tail path into misattributing a statement, and a `--` line comment opened before the window can make text inside it look like a tagged comment (fake tags; covered by the extract regress test). Such results are counted in `_info().heuristic_scans` so operators can see how often the inexact path is used.
- `position=any` does a full forward lexical scan.

The scanner uses the *current* `standard_conforming_strings` setting. The hooks don't expose the setting that was in effect at parse time, so changing it between PREPARE and EXECUTE can mis-scan plain strings that contain backslashes. This is documented as a limitation.

### 6.3 Prepared statements carry stale comments
Comments are read from the source text saved at **Parse/PREPARE** time. **Bind/Execute** and SQL `EXECUTE ... /*tags*/` supply no new statement text, and the executor runs the saved source. If a client prepares once and executes many times from different code paths, every execution is attributed to the first caller's tags. This applies to named *and* unnamed statements: an unnamed statement only gets fresh context if the driver sends a new Parse for each use. pgss has the same limitation for query text. **Mitigation:** document this clearly. Drivers and ORMs that include the comment in their statement-cache key, or that re-Parse each time, are unaffected. Before v1, check the behavior of the target drivers (Rails/PG, pgx, JDBC, psycopg, and pgbouncer in transaction mode). If they reuse prepared plans across contexts, move the session/transaction override (`tags_override`, §8) into v1.

**Result (2026-10-05, item -27, `research/driver-prepared-statements/`):** no target driver reuses a prepared statement across different comments. The statement cache is keyed by SQL text that includes the comment, and pgbouncer shares statements by query text. Tested with pgx 5.11, pgjdbc 42.7.13, psycopg 3.3.6, ActiveRecord 7.0–8.1 with pg 1.7 and marginalia 1.11, and pgbouncer 1.26. Native Rails `query_log_tags` (Rails 7.1+) disables prepared statements. Stale context appears only when the application itself reuses one prepared handle across requests. **Decision: `tags_override` stays on the roadmap (no-go for v1).** Notes for the user docs:
- Each distinct comment value creates a separate prepared statement, so high-cardinality values in comments (such as request IDs) defeat statement caching.
- On Rails 8, marginalia does not annotate some ORM paths (`pick`, `find_by`); prefer `query_log_tags`.

### 6.4 Nested statements
PL/pgSQL and trigger bodies have their own `sourceText` (the function body), where comments are code comments rather than request context. **Mitigation:** with `nested_tags = inherit` (the default), a nested executor or utility frame copies the tags of the *active* frame (§3.2). Utility statements such as `CALL` and `DO` always create a frame so their children can inherit, even when the utility itself is not recorded. This lets users answer "which controller caused this trigger's query to run?", which is a useful feature in its own right. Statements run during the *planning* of a parent, such as constant-folded function calls, have no active frame. They get only their own tags.

**Costs are inclusive.** With `track = all`, a parent's time already includes its children's work. Executor instrumentation wraps Run and Finish, including AFTER triggers. Summing parent and child `total_exec_time` therefore double-counts. Per-application cost totals should filter on `toplevel` (§7).

### 6.5 Multi-statement query strings
`SELECT 1 /*a*/; SELECT 2 /*b*/` arrives as a single `sourceText`. Each statement's range comes from `stmt_location`/`stmt_len`. The first statement starts at byte 0, and each later one starts just after the preceding `;` (PG14–17). **PG18** instead sets `stmt_location` to the statement's first token, so leading comments fall *before* the range. The scanner therefore extends a statement's owned range backwards over leading trivia, back to the previous `;` token boundary or the string start, found lexically, never by a naive search for `;`. A `stmt_len` of 0 means "to the end of the string", and a location of -1 means unknown, in which case the whole string is used, as in `CleanQuerytext`. A comment after the final `;` lies outside every range. A hook only knows its own statement's range. **Mitigation:** extraction is statement-local. As a single fallback, a statement whose own range has no tags may use a trailing comment after its range, but only if the rest of the string contains nothing except `;`, whitespace, and comments. That proves the statement is the last one. So in `SELECT 1; SELECT 2; /*controller:x*/`, only `SELECT 2` gets the tags. Comments are never attached to statements whose ranges don't own them.

### 6.6 Utility statement query IDs differ by version
- **PG14/15:** core sets a utility statement's `queryId` by hashing the **statement text** (`compute_utility_query_id` after `CleanQuerytext`), which includes comments. Every distinct tag set therefore produces a different query ID, and DDL fingerprints fragment.
- **PG16+:** utility statements are jumbled from the parse tree, so comments have no effect.

**Decision:** the `queryid` column always equals the core and pgss value on every version, because it is the join key. pgss sees the same fragmented IDs on PG14/15, so joins still work. The fragmentation is documented. A separate comment-insensitive `utility_textid` column was considered and rejected (decided 2026-10-05, §11 Q4): pgss has the same PG14/15 behavior, so it is not worth the extra column.

### 6.7 `EXECUTE`/`PREPARE` and utility nesting
SQL `EXECUTE` goes through `ProcessUtility`, and then the executor runs the prepared plan. Recording eligibility and nesting are separate decisions:
- **Recording** mirrors pgss so rows join one-to-one. `EXECUTE` and `PREPARE` are never recorded as utilities, because the executor records the underlying plan. `DEALLOCATE` is excluded on PG14–16 and recorded on PG17+, where pgss changed its list. That exclusion was not about double counting, since `DEALLOCATE` runs no plan.
- **Nesting:** `EXECUTE` and `PREPARE` do **not** increment `nesting_level`, so the plan run by `EXECUTE` still counts as top-level and `track = top` records it. All other utilities always activate a frame, even when `track_utility = off`, so `CALL`/`DO` children inherit tags. Nesting is a separate decision that mirrors pgss, so `toplevel` matches on every version:
  - PG17+: every other utility increments nesting.
  - PG14–16: nesting is incremented only when pgss itself would track the utility (its `track_utility`, its `track` at this level, its exclusion list, not in a parallel worker). pgss's GUCs (`pg_stat_statements.track_utility`, `pg_stat_statements.track`) are read by name on each utility statement. When pgss isn't loaded, or its GUCs are only placeholders, this extension's own `track`/`track_utility` are used instead (decided 2026-10-05, item -18).
  - Recording eligibility always follows this extension's own settings.
- A tracked utility that arrives with `queryId = 0` (wrong load order) increments `utility_missing_queryid`, regardless of the `untagged` policy. It also rises in the correct load order for a utility re-executed from a plan cache (a named extended-protocol statement, or a PL/pgSQL `CREATE`/`DROP`): pgss zeroes such a statement's `queryId` on its first execution, so pgss itself counts only that first execution per backend, and so do we. Parity holds; the counter shows the later executions (found in item -22).
- **PG18 boundary cache (§6.5):** every top-level statement of the client string that gets no frame (`PREPARE`, `EXECUTE`, untracked utilities, or statements skipped at `ExecutorStart`) still advances the boundary cache via `pssc_context_note_stmt_boundary()`, so later statements keep their leading comments (item 181131-1).
- `ProcessUtility` copies everything it needs before chaining and never reads `pstmt` afterwards, because `ROLLBACK` can free it.

### 6.8 Parallel workers
Parallel workers run executor hooks too. **Mitigation:** skip when `IsParallelWorker()`, as pgss does, so each statement is counted once. The leader's elapsed time already covers the workers' execution.

### 6.9 Errors and cancellations
`ExecutorEnd` is not reached when a statement errors, so failed statements are not counted. This is the same as pgss. Frames of failed executors are cleaned up by their memory-context callback (§3.2). The active-frame and nesting changes are wrapped in `PG_TRY`/`PG_FINALLY`, so they can't leak. Per-tag-set error and cancellation counts were considered for the roadmap and rejected (decided 2026-10-05): they are out of scope for a pgss companion, so §11 Q6 (error deduplication rules) is moot.

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

**Mitigation:** a `compat.h` with `PG_VERSION_NUM` macros, and a CI matrix that builds and runs regression tests against every supported major version.

**Counter availability.** The stored counters, `calls` and `total_exec_time`, exist on every supported version, so v1 needs no per-version counter shims, and `compat.h` contains none (item 103941-1 removed the speculative ones). A future column that needs a shim adds it in the same change, with a case in `002_compat.pl`. Row counts, buffer/WAL/I/O-timing and JIT fields, whose availability varies by version (for example `shared_blk_read_time` in PG17), are not stored at all (§5.1). General policy, following pgss: if a future column is unavailable on some server version, it is omitted on that version rather than exposed as `NULL`.

### 6.11 Security and privacy
- Tag values are untrusted client input and are never interpreted. Processing order:
  1. decode (URL/`\'`)
  2. reject values that contain NUL or are invalid in the database encoding (`pg_verify_mbstr`)
  3. apply the per-extractor `keys` allowlist, matching **original** key names (decided 2026-10-05)
  4. rename. The renamed key is checked against the database encoding, because the GUC is cluster-wide but databases may have different encodings
  - *exemplar capture* (item -33, §6.13), between steps 4 and 5: if the final (renamed) key is in `exemplar_keys`, the value from step 2 is captured for that key's exemplar slot, **whatever step 5 then does with the pair**, so a key can be both denylisted (not grouped by) and an exemplar. Only the renamed key is looked up, as for the allow- and denylists. Steps 6–8 do not apply to the captured value: it is not normalized, truncated, or capped. A value longer than the exemplar value length is dropped and counted (`exemplar_values_dropped`) and leaves the slot open for a later occurrence. Within one statement the first occurrence wins, with the tags' precedence (override > comment chain > footer > appname). A capture does not count as "producing" for the extractor chain.
  5. apply the global allowlist (or the denylist when `tags = '*'`), and drop keys longer than 63 bytes
  6. value normalization (`normalize`, item -41): rules for the final key run in config order, each on the previous output. Each rule's output is capped at `max(value length, max_tag_value_len)` bytes. On a run-time failure the pair is dropped (fail closed) and counted in `normalize_failures`. A rule that fails to compile is disabled for the backend until the next `extractors` or `normalize` change and counted in `regex_compile_failures`. The CPU and compile limits are the regex extractor's (§4.2).
  7. truncate on a character boundary (`pg_mbcliplen`)
  8. per-key cardinality caps (item -32, §6.1): a value beyond its key's cap becomes `null`, stored internally as `key\0\0\0` (no client string can produce this) and output as JSON `null`. A `null` counts as 2 bytes toward `max_tagset_bytes`. Only tags that step 9 keeps are admitted to the cap, so a dropped tag never uses up cap space; a capped value still counts as "produced" for the extractor chain. Caps are counted per `cardinality_cap_scope`, by default per (role, database), so whether a value collapses depends only on the role's own values in that database (and on the fill of the shared table): it is not an oracle for other roles' tag values, and other roles cannot use up its caps. `database` and `server` scopes share caps between roles (and, for `server`, databases), which reintroduces both cross-role observability (membership of a value in another role's admitted set) and poisoning; use them only when the roles sharing a scope trust each other. Under the scoped modes, slot positions are keyed with a secret, so a role can't aim its own values at another scope's slots; what stays observable is a (nearly) full shared table. Under `role` scope, a tag set recorded, published or inherited under another role than the one it was extracted for (cursors finished under another role, `SECURITY DEFINER`) gets steps 8 and 9 re-applied for that role first (§6.1 "Identity").
  9. sort and serialize within `max_tags` and `max_tagset_bytes`, using greedy fill (decided 2026-10-05):
     - Tags are considered in priority order: allowlist order, or sorted-key order when `tags = '*'`.
     - A tag is kept if it still fits within both `max_tags` and `max_tagset_bytes`; one that doesn't is dropped and counted in `dropped_tags`, and the next tag is tried.
     - So an oversized tag never evicts smaller lower-priority tags.

  Extractor-chain semantics:
  - An extractor "produces" if at least one pair survives steps 1–7, before the `max_tags`/`max_tagset_bytes` limits of step 9. If step 9 then drops every tag, skipped extractors are not retried, and the statement counts as untagged (found while reviewing the user docs, item -28).
  - Once one has produced, later non-`merge` extractors are skipped, but later `merge=on` extractors still run.
  - The first occurrence of a key wins: by chain order, then comment order, then pair order.
  - The trailing-footer fallback (§6.5) is used only when the statement's own range yields no tags.

  Tags from `tags_override` (item -30, §8) go through the same steps except step 3, since no extractor is involved; their step-4 renames come from the comment `sqlcommenter` extractors' `rename` lists in config order (first match wins). They enter ahead of comment tags, so with first-key-wins the precedence is override > comment > appname. `appname` (item -38) is an extractor, so its tags follow every step. Malformed tags are dropped and counted in `_info().invalid_tags`. This includes:
  - NUL or invalid encoding
  - keys longer than 63 bytes, when `tags = '*'`. With an allowlist they can never match (allowlist keys are at most 63 bytes), so they are dropped silently like any other unlisted key
  - parser-malformed segments, counted only for a comment from which the same parser obtained at least one well-formed pair, so probing another format's comment isn't counted (decided 2026-10-05) They never raise an error in the user's statement.
- Tags are stored in the originating database's encoding, which is recorded per entry as pgss does, and converted with `pg_any_to_server` when read. For a `SQL_ASCII` origin, non-ASCII bytes are escaped on output instead of being converted: each byte ≥ 0x80 becomes `\xHH` (lowercase hex) and `\` becomes `\\`, in both keys and values, so the escaping is reversible and distinct keys stay distinct (decided 2026-10-05, item -11). All tag output goes through the shared helper `pssc_tags_push_jsonb()` (`src/tagout.c`); a tag set from a non-`SQL_ASCII` origin that can't be converted falls back to the same `\xHH` escaping for that whole entry, using the conversion's no-error mode (no `PG_TRY`), so the SRF never fails on one bad entry (decided 2026-10-05, item -20). In a `SQL_ASCII` server, tags from other encodings are validated and passed through unconverted, as `pg_any_to_server` does.
- Tags may contain PII, for example user emails in a route. Visibility is **at least as strict as pgss**. For rows owned by another role, both `queryid` and `tags` are `NULL` unless the caller has the privileges of `pg_read_all_stats`. The check runs inside the C SRF, so `showtags = false` doesn't bypass it. The check is `has_privs_of_role(GetUserId(), pg_read_all_stats)` on every version, which on PG14 is slightly stricter than pgss there (`is_member_of_role` also admitted NOINHERIT members). The activity view and the `exemplars` column (§6.13) use the same rule: `exemplars` is `NULL` exactly when `tags` is. The cardinality caps keep to this only under the default `cardinality_cap_scope = role` (step 8 above).
- Regex patterns are superuser-only (GUC context). An input bound alone is not a CPU bound. v1 therefore also rejects back-references, caps the pattern length and capture count (§4.2), and caps the number of comments examined per statement (16) and their total size (`scan_window`). The core regex engine already checks for interrupts, which allows cancellation but does not bound complexity. `scan_window` can be set per database, role or session, but only by a superuser or a role granted `SET` on it (PG 15+), so an ordinary role cannot raise the bound for its own statements.

### 6.12 Deployment requirement
The extension must be listed in `shared_preload_libraries`, **after** `pg_stat_statements` if both are used (§3.2), so enabling it requires a restart. Managed providers (RDS, Cloud SQL, Azure) only allow extensions on their allowlists, so users there cannot install it until a provider adds it. This is the main obstacle to adoption.

### 6.13 Exemplars for excluded high-cardinality keys
High-cardinality keys such as `traceparent` must not be grouped by (§6.1), but a link from an aggregate to one real trace is useful. An **exemplar** is the most recent value of such a key, stored per entry without becoming part of the key (item 20261005-091225-33; part of SQL version 1.0, §7).

- **Which keys:** only those listed in `exemplar_keys` (§4.1), a dedicated postmaster GUC (decided 2026-10-05): the denylist does not double as the exemplar list. A key may be in both, which is the usual setup for `traceparent`; a grouping key may also be listed. At most 8 keys, so the per-entry slots stay small and the slot index fits a byte.
- **Capture:** between steps 4 and 5 of the tag pipeline (§6.11), on the renamed key and the validated (step 2) value, independently of the allowlist/denylist. Per-extractor `keys` (step 3) still apply, as for any pair of that extractor.
- **Storage:** fixed per-entry slots after the counter ring (§5.1), one per key in `exemplar_keys` order, sized at startup from `exemplar_memory`. No dynamic shared memory.
- **Overflow: drop, not truncate** (implementer's choice, 2026-10-05 decision). A truncated trace id doesn't identify a trace, so a value longer than `_info().exemplar_value_bytes` is dropped, counted in `_info().exemplar_values_dropped`, and the slot keeps its previous value. Total exemplar memory, `_info().exemplar_shmem_bytes`, never exceeds `exemplar_memory`.
- **Writes:** the values captured at extraction travel with the statement's frame (inherited by nested statements with `nested_tags = inherit`, like tags) and are written by the record call, under the entry's spinlock with the call itself, on the fast path and the insert path alike: no extra lock. A slot with no value in this statement keeps its last value, so the exemplar is the latest value *seen*, not necessarily from the latest call. The value written is the one of the statement that recorded last, which under concurrency is the last to take the spinlock.
- **Reads:** a jsonb object `{key: value}` in the `exemplars` column of the stats views and functions (§7), `{}` when nothing is stored. Values are converted from the entry's encoding like tags. Visibility is that of tags (§6.11): `NULL` for other roles' rows without `pg_read_all_stats`, and with `showtags = false`.
- **Not saved** across restarts (§5.5), and zeroed by `_reset()` with the entries.
- **Off by default:** with `exemplar_keys = ''` nothing is captured, no memory is used, and `exemplars` is `{}`.

## 7. SQL interface (v1)

`sql/pg_stat_statement_context--1.0.sql` is **frozen** as of v1.0.0 (item 20261005-091225-29). Installations created from it must stay identical to new ones, so any later change to the SQL surface ships as an upgrade script (`pg_stat_statement_context--1.0--1.1.sql`, ...) with a `default_version` bump in the `.control` file, and its scripts are added to `sql/frozen.sha256` when that version is released. `scripts/check-frozen-sql.sh` (run in CI and by `docker/run-tests.sh`) fails if a listed script is edited in place.

Version 1.0 was never released before v1.0.0, so the SQL surface added on `main` after the first freeze (exemplars, item 20261005-091225-33, briefly version 1.1 with an upgrade script) was folded into `pg_stat_statement_context--1.0.sql` and its checksum re-recorded (owner decision, 2026-10-06, item 20261005-091225-29). The first release therefore ships a single install script and no upgrade scripts; the freeze applies from v1.0.0 on.

```sql
CREATE FUNCTION pg_stat_statement_context(
    showtags boolean DEFAULT true,
    merge_buckets boolean DEFAULT false,
    OUT bucket_start timestamptz, OUT userid oid, OUT dbid oid,
    OUT queryid bigint, OUT toplevel bool, OUT tags jsonb,
    OUT calls bigint, OUT total_exec_time float8,
    OUT calls_total bigint, OUT exec_time_total float8,
    OUT stats_since timestamptz, OUT exemplars jsonb)
RETURNS SETOF record ...;

CREATE VIEW pg_stat_statement_context AS
    SELECT * FROM pg_stat_statement_context(true, false);

-- One row per (db, user, queryid, toplevel, tags) across live buckets;
-- bucket_start is the oldest contributing bucket.
CREATE VIEW pg_stat_statement_context_totals AS
    SELECT * FROM pg_stat_statement_context(true, true);

-- Only the last closed bucket (current_bucket - 1), one row per entry with
-- calls in it.
CREATE FUNCTION pg_stat_statement_context_last_bucket(
    showtags boolean DEFAULT true,
    OUT bucket_start timestamptz, OUT userid oid, OUT dbid oid,
    OUT queryid bigint, OUT toplevel bool, OUT tags jsonb,
    OUT calls bigint, OUT total_exec_time float8,
    OUT calls_total bigint, OUT exec_time_total float8,
    OUT stats_since timestamptz, OUT exemplars jsonb)
RETURNS SETOF record ...;

CREATE VIEW pg_stat_statement_context_last_bucket AS
    SELECT * FROM pg_stat_statement_context_last_bucket(true);

CREATE FUNCTION pg_stat_statement_context_reset() RETURNS void ...;
REVOKE ALL ON FUNCTION pg_stat_statement_context_reset() FROM PUBLIC;

CREATE FUNCTION pg_stat_statement_context_info(
    OUT entries bigint, OUT max_entries bigint, OUT dealloc bigint,
    OUT reclaimed_entries bigint, OUT evicted_entries bigint,
    OUT dropped_records bigint,
    OUT buckets int, OUT bucket_seconds int, OUT oldest_bucket timestamptz,
    OUT current_bucket_start timestamptz,
    OUT last_closed_bucket_start timestamptz, OUT shmem_bytes bigint,
    OUT cap_shmem_bytes bigint, OUT invalid_tags bigint, OUT dropped_tags bigint,
    OUT heuristic_scans bigint, OUT regex_compile_failures bigint,
    OUT utility_missing_queryid bigint, OUT capped_tags bigint,
    OUT cap_table_full bigint, OUT stats_reset timestamptz,
    OUT stats_reset_epoch bigint, OUT exemplar_shmem_bytes bigint,
    OUT exemplar_value_bytes int, OUT exemplar_values_dropped bigint) ...;
```

These are the 1.0 definitions. The `exemplars` column (§6.13, item 20261005-091225-33) is the last column of both functions and so of all three views, and the three `exemplar_*` columns are the last ones of `_info()`.

Debug function (item -11, ships in 1.0):

```sql
CREATE FUNCTION pg_stat_statement_context_extract(
    query text, stmt_location int DEFAULT -1, stmt_len int DEFAULT 0)
RETURNS jsonb VOLATILE STRICT PARALLEL RESTRICTED ...;
REVOKE ALL ON FUNCTION pg_stat_statement_context_extract(text, int, int) FROM PUBLIC;
```

- Runs the hooks' extraction pipeline with the current GUC config and returns `tags`, `ntags`, `tagset_bytes`, `footer`, `heuristic`, `oom`, `stmt_start`, `stmt_end` (byte offsets), and this call's `invalid_tags`, `dropped_tags`, `heuristic_scans`, `regex_compile_failures`, `normalized_tags`, `normalize_failures`, `capped_tags`. Cardinality caps are only checked, never admitted, so calling it doesn't use up cap space.
- Restricted because it runs the regex engine on arbitrary input (CPU cost) and reveals the extractor configuration; superusers may `GRANT` it.
- Works when `enabled = off`; errors if the library isn't preloaded. `stmt_location = -1` means the whole string; out-of-range offsets error.
- Records nothing and doesn't touch pending stats, except regex compile failures, which are per-backend state and go to the shared counter.
- Statement ownership uses the hooks' `pssc_stmt_owned_range()` (same `scan_window` budget), so the result equals the hooks' for a single statement or the first statement of a string. Exception: for a later statement starting more than `scan_window` bytes in, the PG18 boundary cache (§6.5) can let the hooks see leading comments that this function doesn't.
- Output is escaped in `SQL_ASCII` databases (§6.11).

The column set is deliberately minimal (§5.1, decided 2026-10-05): `calls` and `total_exec_time` only (plus their per-entry monotonic totals, below). Rows, buffers, WAL, I/O timing, JIT, and min/max/mean/stddev come from `pg_stat_statements`, joined on `(userid, dbid, queryid, toplevel)`. `tags` is `jsonb` (decided 2026-10-05, §11 Q2), with string values (and `null` for values collapsed by the cardinality caps, §6.1).

`_info()` columns added on 2026-10-05: `evicted_entries` (§5.3), `dropped_tags` (tags dropped because the tag set would exceed `max_tags` or `max_tagset_bytes`, §4.1), and `regex_compile_failures` (lazy-compile failures, §4.2). Regex failures happen per backend, so they are flushed into a shared counter in the header. Added on 2026-10-06 (item -32): `capped_tags` (values collapsed to `null` by a cardinality cap) and `cap_table_full` (of those, the ones collapsed because the cap-tracking table was full). `shmem_bytes` covers the store only; `cap_shmem_bytes` is the exact size of the separate cap table (it is allocated even when caps are off). Added on 2026-10-06 (item 20261005-213120-1): `reclaimed_entries` (dead entries reclaimed) split out of `evicted_entries` (now live entries only), and `dropped_records` (§5.1, §5.3). Added on 2026-10-06 for exporters (item 20261006-010149-1, below): `bucket_seconds`, `current_bucket_start`, `last_closed_bucket_start` and `stats_reset_epoch`. Added for exemplars (item 20261005-091225-33, §6.13): `exemplar_shmem_bytes` (the exemplar slots' part of `shmem_bytes`: `max_entries` × the per-entry block, at most `exemplar_memory`; 0 when off), `exemplar_value_bytes` (the most bytes an exemplar value may take) and `exemplar_values_dropped` (values dropped as longer than that; zeroed by `_reset()`, not saved).

Exporter support (item 20261006-010149-1). Bucket gauges only become final once their bucket has closed, and no column used to grow monotonically, so Prometheus-style `rate()` was impossible and exporters had to guess the last closed bucket from the clock. v1 therefore adds:
- **Bucket metadata in `_info()`:** `bucket_seconds` (`bucket_interval`), `current_bucket_start` (start of the `current_bucket` watermark that the snapshot judged every slot against) and `last_closed_bucket_start` (the bucket before it, the newest that can no longer receive calls, §5.2). Boundaries fall on wall-clock multiples of the interval, because the epoch is the startup time rounded down. So until the first boundary after startup, the last closed bucket predates any data. The bucket in progress at startup closes at that boundary, which is less than one interval after startup. It is the first bucket with data, and it covers only part of an interval. `stats_reset_epoch` is `stats_reset` in whole Unix seconds (rounded down).
- **`_last_bucket`:** the SRF restricted to the slot holding `current_bucket - 1`, never merged. It is a separate C entry point (`pg_stat_statement_context_last_bucket_1_0`) sharing the SRF's code rather than a view filtering on `_info()`, so the bucket is chosen from the watermark observed at the start of the same scan: the result is one consistent bucket, and is closed because writers only write to the watermark (§5.2). A slot is shown only if it is still live at the watermark read after copying the entry, as in the SRF, so with `bucket_count = 1` the view is always empty. Visibility rules are the SRF's.
- **Per-entry monotonic counters** `calls_total`, `exec_time_total`, `stats_since` (§5.1) on every SRF row, so on all three views. They are per entry, so in the per-bucket view they repeat on each of the entry's rows and must not be summed there; `_totals` has them once per entry. They are like pgss's counters and its `stats_since` (PG17): they reset when the entry is created again (reset, reclamation, eviction), never when a bucket expires. An entry whose slots have all expired is hidden but keeps its counters until it is reclaimed.

SRF implementation (item -20): materialize mode, `STRICT VOLATILE PARALLEL SAFE`, C symbol `pg_stat_statement_context_1_0`. Under the shared lock only raw bytes are copied (key fields, encoding, live slots, and tags only when shown); encoding conversion, jsonb building and merging happen after the lock is released. Reading changes no entry data: it may only advance the `current_bucket` watermark (§5.2). Expired slots keep their contents until a writer rolls them over. Non-merged rows of an entry come out in bucket order.

`_info()` and `_reset()` (item -21):
- `buckets` is the configured `bucket_count`. `oldest_bucket` is the start of the oldest live slot of any entry (it equals `min(bucket_start)` in the view), or `NULL` when no slot is live. All values come from one snapshot under the shared lock. Every slot is judged against the one watermark the row reports as `current_bucket_start`. If the watermark moves during the scan, the scan is repeated, so `oldest_bucket` is never a bucket that has already expired at the row's own `current_bucket_start`. `shmem_bytes` is the exact size requested at startup.
- `_info()` first flushes the caller's pending extraction counters, so a session sees its own activity. It is callable by `PUBLIC`, like `pg_stat_statements_info`.
- `_reset()` (superuser-only by default) takes the exclusive lock, clears all entries, header counters and the cardinality-cap value sets, sets `stats_reset`, and discards the caller's own pending counters. Counts from statements running elsewhere land after the reset. A backend whose regex failed before the reset keeps that extractor disabled and doesn't count the failure again.
- Diagnostic counter flushes hold the store's shared lock, so a flush lands entirely before or after a reset. When nothing is pending, no lock is taken. At `ExecutorEnd` the flush reuses the record's lock hold (`pssc_store_record_with_stats()`).
- Both functions are `VOLATILE PARALLEL RESTRICTED`, because pending counters live only in the leader backend.

Bucket merging (`merge_buckets = true`) sums `calls` and `total_exec_time` across an entry's live slots (§5.2). Because the key has no bucket, each entry yields exactly one merged row, and `bucket_start` is its oldest live slot.

Counter semantics: `calls` counts completed executor instances, meaning `ExecutorEnd` was reached. That is not the number of Execute or FETCH messages. `total_exec_time` is in milliseconds and is measured the same way pgss measures it (`queryDesc->totaltime` for plannable statements, elapsed time around the chained call for utilities).

Typical use, joined to `pg_stat_statements` for query text. Filtering on `toplevel` avoids double-counting nested work (§6.4):

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

Other pgss metrics can be apportioned to a context approximately by its share of the statement's execution time, for example `s.shared_blks_read * c.total_exec_time / nullif(s.total_exec_time, 0)`. This is an estimate: it assumes the metric is proportional to time, and pgss accumulates since its last reset, while this extension covers only the live bucket window.

`toplevel` is only present in `pg_stat_statements` from PG14 onwards, which matches this extension's minimum supported version.

## 8. Roadmap

**v1.x — hardening**
- ~~**Per-key cardinality caps.**~~ Done (item -32, 2026-10-06; §4.1, §6.1, §6.11 step 8). Possible follow-up: decay of values unused for a while, so a long-running server doesn't keep old values' cap space until `_reset()`.
- ~~**Exemplars:** store the most recent value of a high-cardinality key, such as `traceparent`, per entry, so users can jump from an aggregate to a real trace without the key exploding. Exemplar keys are an explicit list in a dedicated GUC; the `exclude_tags` denylist does not double as that list. Total exemplar storage is bounded by a configurable memory cap (decided 2026-10-05).~~ Done (item 20261005-091225-33, ships in v1.0.0; §4.1, §5.1, §6.11, §6.13, §7): `exemplar_keys` and `exemplar_memory` (postmaster), the `exemplars` jsonb column, over-long values dropped and counted.
- ~~Optional background worker that reclaims dead entries (all slots expired) on idle systems.~~ Done (item 20261005-091225-34, 2026-10-06, ships in v1.0.0; §4.1, §5.3): `reclaim_worker` (postmaster, default `off`) and `reclaim_worker_interval`. It is not needed for correctness, since readers filter expired slots (§5.2).
- ~~**Persist stats across clean restarts** (dump/load like `pg_stat_statements.save`)~~. Done (item 20261005-091225-35, 2026-10-07, ships in v1.0.0; §4.1, §5.5). It follows pgss's lead (decided 2026-10-05):
  - the saved file is discarded on a file-format or extension-version mismatch;
  - if `max_entries` shrank, what fits is loaded and the rest is evicted (§5.3);
  - if `bucket_interval` or `bucket_count` changed, the file is discarded;
  - slots that expired during the downtime are dropped.

**v2 — more context sources**
- **`tags_override`:** context from a session or transaction GUC, e.g. `SET LOCAL pg_stat_statement_context.tags_override = 'controller=''users'',action=''show'''`. The value uses sqlcommenter syntax (`k='v',k2='v2'`, URL-encoded values). It **merges** with comment tags, and the override wins on key conflicts. Override tags go through the §6.11 pipeline (rename, allowlist/denylist, truncation). This works with prepared statements and with drivers that can't add comments. **Done (item -30):**
  - USERSET, default `''`. The check hook URL-decodes and validates the value into a flat extra blob; bad syntax, bad `%` escapes, NUL, or keys over 63 bytes are rejected at `SET` time (invalid encoding only when set from SQL/the client, since the database encoding is unknown for file, `ALTER ROLE` and `ALTER DATABASE` values).
  - Read at extraction (execution start), so a prepared statement uses the value at `EXECUTE`/Bind-Execute, and a `SET` statement is tagged with the previous value (as for appname).
  - Nested statements follow `nested_tags`: `inherit` copies the top-level tags; `scan` re-reads the GUC, so a function's `SET` clause or a `SET LOCAL` inside it applies to its nested statements.
  - Override-only statements count as tagged. `_extract()` uses the session's current override, with no separate output field.
  - Each backend caches the built tags keyed on the GUC and config generations, with a `memcmp` fallback.
  - The rename rule and the `scan` behavior were confirmed by the owner (2026-10-06).
- **Context from `application_name` (done, item -38):** a DSL extractor `appname(format=sqlcommenter|marginalia|regex)` (§4.2). Tags from comments win over `appname`-derived tags on key conflicts.
- **Activity view (done, item -39):** `pg_stat_statement_context_activity` (`pid`, `userid`, `dbid`, `queryid`, `state`, `tags`) shows each backend's current top-level tags, as a companion to `pg_stat_activity` (join on `pid`).
  - Each backend owns one shared slot (MaxBackends × `max_tagset_bytes`, ~75 kB at defaults), indexed by proc number (`MyProcNumber` on PG17+, `MyBackendId - 1` before; shims in `src/compat.h`) and requested in shmem_request/`_PG_init` like the store.
  - Writes are lock-free with a PgBackendStatus-style changecount: only the owner writes, in a critical section with barriers; readers retry until the counter is stable and even. Writers never wait.
  - A slot is published when a top-level frame starts executing (executor run or utility start: no active frame, nesting level 0, not inside the planner on any version, so the planner hook is installed on PG14–16 to count planning depth). It is marked `idle` when the statement ends and keeps the last tags, as `pg_stat_activity.query` does. `ExecutorFinish` only re-marks the row active if it still belongs to the same statement, so a portal that never ran (Bind→Close, Bind→Sync) or a cursor dropped at COMMIT doesn't replace it. The row is cleared by a top-level statement without tags resolved and at backend exit.
  - `userid` is the role the statement executes as (`GetUserId()` at publish time, not at Bind). Per §6.11, other roles' `queryid`, `state` and `tags` are NULL without `pg_read_all_stats`.
  - Cost: ~6 ns per publish/idle pair with typical tags (110 ns at 512 B); reading all slots ~0.5 µs (docs/benchmarks.md). No on/off GUC.
- **Value normalization rules (done, item -41):** per-key regex-replace rules, e.g. `/users/\d+` → `/users/:id`, set with `normalize` (§4.1). They run after rename and the allowlist/denylist, and before truncation and cardinality caps (§6.11 step 6). The `normalize_*` counters appear only in `_extract()`. Adding them to `_info()` is deferred to a later upgrade script.

**v3 — ecosystem**
- **Integrations (done, item -42):** `docs/integrations/` ships recipes for sql_exporter, postgres_exporter (its custom queries are deprecated upstream) and the OTel Collector contrib `sql_query` receiver; the `postgresql` receiver cannot run custom queries. It also has a Grafana dashboard and a least-privilege monitoring role. `scripts/test-integrations.sh` tests them end to end in Docker. Buckets expire and entries are evicted, so the recipes export per-second gauges over the last closed bucket rather than Prometheus counters. Only the `_info()` counters are exported as `_total`. Selected tag keys become `tag_<key>` labels, and the dashboard defaults to `toplevel = true`. Monotonic counters and bucket metadata are proposed in 20261006-010149-1.
- Packaging: PGXN, PGDG apt/yum, Homebrew, Docker images. Engage managed-cloud providers about adding the extension to their allowlists.
- Upstream conversation: propose a core hook or field for "statement comments" or a query-tag mechanism, which would benefit pgss and any similar extension.

**Rejected (2026-10-05).** Out of scope for a pgss companion, which stores only `calls` and `total_exec_time` per context (§5.1):
- `track_planning` (planning time): pgss already tracks planning per `queryid`.
- `utility_textid` for PG14/15: pgss has the same fragmented utility IDs (§6.6).
- Error and cancellation counts per tag set: adds hook-exception machinery for a metric outside the companion scope (§6.9).
- Wait-event sampling attributed to tags: needs a sampling worker and a separate store; not a per-statement counter.
- OS-level CPU and I/O (`getrusage`) per tag set: that is `pg_stat_kcache`'s job.
- Extractor `config_file`: GUCs plus `ALTER SYSTEM`/`pg_reload_conf()` suffice (§4.1, §11 Q3).

## 9. Testing strategy

- **Unit-ish regression (`pg_regress`)**: comment scanner edge cases (nested comments, dollar quotes, `$` inside identifiers, `E''`/`U&''` strings, `standard_conforming_strings = off`, unterminated comments, multibyte text, multi-statement ranges and trailing footers) exposed through a debug SQL function such as `pg_stat_statement_context_extract(text)`. Also cover each extractor format, DSL and regex errors (including back-references), the allowlist and denylist, `%00` and invalid encodings, and truncation on character boundaries.
- **TAP tests**: restarts, SIGHUP reconfiguration, bucket rollover (short `bucket_interval` set at startup), parallel queries, and prepared statements over the extended protocol (`\bind` in psql 16+, or pgbench `-M prepared`). Lifecycle and adversarial cases:
  - overlapping and suspended portals, cursors that are never run or are closed early
  - SPI errors caught in PL/pgSQL followed by successful work, failed portals
  - utility statements that end the transaction (`ROLLBACK`, `COMMIT` in procedures)
  - both `shared_preload_libraries` orders, and pgss/extension `track` and `track_utility` settings that differ
  - nested `toplevel` parity with pgss per version
  - stale-bucket insertion across a rollover, per-entry slot rollover, clock steps
  - a forced hash collision (debug hash override) followed by eviction and reinsertion
  - small-`max_entries` churn, with dead entries reclaimed before live ones
  - the optional reclaim worker (`027_reclaim_worker.pl`): with no query traffic it frees dead entries (`_info().entries` drops, `reclaimed_entries` grows, `dealloc` and survivors' usage unchanged), reloads its interval on SIGHUP, and with the default `off` no worker process exists and dead entries wait for an insert into a full table
  - persistence (`028_persist.pl`, §5.5): a fast restart keeps the entries, rings, usage, eviction-array order, counters, `stats_reset`, and epoch, and recording finds the reloaded entries. Expired slots and dead entries are dropped. A smaller `max_entries` evicts in §5.3 order, and a larger one loads everything. A smaller `max_tagset_bytes` skips what no longer fits. A changed `bucket_interval` or `bucket_count`, a version mismatch, a corrupt, truncated, or foreign file, `save = off`, an immediate shutdown, and a crash restart all start empty with a LOG line. The file is gone after every start.
  - a primary and a streaming standby (`033_standby.pl`, §5.5; item 20261008-065635-4): read-only tagged statements on the standby are recorded in its own store, and neither instance sees the other's entries. With `save = on` a fast standby restart (`DB_SHUTDOWNED_IN_RECOVERY`) saves and reloads its statistics, and an immediate one starts empty. After promotion the new primary keeps its in-memory history, records reads and writes, and a clean restart reloads it.
  - exemplars (`029_exemplars.pl`, §6.13): with `traceparent` both denylisted and in `exemplar_keys`, distinct values add no entries and `exemplars` shows the latest one; keys not listed are never stored; over-long values are dropped and counted, and `exemplar_shmem_bytes` stays within `exemplar_memory`; other roles' exemplars are `NULL` for unprivileged viewers; nested statements inherit them; 1.0 is the only available version (no update paths) and a fresh install has the columns. Unit tests (`test_tagset`) cover the capture step.
  - cross-database encodings, including `SQL_ASCII`
  - visibility for unprivileged roles, and `REVOKE` on reset
- **pg_regress suite** (`make installcheck`: smoke, guc, extract, normalize, appname, tags_override) runs in a UTF8, no-locale database. Server-level GUCs are changed with `ALTER SYSTEM` + `pg_reload_conf()` and an include file that waits until the new values are visible. TAP 013 checks that `_extract()` leaves the store and counters unchanged, the `SQL_ASCII` escaping, and debug/hook parity. Regression output that contains characters whose psql display width varies between minor releases (for example emoji outside the last column) uses `\pset format unaligned`, which still compares the exact bytes.
- **pgss detection** (`test/perl/PsscTest.pm`, on prove's include path via the Makefile): the TAP tests find `pg_stat_statements` in `pkglibdir` with any module suffix (`.so`; `.dylib` on macOS from PG16; `.dll`), and the load-order spelling variants of 014 use that suffix. Without it, the parity checks are skipped and 012/013 take the no-pgss paths (on PG14–16 a CALL/DO then nests per this extension's own utility settings). `docker/run-tests.sh` sets `PSSC_REQUIRE_PGSS=1`, which turns a missing pgss into a test failure, since every harness server (PGDG images, source builds, macOS) installs it. `PSSC_TEST_WITHOUT_PGSS=1` treats pgss as absent, to run the no-pgss paths where it is installed.
- **Harness source builds** (`docker/Dockerfile.source`) install `pg_stat_statements` too, so pgss parity checks run on them. They install bison/flex (needed by PG17+ tarballs), download the release tarball and its official `.sha256` to files, and verify the checksum before extracting; any download or checksum failure fails the build.
- **pgss parity** (`017_lifecycle.pl`, item -22): with matching settings, the per-`(userid, dbid, queryid, toplevel)` `calls` equal pgss exactly. `total_exec_time` is exactly equal for plannable statements, which read the same `totaltime`. For utilities ours is at least pgss's, because our hook wraps theirs. Other behaviour it pins down:
  - A cursor left open is recorded as top level at transaction end, as pgss does.
  - A bound portal that is never executed counts one call.
  - With differing settings, each side records according to its own settings.
  - Releases before upstream `8700851352a8` (14.0–14.9, 15.0–15.4, 16.0) re-parse a cached utility statement when its saved search_path no longer matches. That happens, for example, after the session's first temp table, and the re-parsed statement gets a fresh queryId that both pgss and this extension count. The test detects this at runtime instead of checking minor versions.
- **Store and SQL surface through the hooks** (items -23/-24): `018_store_reconfig.pl` exercises restart/resize, SIGHUP reconfiguration, rollover, stale buckets, clock steps, eviction and pgbench stress through real statements, pinning the debug clock wherever short buckets would otherwise make reads racy. `019_sql_surface.pl` runs the SQL examples and column tables from `docs/sql-interface.md` and §7 against the catalog and pgss, so incompatible doc changes fail the tests.
- **CI matrix** (`.github/workflows/ci.yml`, on push/PR): the Linux cells run the local harness itself:
  - `scripts/docker-test.sh N` (PGDG, 14–18).
  - `--assert N`: a source build with `--enable-cassert --enable-tap-tests`, 14–18.
  - `--valgrind 18`: the server runs under Valgrind with `src/tools/valgrind.supp` and `-DUSE_VALGRIND` for the LOAD checks and the pg_regress suite; any Valgrind error fails the cell.

  macOS builds 14–18 with `docker/build-postgres.sh` and runs `docker/run-tests.sh` on the host. The harness skips `worktrees/` when it copies the sources and stops its server on any failure, so a failed host run doesn't leave a postmaster on `PGPORT`; `scripts/test-run-tests.sh` checks both on the host first. Source builds are cached, keyed on release, flavor and a hash of the build scripts. A separate job runs `scripts/check-version-guards.sh` and `scripts/check-frozen-sql.sh` (§7); `docker/run-tests.sh` runs both too. TAP tests use the PG15+ `PostgreSQL::Test::*` names; PG14 installs them as aliases only from 14.6, so 14.0–14.5 cannot run the TAP suite and the harness says so explicitly.
- **Benchmarks** (`bench/run.sh [--major N] [--quick]`, item -26): pgbench runs inside one Docker container and compares against pgss alone in these setups:
  - no comment;
  - appended and prepended comments;
  - 10,000-element `IN` lists (heuristic and exact scans, `stmt_len` > 0 and = 0);
  - 1 s buckets (latency within ±5 ms of a boundary);
  - sustained eviction.

  Rounds are paired in ABBA order. The run fails if a setup recorded nothing, evicted nothing or never rolled a bucket. Results are in `docs/benchmarks.md`. Steady-state overhead is within the noise of a Docker VM, and bucket boundaries add no latency. Under sustained churn at `max_entries=10000`, eviction raised p99 by 2.5–5× because each pass sorted every entry under the exclusive lock. Item 20261006-043919-1 replaced the sort with partial selection in a single scan (pass ~40% faster; Δp99 +196% → +115% on PG 18). The rest is the scan of ~10,000 entries itself; 20261006-075124-1 tracks batching passes.
- **Fuzzing** (`fuzz/`, item -25): libFuzzer targets for the code that needs no server:
  - the comment scanner in every position mode (`fuzz_scan`);
  - the SQLCommenter and marginalia parsers;
  - the tag-set pipeline (`fuzz_tagset`).

  Besides running under ASan/UBSan, each target checks invariants:
  - results stay within the input;
  - comment spans are whole and re-scan to themselves;
  - comment budgets give a prefix of the unbounded result;
  - tag limits, sorted unique keys and valid encoding hold;
  - repeated calls give the same result;
  - each byte is read a bounded number of times.

  The same targets run under a standalone driver in `make unittest`. `fuzz/run-libfuzzer.sh` runs libFuzzer in Docker. The regex extractor depends on backend allocators, `pg_wchar` and collation code, so `fuzz/sql/run.sh` fuzzes it at the SQL level through `pg_stat_statement_context_extract` against the assert or Valgrind server. It predicts which patterns the check hook will accept, compares tags with a `regexp_matches` oracle, and detects crashes and assertion failures. CI runs a short smoke of both. Six bugs were injected on purpose and the harnesses caught all of them (`fuzz/README.md`).

## 10. Repository layout (proposed)

```
pg_stat_statement_context/
├── Makefile / meson.build          # PGXS
├── pg_stat_statement_context.control
├── sql/pg_stat_statement_context--1.0.sql   # frozen at v1.0.0 (§7)
├── sql/frozen.sha256               # checksums of released scripts
├── src/
│   ├── pg_stat_statement_context.c # _PG_init, hooks
│   ├── compat.h                    # PG14–18 shims
│   ├── guc.c                       # GUCs + DSL parser
│   ├── scan.c                      # comment scanner (backend-independent)
│   ├── extract.c                   # sqlcommenter / marginalia / regex
│   ├── context.c                   # execution frames, active-frame tracking
│   ├── reclaim.c                   # optional dead-entry reclaim worker
│   └── store.c                     # shmem HTAB, buckets, eviction
├── test/{sql,expected,t}/          # pg_regress + TAP
├── test/perl/PsscTest.pm           # shared TAP helpers (pgss detection)
├── fuzz/
└── DESIGN.md
```

## 11. Open questions

All seven questions below were resolved by the project owner on 2026-10-05. They are kept, with their resolutions, for the record. Remaining undecided points are tracked as open questions on individual tasks in `BACKLOG.md`.

1. ~~Should an empty tag set be recorded by default?~~ **Resolved: no.** Untagged statements are skipped by default (`untagged = skip`, §4.1), so untagged traffic doesn't consume entries. `untagged = record` remains available.
2. ~~`jsonb` for `tags`, or fixed columns for a configured set of keys?~~ **Resolved: `jsonb`** (§7).
3. ~~Should the extractor DSL live in one GUC or in a separate config file?~~ **Resolved: GUCs only.** The from-SQL path is `ALTER SYSTEM SET ...; SELECT pg_reload_conf();` (§4.1). The `config_file` idea is rejected (§8).
4. ~~Is a separate `utility_textid` (§6.6) worth adding for PG14/15?~~ **Resolved: not worth it**; pgss has the same PG14/15 behavior (§6.6, §8).
5. ~~Key layout: `bucket_id` in the key versus one entry per (query × context) holding a ring of per-bucket counters?~~ **Resolved: per-entry counter ring** (§5.1–§5.4). With only `calls` and `total_exec_time` stored, the ring costs about 24 bytes per bucket, and the layout avoids re-inserting entries at every boundary and makes capacity count combinations.
6. ~~Error counting (§6.9): deduplication rules and separate cancellation counts?~~ **Moot:** per-tag-set error counts were dropped as out of scope (§6.9, §8).
7. ~~Should a load-order violation (§3.2) be a `WARNING` or disable utility tracking?~~ **Resolved: `WARNING` only** (§3.2).

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
