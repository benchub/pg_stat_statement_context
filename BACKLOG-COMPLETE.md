# pg_stat_statement_context — Completed backlog items

Finished items moved here from BACKLOG.md (see CLAUDE.md).

### 20261005-091225-1: Project skeleton and PGXS build

**Description:** Create the repository layout from §10.
- A PGXS `Makefile` (`MODULE_big`, `OBJS` for `src/*.c`, `EXTENSION`, `DATA`, `REGRESS` with `--inputdir=test`, `TAP_TESTS`, and a temp config that preloads the library).
- `pg_stat_statement_context.control` (`default_version = '1.0'`) and a stub `sql/pg_stat_statement_context--1.0.sql`.
- `src/pg_stat_statement_context.c` with `PG_MODULE_MAGIC` and a `_PG_init` that does nothing unless `process_shared_preload_libraries_in_progress` (§6.12) and that calls `EnableQueryId()` (§2).
- Empty stubs for `guc.c`, `scan.c`, `extract.c`, `context.c`, and `store.c`.
- The `test/{sql,expected,t}` directories, a trivial smoke test, and `.gitignore`.

`meson.build` is optional (§10 lists "Makefile / meson.build"), and PGXS is the required path.

**Acceptance criteria:**
- `make && make install` succeeds against at least one of PG14–18 locally. Task 20261005-091225-3 checks all of them.
- With the library preloaded, the server starts cleanly and `CREATE EXTENSION pg_stat_statement_context` works.
- Loading the library without preloading it doesn't crash.
- `make installcheck` runs the smoke test and passes.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-2: `compat.h` PG14–18 version shims

**Description:** Collect the version differences from §6.10 in `src/compat.h`:
- the `ExecutorRun` signature (`execute_once` was removed in PG18)
- the `ProcessUtility` signature
- shared-memory requests: `RequestAddinShmemSpace`/`RequestNamedLWLockTranche` in `_PG_init` on PG14, `shmem_request_hook` on PG15+
- a helper that allocates GUC `extra` with `malloc` on PG14/15 (freed with `free()`) and `guc_malloc` on PG16+ (§4.2)
- regex allocation context handling (PG14/15 `malloc`, PG16+ `palloc`)
- the rows source: `es_processed` on PG14/15, `es_total_processed` on PG16+
- availability macros for buffer, WAL, I/O-timing, and JIT fields
- `MarkGUCPrefixReserved` (PG15+) vs `EmitWarningsOnPlaceholders` (PG14)
- `InitMaterializedSRF` (PG15+) vs a hand-rolled materialize mode on PG14

Other files should put version-dependent code behind these macros or helpers rather than scattering `PG_VERSION_NUM` checks.

**Acceptance criteria:**
- The header compiles warning-free with `-Wall` on PG14, 15, 16, 17, and 18.
- Each shim has a one-line comment naming the versions it covers.
- No `PG_VERSION_NUM` checks appear outside `compat.h` unless a comment justifies them.

**Depends on:** 20261005-091225-1
**Open questions:** none
**Status:** done

### 20261005-091225-4: Comment scanner: forward lexer

**Description:** Implement `src/scan.c`/`scan.h` (§3.1 item 1). This is a backend-independent, single-pass state machine with no `palloc` and no `elog`. It reports comment spans in a byte range `[start, end)` of a `const char *`, following `scan.l`:
- `'strings'` with `''` doubling, and backslash escapes when `standard_conforming_strings = off`. The setting is passed in as a parameter (§6.2).
- `E''` strings (always backslash-escaped), `U&''` strings, and `"quoted identifiers"`, including `U&""`.
- `$tag$...$tag$` dollar quotes, using `scan.l`'s tag-character rules, and `$1` parameters. A `$` inside an identifier, as in `a$b$`, does not start a dollar quote.
- Nested `/* /* */ */` comments and `--` line comments.
- An unterminated comment or string produces no span for the incomplete comment.

Output goes to a caller-provided fixed array of `(offset, len)` spans, capped at 16 comments (§6.11), with a flag that reports truncation. The total size of examined comments is capped by a parameter (`scan_window`). Also expose a helper that reports whether a byte range contains only `;`, whitespace, and comments. Task 20261005-091225-5 uses it for the trailing-footer rule (§6.5).

*Design note:* server encodings are ASCII-safe, so lexing byte by byte is correct for multibyte text.

**Acceptance criteria:**
- A small standalone C test driver (no server; it can later seed the fuzz corpus) covers every construct listed above, plus multibyte UTF-8 and unterminated constructs. All cases pass.
- The scanner is O(n), performs no heap allocation, and never reads outside `[start, end)`.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-5: Statement ranges and positional (windowed) scanning

**Description:** Build the statement-level scanning API on top of the lexer (§6.2, §6.5).

Resolve the statement range from `stmt_location`/`stmt_len`:
- A location of `-1` means the whole string, as in `CleanQuerytext`.
- A length of `0` means to the end of the string. This is the only case that needs `strlen`.

Choose the scan mode:
- If the range fits within `scan_window`, lex it exactly from the front.
- If it is longer:
  - `prepend` lexes only the first `scan_window` bytes.
  - `append` trims whitespace and `;` within the tail window only, requires a closing `*/`, and walks backwards to the matching `/*` while tracking nesting depth. A trailing `--` comment, or a comment that crosses the window start, yields nothing. Results from this path are flagged *heuristic*, which feeds `_info().heuristic_scans`.
  - `any` does a full forward scan.

Provide the trailing-footer fallback separately. It returns comment spans after the statement's range only when the rest of the string contains nothing except `;`, whitespace, and comments. The caller uses these spans only if the statement's own range produced no tags. The API never returns half a comment.

**Acceptance criteria:**
- Unit tests cover:
  - `SELECT 1 /*a*/; SELECT 2 /*b*/`: each statement gets only its own comment.
  - `SELECT 1; SELECT 2; /*controller:x*/`: only the last statement gets the footer.
  - statements longer than `scan_window` in `append`, `prepend`, and `any` modes
  - a string literal that ends in `*/` on the tail path, which must be flagged heuristic
  - a comment that crosses the window start, which yields nothing
  - `stmt_len = 0` and `stmt_location = -1`

**Depends on:** 20261005-091225-4
**Open questions:** none
**Status:** done

### 20261005-091225-6: SQLCommenter and marginalia pair parsers

**Description:** Write pure-C parsers with no backend dependencies (§4.2, §9 fuzzing) that turn one comment body into raw `(key, value)` pairs:
- **SQLCommenter:** `key='value',key2='value2'`. Values are URL-decoded when `url_decode=on` (the default), and `\'` is unescaped.
- **marginalia:** pairs are split on `pair_sep` (default `,`), and each pair is split on the **first** `kv_sep` (default `:`), so `line:app/models/u.rb:12` keeps its colons.

Decoded output goes into a caller-provided bounded buffer. Decoded NUL bytes (`%00`) are kept and flagged so the pipeline can reject them (§6.11). Malformed pairs are skipped and counted, and the parsers never fail.

*Design note:* §4.2 doesn't specify whitespace handling. Proposed default: trim ASCII whitespace around the comment body and around each pair, then document it.

**Acceptance criteria:**
- Unit tests cover:
  - the examples in §4.2
  - values that contain colons
  - escaped quotes
  - valid and invalid `%` escapes, including `%00`
  - empty keys and values
  - custom `kv_sep`/`pair_sep`
  - `url_decode=off`
- The parsers allocate nothing beyond the caller buffer.
- Each parser has an entry point suitable for libFuzzer.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-7: Core GUCs

**Description:** In `src/guc.c`, define every GUC in §4.1 with its default, context, and unit:
- `superuser` maps to `PGC_SUSET`, `postmaster` to `PGC_POSTMASTER`, and `sighup` to `PGC_SIGHUP`.
- `bucket_interval` uses seconds as its unit, and `scan_window` uses bytes.
- `track`, `nested_tags`, and `untagged` are enum GUCs.
- Numeric GUCs get sane bounds, for example `bucket_count >= 1`, keys up to 63 bytes, and a minimum for `max_tagset_bytes`.
- `tags` and `exclude_tags` are parsed in a `check_hook` into a flat `extra` blob using the compat allocator, with `'*'` handled. The `assign_hook` installs the pointer and bumps the backend-local config generation, and it can't fail (§3.1 item 6).
- Postmaster GUCs are defined only during `shared_preload_libraries` loading, and the prefix is reserved.

**Acceptance criteria:**
- Every GUC appears in `pg_settings` with a description.
- A non-superuser `SET` of a `superuser` GUC fails.
- `ALTER SYSTEM` plus a reload changes `sighup` GUCs, and postmaster GUCs need a restart.
- Invalid values are rejected with a clear error, and the previous value stays active.

**Depends on:** 20261005-091225-1, 20261005-091225-2
**Open questions:** none
**Status:** done

### 20261005-121022-1: marginalia: count malformed pairs when `kv_sep` starts with whitespace

**Description:** Split off from 20261005-091225-6 after its second review round. In `src/pairs.c` (~433–443, 470–475), the `blank` check looks only at the first byte of a matched `kv_sep`. Advancing by `kvlen` then skips its other bytes, so with a separator that starts with whitespace (e.g. `kv_sep=" :"`), a segment such as `" :"` is treated as blank. The result is `npairs=0, nmalformed=0` where it should be `nmalformed=1`. Fix this by counting every consumed separator byte when deciding whether a segment is blank. Also consider having the extractor DSL (-8) reject or trim separators that have leading or trailing whitespace.

**Acceptance criteria:**
- `kv_sep=" :"` with body `" :"` gives `nmalformed=1`, and a genuinely whitespace-only segment is still ignored and not counted.
- The marginalia fuzz differential check covers separators that start with whitespace, including multi-byte ones.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-124839-1: GUC test module: old-glibc and 32-bit portability fixes

**Description:** Split off from 20261005-091225-7 after its second review round. Both fixes are in test-only code in `test/modules/pssc_guc_test/pssc_guc_test.c`:
- **~215–216:** the `mallinfo2()` leak probe is guarded only by `__GLIBC__`, but `mallinfo2` arrived in glibc 2.33, so the module fails to compile on older glibc. Add `__GLIBC_PREREQ(2, 33)` (or a feature check), and return NULL so the TAP test explicitly skips the measurement.
- **~201:** the SQL wrapper narrows `bigint` arguments to `size_t` before checking that they fit. On 32-bit, the `(4294967296, 0)` case in `test/t/003_guc.pl:260` becomes `(0, 0)` and returns 12 instead of NULL. Reject positive arguments larger than `SIZE_MAX` before narrowing, keeping the `-1` → `SIZE_MAX` convention.

**Acceptance criteria:**
- The module compiles against glibc older than 2.33 (e.g. a Debian bullseye or Ubuntu 20.04 based image), and the leak check is reported as skipped there.
- The boundary test passes on a 32-bit build (e.g. an i386 Docker image), or the narrowing is proven unit-wise.
- The existing PG14–18 harness still passes.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-8: Extractor DSL parser and GUC check/assign hooks

**Description:** Implement the `pg_stat_statement_context.extractors` grammar from §4.2 in the `check_hook`.

Accepted parameters:
- Extractor names: `sqlcommenter`, `marginalia`, `regex`.
- Common parameters: `position`, `keys` (`|`-separated), `rename` (`old:new|...`), `merge`.
- Format-specific parameters: `url_decode`; `kv_sep` and `pair_sep`; `pattern` and `keys` for `regex`.
- Values may be quoted, with `''` escaping, as in the regex example.

Reject the following with `GUC_check_errdetail` and keep the previous config:
- unknown names or parameters
- duplicate parameters
- invalid values

Validate `regex` extractors by test-compiling each pattern with `pg_regcomp`, using `REG_ADVANCED` and `C_COLLATION_OID`, then freeing it with `pg_regfree`. Reject a pattern when any of these is true:
- it is longer than 1 kB
- it uses back-references (`re_info & REG_UBACKREF`)
- it has more than `max_tags` capture groups

Return the parsed form as one flat, pointer-free blob (offsets, not pointers) allocated through the compat helper. The `assign_hook` only stores it and bumps the generation.

*Design note:* the number of regex `keys` should equal the number of capture groups, and a mismatch is an error.

When `position` is omitted, the parser fills in the per-extractor default: `append` for `sqlcommenter` and `marginalia`, `any` for `regex` (§4.2).

**Acceptance criteria:**
- Every example in §4.2 parses.
- Omitting `position` yields `append` for `sqlcommenter`/`marginalia` and `any` for `regex` in the parsed blob, including for the default `'sqlcommenter, marginalia'`.
- Each class of malformed input is rejected with a specific message, and the old config survives a bad `SIGHUP`.
- The blob contains no pointers and is freed correctly by guc.c on PG14/15 and PG16+, with no leak under Valgrind.

**Decisions:**
- 2026-10-05: The default `position` when omitted is per extractor: `sqlcommenter` = `append`, `marginalia` = `append`, `regex` = `any`.

**Depends on:** 20261005-091225-7
**Open questions:** none
**Status:** done

### 20261005-091225-9: Tag-set canonicalization pipeline and extractor chain

**Description:** In `src/extract.c`, turn `(sourceText, stmt range, config blob, database encoding, standard_conforming_strings)` into a canonical tag set (§3.1 item 2, §4.2, §6.11):

1. For each extractor, scan with its `position` (task 20261005-091225-5) and parse its comments (task 20261005-091225-6, or task 20261005-091225-10 for `regex`).
2. Run every pair through the §6.11 order:
   1. decode
   2. reject values that contain NUL or fail `pg_verify_mbstr`
   3. apply the per-extractor `keys`, matching the **original** key names
   4. apply `rename`
   5. apply the global allowlist, or the denylist when `tags = '*'`
   6. drop keys longer than 63 bytes
   7. truncate values to `max_tag_value_len` with `pg_mbcliplen`
3. Combine extractors: the first one that produces at least one tag wins, unless `merge=on`.
4. Sort the tags by key and serialize them as `k\0v\0...` within `max_tagset_bytes`. Tags that don't fit are dropped in allowlist order. Compute `tags_hash`.
5. Apply the trailing-footer fallback (§6.5) when the statement's own range produced no tags.

Count invalid tags, dropped tags, and heuristic scans in a backend-local stats struct, which the recording path flushes later. No input may raise an error. All work happens before any lock, in a short-lived memory context.

*Design notes* (propose and document; non-blocking):
- With `merge=on`, the earlier extractor wins on a duplicate key.
- When `tags='*'`, overflow tags are dropped in reverse sorted-key order.
- With `position=any`, every comment is parsed, and the first occurrence of a key wins.

**Acceptance criteria:**
- The same tags in a different order produce byte-identical output and hash.
- The serialized size never exceeds `max_tagset_bytes`.
- Malformed input never raises an error.
- A per-extractor `keys` list naming an original key keeps it even when `rename` changes its name, and naming only the renamed key does not keep it.
- Each §6.11 step is covered by the regression suite in task 20261005-091225-11.

**Decisions:**
- 2026-10-05: A per-extractor `keys` allowlist matches the **original** key names and is applied before `rename`. The global allowlist still applies after `rename`.

**Depends on:** 20261005-091225-5, 20261005-091225-6, 20261005-091225-8
**Open questions:** none
**Status:** done

### 20261005-091225-27: Validate prepared-statement behavior of target drivers

**Description:** §6.3 requires this check before v1. Test whether each target driver reuses one prepared statement across different comment contexts, or whether it includes the comment in its statement-cache key or re-Parses each time:
- Rails with `pg` (marginalia and `query_log_tags`, `prepared_statements` on)
- `pgx` (default statement cache)
- JDBC (`prepareThreshold`)
- psycopg 3 (`prepare_threshold`)
- pgbouncer in transaction mode, including `max_prepared_statements`

Use server logging (`log_min_duration_statement = 0`, which logs Parse/Bind/Execute separately), so the extension isn't needed. Write up the results with the versions tested and reproduction scripts.

**Acceptance criteria:**
- Each driver has a finding with the version tested and a reproduction script.
- An explicit go/no-go decision is recorded on moving `tags_override` (task 20261005-091225-30) into v1.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-10: Regex extractor runtime

**Description:** Implement the runtime half of the `regex` extractor (§4.2):
- Each backend compiles patterns lazily, on first use after a config-generation change, into a private memory context that it owns, and frees the old ones with `pg_regfree`.
- Patterns use `REG_ADVANCED` and `C_COLLATION_OID`, and are applied **only to comment text**, honoring `position`.
- Comment bytes are converted to `pg_wchar` before matching, and capture offsets are mapped back to byte offsets. Capture group *n* maps to key *n*.
- Raw pairs go into the pipeline from task 20261005-091225-9.
- If lazy compilation fails (for example, out of memory), the extractor is disabled for that backend and the failure is counted. The statement never fails.

**Acceptance criteria:**
- The `svc=(\w+)\s+op=(\w+)` example from §4.2 extracts `service` and `operation`.
- Captures in multibyte comments map to the correct bytes.
- A config reload recompiles the patterns and frees the old ones, with no leak under Valgrind.
- An injected compile failure disables only that extractor, increments a counter, and lets the statement succeed.

**Depends on:** 20261005-091225-8, 20261005-091225-9
**Open questions:** none
**Status:** done

### 20261005-091225-12: Counters (`calls`, `total_exec_time`): accumulation and bucket merge

**Description:** Define the per-bucket counter slot from §5.1 (`ctxSlot`: `bucket_id`, `calls`, `total_exec_time`) and the pure functions that work on it. The extension is a pg_stat_statements companion, so no other counters are stored (§5.1, §7).
- Initialize a slot, and reset/relabel it for a new `bucket_id` (used by the per-entry ring rollover in task 20261005-091225-14).
- Accumulate one call plus elapsed milliseconds: from `queryDesc->totaltime` for the executor, and from a measured duration for utilities.
- Merge slots for `merge_buckets` (§7): sum `calls` and `total_exec_time`, and keep the oldest contributing `bucket_id` for `bucket_start`.
- Provide the pgss-style `usage` update used for eviction ordering (task 20261005-091225-15). `usage` is internal and not exposed.

**Acceptance criteria:**
- The code compiles warning-free on PG14–18 and uses no version-specific counter fields.
- Unit tests (or a test hook) show that accumulation and merge produce exact sums, that merge picks the oldest `bucket_id`, and that relabeling a slot zeroes its counters.
- Executor `total_exec_time` uses the same source and units (ms) as pgss, verified end to end by task 20261005-091225-22.

**Decisions:**
- 2026-10-05: The extension is a companion to pg_stat_statements, not a replacement. Per (queryid × context) entry it stores **only** `calls` and `total_exec_time`. Rows, blocks, WAL, I/O timing, JIT, min/max/mean/stddev, and so on are left to pgss, and users join on `(userid, dbid, queryid, toplevel)`. Rationale: `calls` alone can't apportion load across contexts when per-context cost differs, and `total_exec_time` is the minimum needed for attribution.
- 2026-10-05: The question of how to expose counters missing on older versions is moot (both stored counters exist on PG14–18). General policy, following pgss: version-unavailable columns are omitted rather than exposed as `NULL`.

**Depends on:** 20261005-091225-2
**Open questions:** none
**Status:** done

### 20261005-091225-13: Shared store core (shmem, HTAB, key, locking)

**Description:** Implement `src/store.c` (§3.1 item 4, §5.1, §5.4). The layout is one entry per (query × context) holding a per-bucket counter ring; `bucket_id` is **not** part of the key.

Sizing and setup:
- Compute `keysize` from `max_tagset_bytes`, and `entrysize` from `keysize` plus `bucket_count` ring slots (task 20261005-091225-12).
- Size shared memory as `hash_estimate_size(max_entries, entrysize)` plus the header, using `add_size`/`mul_size`, and request it through the compat path with an LWLock tranche.
- In `shmem_startup_hook`, create or attach the header and an HTAB with `init_size = max_size = max_entries` and custom `HASH_FUNCTION`/`HASH_COMPARE` callbacks.

Keys and entries:
- The key is `(dbid, userid, queryid, toplevel, tags_len, tags_hash, tags[])`. Build keys by `memset`-ing the whole key to zero first.
- The hash combines the fixed fields with `tags_hash`. The compare checks the fixed fields and `tags_len`, then `memcmp`s only the used tag bytes.
- Each entry has its own spinlock, stores the database encoding, `last_bucket`, `usage`, and `bucket_count` slots.

Header counters:
- `entries`, `dealloc`, `evicted_entries`, `invalid_tags`, `dropped_tags`, `regex_compile_failures`, `heuristic_scans`, `utility_missing_queryid`, and `stats_reset`.

Recording:
- `store_record(key, bucket_id, elapsed)` looks up an existing entry under the shared lock and, under the entry spinlock, adds one call and the elapsed time to slot `bucket_id mod bucket_count`, relabeling the slot first if it holds an older `bucket_id`.
- On a miss, it releases the lock, takes the exclusive lock, repeats the `HASH_ENTER` lookup, and enforces `max_entries` itself. Until task 20261005-091225-15 lands, a full table simply drops the record and counts it.

Support code:
- A reset routine.
- A debug-only hash override, via an assert build or a developer GUC, to force collisions (§9).
- Until task 20261005-091225-14 lands, the `bucket_id` is supplied by the caller.

**Acceptance criteria:**
- The server starts with the default settings and with boundary values for `max_entries`, `max_tagset_bytes`, and `bucket_count`.
- The reported `shmem_bytes` equals the requested size.
- Concurrent `pgbench` recording loses no updates (the sum of `calls` matches the executed statements).
- Recording the same key into different `bucket_id`s uses one entry with separate slots, not separate entries.
- With forced collisions, distinct tag sets stay separate.
- The entry count never exceeds `max_entries`.

**Decisions:**
- 2026-10-05 (§11 Q5): Key layout is one entry per (query × context) holding a per-bucket counter ring, not `bucket_id` in the key. This is cheap because each slot holds only `calls` and `total_exec_time` (about 24 bytes per bucket). Capacity counts (query × context) combinations, independent of `bucket_count`.

**Depends on:** 20261005-091225-2, 20261005-091225-7, 20261005-091225-12
**Open questions:** none
**Status:** done

### 20261005-091225-14: Time buckets and lazy per-entry ring rollover

**Description:** Implement §5.2. The header stores the epoch, `bucket_interval`, `bucket_count`, and `current_bucket`. Every backend computes `bucket_id = floor((now - epoch) / interval)` as a signed `int64`.

Header advance:
- `current_bucket` changes only under the exclusive lock.
- A writer whose computed ID is newer releases the shared lock, takes the exclusive lock, re-checks the header, and advances `current_bucket` if it is still behind. Advancing touches no entries.

Clamping:
- A write ID older or newer than `current_bucket` is clamped to `current_bucket` while the lock is held.
- `current_bucket` never decreases when the clock moves backwards.
- A forward jump larger than the ring makes every slot stale.

Per-entry ring rollover:
- Under the entry spinlock, the writer uses slot `bucket_id mod bucket_count`. If the slot holds an older `bucket_id`, it is zeroed and relabeled (task 20261005-091225-12) before the counters are added. `last_bucket` is updated.

Readers:
- Provide a helper that tells readers whether a slot is live, i.e. its `bucket_id` is within `[current - bucket_count + 1, current]` of the clock-derived current bucket, without depending on writers. An entry with no live slot is *dead* (reclaimed by task 20261005-091225-15).

Testing:
- Provide a debug-only clock offset so that clock steps can be tested (§9).
- Executions are attributed to the bucket in which they complete (§5.2 semantics).

**Acceptance criteria:**
- With a 1 s interval, an entry's slots roll over and old counts disappear from readers.
- Readers hide expired slots even when no writes happen.
- A stalled writer's stale ID is clamped.
- A backward clock step doesn't regress `current_bucket`, and a forward jump hides everything.
- A concurrent stress run at bucket boundaries never writes into an expired slot. Assert builds verify the ring invariants (each slot's `bucket_id` ≤ `current_bucket` and ≡ its index mod `bucket_count`).

**Depends on:** 20261005-091225-13
**Open questions:** none
**Status:** done

### 20261005-091225-15: Eviction under pressure (dead entries first, then pgss-style)

**Description:** Implement §5.3. When an insert finds the table at `max_entries`, under the exclusive lock:
1. Reclaim dead entries: those whose `last_bucket` is older than the live window, so all slots have expired.
2. If that frees less than ~5% of `max_entries`, evict further entries ordered by `last_bucket` (oldest first), then by `usage` (lowest first), until ~5% is free. Decay `usage` pgss-style.
3. Increment `dealloc` (once per pass) and `evicted_entries` (per entry).

The insert that triggered eviction must then succeed.

**Acceptance criteria:**
- In a small-`max_entries` churn test, the entry count never exceeds the limit and the counters increase.
- Dead entries are reclaimed before any live entry is evicted.
- Among live entries, the least recently written are evicted first.
- Task 20261005-091225-26 measures the latency of an eviction pass (a full-table scan and sort, as in pgss).

**Depends on:** 20261005-091225-14
**Open questions:** none
**Status:** done

### 20261005-091225-16: Execution frames and active-frame tracking

**Description:** Implement `src/context.c` (§3.1 item 3, §3.2 "Frame lifetime", §6.4).

Frame data:
- A frame holds the resolved tag set and statement metadata: `queryId`, `dbid`, `userid`, encoding, `toplevel`, and whether it is recordable.

Executor frames:
- Allocate them in `es_query_cxt`.
- Register them in a backend-local list, with a `MemoryContextCallback` that unlinks the frame when the context is destroyed. This covers abort paths and failed portals that skip `ExecutorEnd`.
- Look up a frame from its `QueryDesc`.

Utility frames:
- Snapshot the data before chaining, into storage that survives a `ROLLBACK` freeing transaction memory, for example on the stack or in a context released in `PG_FINALLY`.

Active frame and nesting:
- Provide helpers that save and restore the active-frame pointer and `nesting_level`, for use inside `PG_TRY`/`PG_FINALLY`.

Tag resolution follows `nested_tags`:
- `inherit` copies the active frame's tags.
- `scan` runs extraction on the frame's own source.
- `none` uses no tags.

A statement planned without an active frame gets only its own tags.

**Acceptance criteria:**
- In assert builds, the frame registry is empty at transaction end after these cases:
  - normal execution
  - errors
  - portals dropped without `ExecutorEnd`
  - suspended or interleaved portals
- All three `nested_tags` modes behave as described in §6.4.

**Depends on:** 20261005-091225-9
**Open questions:** none
**Status:** done

### 20261005-091225-17: Executor hooks and recording

**Description:** Install the `ExecutorStart`, `ExecutorRun`, `ExecutorFinish`, and `ExecutorEnd` hooks exactly as in §3.2 and §3.3.

`ExecutorStart`:
- Chain first.
- Return without a frame when the extension is disabled, `IsParallelWorker()` is true (§6.8), or `queryId == 0`.
- Otherwise create the frame and resolve its tags eagerly.
- Set up `queryDesc->totaltime` as pgss does.

`ExecutorRun` and `ExecutorFinish`:
- Activate the frame and increment `nesting_level` around the chained call, restoring both in `PG_FINALLY`. Use the compat signatures.

`ExecutorEnd`:
- Record when the frame is recordable under `track` (`top` or `all`), the `toplevel` rule, and the `untagged` policy (default `skip`).
- To record, take one call plus the elapsed time from `queryDesc->totaltime` (task 20261005-091225-12) and call `store_record()` with the current bucket. No other counters (rows, buffers, WAL, JIT) are collected; pgss covers them.
- Flush the backend-local extraction stats into the header counters.
- Then chain.

*Design note (from -5):* on PG18, `stmt_location` points at the first token, so leading comments fall before the range. Call `pssc_stmt_owned_start()` to extend the range. To keep strings with many statements O(n), cache the previous statement's end (per query string) and pass it as `from`. Starting from 0 with a gap longer than `max_bytes` silently loses PG18 leading comments.

**Acceptance criteria:**
- A commented simple-protocol `SELECT` is recorded with its tags, and its `queryid` equals the one in pgss.
- `track=top` and `track=all` behave as in pgss.
- Statements in a PL/pgSQL function inherit the caller's tags.
- A parallel query is counted once.
- A cursor fetched many times counts as one call.
- By default (`untagged=skip`) untagged statements are not recorded; with `untagged=record` they are recorded with an empty tag set.
- `enabled=off` records nothing.

**Depends on:** 20261005-091225-12, 20261005-091225-14, 20261005-091225-16
**Open questions:** none
**Status:** done

### 20261005-091225-18: `ProcessUtility` hook

**Description:** Implement utility handling (§3.2, §6.6, §6.7).

Before chaining, snapshot `queryId`, `stmt_location`/`stmt_len`, and tags into a utility frame.

Recording follows pgss, so rows join one-to-one:
- Record only when `track_utility` is on and `track` allows the nesting level.
- Never record `EXECUTE` or `PREPARE`.
- Exclude `DEALLOCATE` on PG14–16 and record it on PG17+.
- A recordable utility that arrives with `queryId == 0` increments `utility_missing_queryid` instead of being recorded.

Nesting:
- `EXECUTE` and `PREPARE` don't bump `nesting_level`.
- Every other utility bumps nesting and activates its frame, even when it isn't recorded, so `CALL`/`DO` children inherit tags.

Measurement:
- Measure elapsed time around the chained call, as pgss does for `total_exec_time`. No rows, buffer, or WAL counters are collected.
- Never read `pstmt` after chaining.
- Record from the snapshot, and restore state in `PG_FINALLY`.
- Never modify `pstmt->queryId`.

*Note (from -17):* until this hook exists, the inner statement of a plain `EXPLAIN` (no ANALYZE) is recorded as top level, while pgss counts it as nested under the `EXPLAIN` utility. This hook must add the utility nesting level so `toplevel` matches pgss; add a parity test.

**Acceptance criteria:**
- DDL is recorded with its tags.
- `EXECUTE` of a prepared statement records the plan as top-level and doesn't record the utility.
- `DEALLOCATE` follows the per-version rule.
- With `track_utility=off`, children of `CALL`/`DO` still inherit tags.
- `ROLLBACK` and `COMMIT` inside procedures don't crash and are Valgrind-clean.
- Utility `queryid` matches pgss on every version.

**Depends on:** 20261005-091225-17
**Open questions:** none
**Status:** done

### 20261005-181131-1: PG18 boundary cache: advance for skipped utilities (PREPARE/EXECUTE)

**Description:** Split from 20261005-091225-16 (round 2 review finding). The PG18 statement-boundary cache in `src/context.c` (used by `pssc_stmt_owned_start` to keep leading comments of later statements in a multi-statement simple-protocol string) only advances when a frame is initialized. `PREPARE` (and possibly `EXECUTE`, other skipped utilities) bypasses frame initialization, so the cache keeps the end of an earlier statement. If the skipped utility is longer than `scan_window`, the next statement's leading comment is outside the window and its tags are lost (with `untagged=skip`, the statement becomes unrecordable).

Repro (one simple-protocol query, PG18): `SELECT 1; PREPARE p AS SELECT length('<3000 chars>'); /*controller='second'*/ SELECT pssc_context_test_tags();` → empty tags.

Fix: advance the client statement-boundary state for every top-level statement of the client query string (including `PREPARE`/`EXECUTE` and other utilities that skip frames/recording), independently of frame activation or recording. Coordinate with the real `ProcessUtility` hook (20261005-091225-18) — if -18 lands first, do it there; otherwise expose a `pssc_context_note_stmt_boundary()` helper that -18 must call.

**Acceptance criteria:**
- A PG18 test with a skipped utility (`PREPARE`) longer than `scan_window` between statements keeps the following statement's leading-comment tags.
- Existing 010 tests still pass on PG14–18.

**Depends on:** 20261005-091225-16
**Open questions:** none
**Status:** done

### 20261005-091225-11: Debug extract function and scanner/extractor regression suite

**Description:** Add a debug SQL function such as `pg_stat_statement_context_extract(query text, stmt_location int DEFAULT -1, stmt_len int DEFAULT 0) RETURNS jsonb` (§9). It runs the full extraction pipeline with the current GUC config and reports whether the heuristic path was used.

Write `pg_regress` tests (`test/sql`, `test/expected`) for every item in the first bullet of §9:
- nested comments, dollar quotes, and `$` inside identifiers
- `E''` and `U&''` strings, and `standard_conforming_strings = off`
- unterminated comments and multibyte text
- multi-statement ranges and trailing footers
- each extractor format
- DSL errors and regex errors, including back-references
- allowlist, denylist, and `rename`
- `%00` and invalid encodings
- truncation on character boundaries

*Design note:* decide whether the debug function ships in the 1.0 script, and whether it is restricted (for example `REVOKE` from `PUBLIC`), then document the choice.

**Acceptance criteria:**
- `make installcheck` passes on PG14–18 in CI.
- Any version-specific expected output uses alternative expected files, with a comment explaining why.

**Depends on:** 20261005-091225-9, 20261005-091225-10
**Open questions:** none
**Status:** done

### 20261005-091225-19: `shared_preload_libraries` load-order detection and policy

**Description:** In `_PG_init`, parse `shared_preload_libraries` and detect when `pg_stat_statements` is loaded **after** this extension. In that order, pgss's hook runs outside ours and zeroes `pstmt->queryId` before our hook sees it (§3.2, §6.12). Log a `WARNING` that explains the required order. Take no other action: utility tracking stays enabled. The runtime counter `utility_missing_queryid` already exists (task 20261005-091225-18).

**Acceptance criteria:**
- The warning is emitted exactly when the order is wrong, and utility tracking is not disabled.
- The correct order, or no pgss at all, produces no warning.
- The TAP tests in task 20261005-091225-22 cover both orders.

**Decisions:**
- 2026-10-05 (§11 Q7): A load-order violation produces a `WARNING` only.

**Depends on:** 20261005-091225-18
**Open questions:** none
**Status:** done

### 20261005-091225-20: Stats SRF and views

**Description:** Implement the C set-returning function `pg_stat_statement_context(showtags, merge_buckets)` and its two views (§7). The SRF uses materialize mode.

Reading:
- Under the shared lock, copy the entries out. Each entry yields one row per live ring slot, hiding expired slots based on the clock (§5.2). Dead entries yield nothing.
- Compute `bucket_start = epoch + bucket_id × interval`.
- Output columns are exactly those in §7: `bucket_start`, `userid`, `dbid`, `queryid`, `toplevel`, `tags`, `calls`, `total_exec_time`.

Visibility (§6.11):
- For another role's rows, `queryid` and `tags` are `NULL` unless the caller has the privileges of `pg_read_all_stats`.
- The check runs inside the C function, so `showtags = false` can't bypass it.

Tag output:
- Convert tags from each entry's encoding with `pg_any_to_server`.
- For a `SQL_ASCII` origin, escape non-ASCII bytes instead of converting them.
- Output the tags as `jsonb`.

Merging:
- With `merge_buckets`, each entry yields one row: the task 20261005-091225-12 merge of its live slots (sums of `calls` and `total_exec_time`). `bucket_start` is the oldest live slot.

SQL script:
- Add the `pg_stat_statement_context` and `pg_stat_statement_context_totals` views and grants to the 1.0 script.

*Design note:* choose and document the escape format for `SQL_ASCII` output, for example `\xNN`.

**Acceptance criteria:**
- Both views return the expected rows and exactly the §7 columns.
- Merged sums match a hand computation.
- An unprivileged role sees `NULL` `queryid`/`tags` for other roles' rows, including with `showtags = false`.
- Tags from a non-UTF8 database are converted correctly, and `SQL_ASCII` bytes are escaped.
- Expired slots are hidden without any writes.

**Decisions:**
- 2026-10-05 (§11 Q2): `tags` is `jsonb`.
- 2026-10-05: Only `calls` and `total_exec_time` are exposed; other statistics come from pgss (see task 20261005-091225-12).

**Depends on:** 20261005-091225-12, 20261005-091225-14
**Open questions:** none
**Status:** done

### 20261005-091225-21: `_info()` and `_reset()` functions

**Description:** Implement `pg_stat_statement_context_info()` (§7), which returns:
- `entries`, `max_entries`, `dealloc`, `evicted_entries`
- `buckets`, `oldest_bucket`, the exact `shmem_bytes`
- `invalid_tags`, `dropped_tags` (tags dropped because the tag set would exceed `max_tagset_bytes`), `heuristic_scans`
- `regex_compile_failures` (regex lazy-compile failures; these happen per backend, so the backend-local count is flushed into a shared header counter)
- `utility_missing_queryid`, `stats_reset`

Implement `pg_stat_statement_context_reset()`, which clears all entries and counters and sets `stats_reset`. In the SQL script, run `REVOKE ALL ... FROM PUBLIC` on the reset function.

**Acceptance criteria:**
- Each counter moves under the activity that drives it:
  - `dealloc` and `evicted_entries` after churn
  - `invalid_tags` after malformed tags
  - `dropped_tags` after a tag set larger than `max_tagset_bytes`
  - `heuristic_scans` after `append` scans of long statements
  - `regex_compile_failures` after an injected lazy-compile failure (task 20261005-091225-10), visible from another session
  - `utility_missing_queryid` under the wrong load order
- Reset zeroes the counters and updates `stats_reset`.
- An unprivileged role gets "permission denied" when calling reset.

**Decisions:**
- 2026-10-05: Add `evicted_entries`, `dropped_tags` (tags dropped for exceeding `max_tagset_bytes`), and `regex_compile_failures` (needs a shared counter) to `_info()`.

**Depends on:** 20261005-091225-15, 20261005-091225-20
**Open questions:** none
**Status:** done

### 20261005-204419-1: Fix flaky `008_buckets.pl` ring-wrap check

**Description:** Found during 20261005-091225-21. `test/t/008_buckets.pl` check "the ring wrapped during phase B" (`current_bucket > $b0 + 64`, around line 384) failed once on PG15 and passed on rerun. It depends on random clock jumps driven by pgbench; the builder's simulation estimates a ~1% failure rate. Make the check deterministic (e.g. drive the clock with fixed steps, or loop until the wrap condition holds with a bounded number of iterations) without weakening what it verifies. Failing log was in `tmp/harness-15-flake.log` (scratch, may be gone).

**Acceptance criteria:**
- The check cannot fail by chance: either it's deterministic, or the probability argument is documented and below 1e-6.
- The test still fails if ring wrap-around handling is broken (show this by temporarily breaking it).

**Depends on:** 20261005-091225-14
**Open questions:** none
**Status:** done

### 20261005-091225-28: User documentation

**Description:** Write the README (and `docs/` if needed) covering:
- Purpose, plus build and install.
- Positioning as a pg_stat_statements companion: only `calls` and `total_exec_time` are stored per context, and everything else comes from pgss via the join on `(userid, dbid, queryid, toplevel)`, including the approximate apportioning recipe and its caveats (§5.1, §7).
- `shared_preload_libraries` ordering and the restart requirement (§3.2, §6.12).
- A reference for every GUC (§4.1), including the `untagged = skip` default.
- Changing configuration from SQL: `ALTER SYSTEM SET pg_stat_statement_context.extractors = '...'; SELECT pg_reload_conf();` (the `extractors`, `tags`, and `exclude_tags` GUCs are `sighup`; there is no config file).
- The extractor DSL with examples (§4.2), including the per-extractor `position` defaults and that `keys` matches original key names before `rename`.
- Allowlist, denylist, and cardinality guidance (§6.1).
- The capacity sizing rule: `max_entries` counts (query × context) combinations, independent of `bucket_count` (§5.1).
- Bucket semantics, "completions per interval" (§5.2).
- Eviction and the `_info()` counters (§5.3).
- The SQL interface and the example join (§7).
- Inclusive costs and filtering on `toplevel` (§6.4).

It must also list the limitations:
- stale comments on prepared statements, with the findings from task 20261005-091225-27 (§6.3)
- the `standard_conforming_strings` caveat and heuristic scans (§6.2)
- fragmented utility `queryid`s on PG14/15 (§6.6)
- `toplevel` divergence from pgss on PG14–16 (§6.7)
- failed statements are not counted (§6.9)
- visibility and PII (§6.11)
- managed providers (§6.12)

**Acceptance criteria:**
- Every GUC and SQL object is documented.
- The examples run as written against a test cluster.
- A review against DESIGN.md finds no gaps.

**Depends on:** 20261005-091225-10, 20261005-091225-19, 20261005-091225-21, 20261005-091225-27
**Open questions:** none
**Status:** done

### 20261005-091225-22: TAP tests: execution lifecycle and pgss parity

**Description:** Write TAP tests (`test/t/`) for the lifecycle and adversarial cases in §9:
- Prepared statements over the extended protocol (psql 16+ `\bind`, or `pgbench -M prepared`), asserting the stale-comment behavior documented in §6.3.
- Overlapping and suspended portals, and cursors that are never run or are closed early.
- SPI errors caught in PL/pgSQL followed by successful work, and failed portals.
- `ROLLBACK`, and `COMMIT` inside procedures.
- Both `shared_preload_libraries` orders.
- pgss and extension `track`/`track_utility` settings that differ.
- Nested `toplevel` parity with pgss on each version.
- Parallel queries.

**Acceptance criteria:**
- The tests pass on PG14–18 in CI, including the assert and Valgrind jobs.
- `calls`, `queryid`, and `toplevel` match pgss wherever the design says they should. With `untagged=record` and both extensions freshly reset, the per-`(userid, dbid, queryid, toplevel)` sums of `calls` equal pgss, and the sums of `total_exec_time` match pgss within a small tolerance.
- The documented divergence (pgss on PG14–16 with `track_utility=off`, §6.7) is asserted explicitly.

**Depends on:** 20261005-091225-18, 20261005-091225-20
**Open questions:** none
**Status:** done

### 20261005-091225-23: TAP tests: store, buckets, eviction, and reconfiguration

**Description:** Write TAP tests for the store-related items in §9:
- Restarts, including resizing through postmaster GUCs.
- `SIGHUP` reconfiguration: a bad DSL is rejected and the old config is kept, and a generation change recompiles regexes.
- Bucket rollover with a short `bucket_interval` set at startup: one entry per combination whose ring slots roll over lazily, with no re-insert at bucket boundaries.
- Stale-bucket insertion across a rollover.
- Clock steps, using the debug clock offset.
- A forced hash collision followed by eviction and reinsertion.
- Small-`max_entries` churn, with dead entries (all slots expired) reclaimed before live ones.
- Multi-client `pgbench` stress across bucket boundaries.

**Acceptance criteria:** The tests pass on PG14–18 in CI, including the assert and Valgrind jobs.

**Depends on:** 20261005-091225-17, 20261005-091225-21
**Open questions:** none
**Status:** done

### 20261005-091225-24: Tests: SQL interface, visibility, encodings, bucket merge

**Description:** Write regression or TAP tests for the SQL surface:
- Cross-database encodings, including `SQL_ASCII`. These need TAP, because `createdb` must use different encodings.
- Visibility for unprivileged roles with and without `pg_read_all_stats`, including `showtags = false`.
- `REVOKE` on the reset function.
- `merge_buckets`: the totals view sums `calls` and `total_exec_time` across live buckets, and `bucket_start` is the oldest live bucket.
- View column names and types match §7 exactly (only `calls` and `total_exec_time` as counters), and the `_info()` columns match §7.
- The example join query from §7, including the apportioning example, runs against pgss.

**Acceptance criteria:** The tests pass on PG14–18 in CI.

**Depends on:** 20261005-091225-17, 20261005-091225-21
**Open questions:** none
**Status:** done

### 20261005-101154-1: Harden exact-release source-build harness

**Description:** These problems were split off from 20261005-091225-2 after its second review round. They are in the `scripts/docker-test.sh <major>.<minor>` path, which builds an exact PostgreSQL release from source using `docker/Dockerfile.source`.
- **Missing build tools:** the source image doesn't install Bison or Flex, which PG17+ needs even when building from release tarballs. `scripts/docker-test.sh 17.0` fails with `configure: error: bison not found`.
- **Download not verified:** the `curl … | tar` pipeline runs under `/bin/sh` without `pipefail`, so a failed or truncated download can still let the build continue. The tarball also isn't checked against a SHA-256 checksum. Fix: download to a file, verify the checksum against the official `.sha256` for that release, and only then extract.

**Acceptance criteria:**
- `scripts/docker-test.sh 17.0` and `scripts/docker-test.sh 18.0` build and pass. `scripts/docker-test.sh 15.0` still passes.
- A failed download (e.g. a bad URL or a simulated mid-stream failure) or a checksum mismatch fails the build.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-103941-1: Trim unused counter-availability shims from `compat.h`

**Description:** Task 20261005-091225-2 landed counter-availability shims in `src/compat.h` for the larger counter set that was dropped on 2026-10-05 (the extension now stores only `calls` and `total_exec_time`, §5.1, §6.10). Once tasks 20261005-091225-12 and 20261005-091225-17 have settled which shims are actually used, remove or trim the unused ones:
- `PSSC_QUERYDESC_ROWS` (`es_processed` vs `es_total_processed`)
- `PSSC_HAS_TEMP_BLK_IO_TIME`, `PSSC_HAS_LOCAL_BLK_IO_TIME`, `PSSC_SHARED_BLK_READ_TIME`/`PSSC_SHARED_BLK_WRITE_TIME`
- `PSSC_HAS_WAL_BUFFERS_FULL`, `PSSC_HAS_JIT_DEFORM_COUNTER`

Remove the matching cases from the compat test module (`test/modules/pssc_compat_test`) and `test/t/002_compat.pl`. Keep every shim that some source file uses.

**Acceptance criteria:**
- No shim remains in `compat.h` that nothing outside the compat test module uses, except where a comment justifies keeping it.
- The compat test module and TAP test no longer reference removed shims, and the full suite passes on PG14–18.
- The header still compiles warning-free with `-Wall` on PG14–18.

**Depends on:** 20261005-091225-17
**Open questions:** none
**Status:** done

### 20261005-233059-1: Fix 017 cached-utility expectations on PG 15.0

**Description:** Found while working on 20261005-101154-1. `scripts/docker-test.sh 15.0` fails 4 subtests in `test/t/017_lifecycle.pl`: 104–105, 231 and 256. These are the cached PL/pgSQL `CREATE TEMP TABLE` checks. Both pg_stat_statements and this extension report 4 calls where the test expects 2, so the two still agree; the test's expectation is what's wrong. It fails the same way on clean HEAD, so the harness change didn't cause it. It's probably a behaviour difference between 15.0 and the latest 15.x in pgss utility tracking or in plancache re-execution. Find which 15.x release changed it, then make the expectation depend on the version (through compat.h or by using pgss as the oracle). Don't hard-code a minor version in the test without a reason. Logs are in `tmp/harness/`.

**Acceptance criteria:**
- `scripts/docker-test.sh 15.0` passes, and the latest 14–18 releases still pass.
- The test still asserts that our counts match pgss for these statements.

**Depends on:** 20261005-091225-22
**Open questions:** none
**Status:** done

### 20261005-091225-3: CI matrix (PG14–18 × Linux/macOS, assert, Valgrind)

**Description:** Add a GitHub Actions workflow (§9 CI matrix):
- Build and run `make installcheck` plus the TAP tests for PG14–18 on Linux (PGDG packages) and macOS (Homebrew or source build).
- Add a Linux job that builds PostgreSQL with `--enable-cassert`/`-DUSE_ASSERT_CHECKING` and `--enable-tap-tests`.
- Add a Linux job that runs the regression suite under Valgrind, using PostgreSQL's `valgrind.supp`.
- Cache source builds.
- Make the TAP harness work on both module namespaces: `PostgresNode`/`TestLib` on PG14 and `PostgreSQL::Test::*` on PG15+. This can be a small compatibility wrapper or a documented PG14 skip policy.

**Acceptance criteria:**
- The workflow runs on pushes and pull requests.
- Every cell is green on the skeleton.
- A deliberately failing test fails its job.
- The Valgrind job reports no errors.

**Depends on:** 20261005-091225-1
**Open questions:** none
**Status:** done

### 20261005-235708-1: `extract` regression output differs on old PG14 minors (psql emoji padding)

**Description:** Found while working on 20261005-233059-1. On `scripts/docker-test.sh 14.9`, the pg_regress `extract` test fails: older psql pads the row containing the 😀 tag differently, so the aligned output doesn't match `expected/extract.out`. The extension's output is fine; only psql's width calculation differs. Fix it without weakening the test. Options: use unaligned output (`\pset format unaligned`) for the affected queries, return the value through `encode(convert_to(...), 'hex')` or as a length, or add an alternative expected file (`extract_1.out`) if the difference is truly only psql. Also check whether other old minors (14.3+, 15.0–15.x, 16.0) are affected.

**Acceptance criteria:**
- `scripts/docker-test.sh 14.9` passes the `extract` test, and so do the latest 14–18 releases.
- The test still checks the exact emoji bytes or characters.

**Depends on:** 20261005-091225-11
**Open questions:** none
**Status:** done

### 20261005-091225-42: Roadmap: exporter recipes and Grafana dashboard

**Description:** Publish ready-to-use integration recipes (§8 v3):
- `postgres_exporter` custom queries
- an OpenTelemetry Collector `postgresql` receiver configuration
- a Grafana dashboard JSON that uses the `toplevel` filter correctly

**Acceptance criteria:**
- Each recipe is tested against a running cluster, and the dashboard renders sample data.

**Depends on:** 20261005-091225-28
**Open questions:** none
**Status:** done

### 20261005-091225-25: Fuzzing harnesses

**Description:** Add fuzzing under `fuzz/` (§9):
- A standalone libFuzzer harness, built with `clang -fsanitize=fuzzer,address,undefined`, for the comment scanner (all positional modes) and the SQLCommenter/marginalia parsers. Seed it from the regression vectors.
- A backend-aware, SQL-level fuzz driver for the regex extractor that drives the debug function from task 20261005-091225-11 with generated comments and configured patterns.
- An optional short smoke run in CI.

**Acceptance criteria:**
- Each harness builds with one documented command.
- A 10-minute local run finds no crashes or sanitizer reports.
- The CI smoke run takes about 60 s.

**Depends on:** 20261005-091225-5, 20261005-091225-6, 20261005-091225-11
**Open questions:** none
**Status:** done

### 20261005-091225-41: Roadmap: tag value normalization rules

**Description:** Add per-key regex-replace rules for tag values, for example `/users/\d+` → `/users/:id` on the `route` key (§8 v2). They run after `rename` and the allowlist/denylist, and before truncation and cardinality caps (§6.11 step 6). They reuse the regex infrastructure from task 20261005-091225-10 and its safety limits (§6.11).

*Design note* (non-blocking): GUC name and rule syntax, for example `normalize = 'route:/users/\d+=>/users/:id|...'`, and whether multiple rules per key apply in order.

**Acceptance criteria:**
- Configured rules rewrite values of their key only, before the key is built.
- Normalization sees renamed keys and only allowed tags, and its output is then truncated (and, once task 20261005-091225-32 lands, capped).
- Invalid rules are rejected at `SET`/reload time.
- CPU limits match those of the regex extractor.

**Decisions:**
- 2026-10-05: Rules are per-key regex-replace rules.
- 2026-10-05: Normalization runs after rename and allowlist/denylist, before truncation and cardinality caps.

**Depends on:** 20261005-091225-9, 20261005-091225-10
**Open questions:** none
**Status:** done

### 20261006-021621-1: SQL fuzzer: classify only the check hook's own error

**Description:** Left over from item -25 after its second review round. In `fuzz/sql/regex_fuzz.pl` (around lines 959–972), `$err` holds stderr from the whole round, so a statement timeout from the earlier `regexp_matches()` pre-check gets read as the check hook's rejection reason. That has two effects:
- Once the pre-check times out, an unrelated check-hook error (e.g. `division by zero`) is accepted as an expected timeout, hiding a real bug.
- A correct length-limit rejection after a pre-check timeout counts as a false failure (`want='long'` but `reason='timeout'`).

Fix: capture the check hook's own error message and SQLSTATE right after `ALTER SYSTEM` and classify only that. Keep the pre-check timeout only as the condition for accepting a timeout from the check hook itself.

**Acceptance criteria:**
- New `--self-test` cases cover both scenarios above and give the right verdict.
- A short SQL fuzz run against the assert build still passes.

**Depends on:** 20261005-091225-25
**Open questions:** none
**Status:** done

### 20261005-091225-26: Overhead and latency benchmarks

**Description:** Add reproducible `pgbench` scripts and a results write-up (§9 Benchmarks). Run `pgbench -S` in these configurations:
- with no extension
- with pgss only
- with pgss plus this extension
- with and without comments
- with large `IN` lists in `append`/`any` modes, including the `stmt_len = 0` `strlen` case

Also measure bursts at bucket boundaries (short interval) and sustained eviction (small `max_entries` with high-cardinality tags). Report TPS, average latency, p99 latency, and maximum latency, relative to pgss alone.

**Acceptance criteria:**
- The scripts run from one command.
- The results table is published in the repository docs.
- p99 and maximum latency at bucket boundaries and under eviction are reported explicitly.

**Depends on:** 20261005-091225-15, 20261005-091225-18, 20261005-091225-20
**Open questions:** none
**Status:** done

### 20261005-091225-38: Roadmap: context from `application_name`

**Description:** Add a DSL extractor `appname(format=sqlcommenter|marginalia|regex)` that derives tags from `application_name` instead of comment text (§4.2, §8 v2). It parses with the named format's rules and parameters (`kv_sep`/`pair_sep`, `url_decode`, or `pattern`/`keys` for `regex`), and accepts the common `keys`/`rename` parameters. Its output goes through the §6.11 pipeline. Tags from comments win over `appname`-derived tags on key conflicts.

**Acceptance criteria:**
- Each `format` produces the expected tags from a matching `application_name`.
- On a key conflict, the comment's value is stored.
- Malformed values are dropped and counted.
- Invalid `appname(...)` parameters are rejected by the DSL `check_hook`.

**Decisions:**
- 2026-10-05: Parsed via a DSL extractor `appname(format=sqlcommenter|marginalia|regex)`.
- 2026-10-05: Comment tags win over `appname`-derived tags.

**Depends on:** 20261005-091225-9, 20261005-091225-17
**Open questions:** none
**Status:** done

### 20261006-043919-1: Reduce eviction-pass lock hold time (sustained churn triples p99)

**Description:** Found by the benchmarks (item -26, docs/benchmarks.md). Under sustained eviction with high-cardinality tags at `max_entries=10000`, p99 latency rose from 0.477 ms (pgss alone) to 1.200 ms on PG 18.6 (+158%), and 5× on PG 14. TPS fell 12.5%. That was about 90 passes per second, each removing 500 entries. `store_evict()` in `src/store.c` (§5.3) holds the store's exclusive lock while it scans the whole table, copies every live entry, and sorts them all with `pssc_evict_sort()`. Every backend recording during a pass waits.

Options, in order of preference:
1. Choose the victims by partial selection instead of a full sort (quickselect or a bounded heap of size `nvictims`, O(n)). Eviction order stays the same: last_bucket, then usage.
2. Reuse the candidate buffer instead of allocating it each pass.
3. Make the batch bigger when passes come close together, so fewer passes run. This changes semantics: document it, and keep the §5.3 order.
4. Do the scan and sort under the shared lock with a generation check, and take the exclusive lock only to remove the victims. This is more complex; justify it with measurements first.

Measure each step with `bench/run.sh --only evict` and keep the semantics in §5.3 (victim order and `evicted_entries` accounting) intact; the existing TAP tests 008/018 must still pass.

**Acceptance criteria:**
- The eviction benchmark's p99 at `max_entries=10000` is no more than about 1.5× pgss alone on PG 18 (or the remaining gap is explained), with numbers updated in docs/benchmarks.md.
- Victim selection matches the old full sort exactly; a unit test compares them on random inputs, including ties.
- Full harness passes on PG 14–18.

**Depends on:** 20261005-091225-26
**Open questions:** none
**Status:** done

### 20261006-021334-1: Bound regex compile cost (pathological patterns stall first tagged query)

**Description:** Found by the SQL fuzzer (item -25). The pattern `((?:(?:$)|\Zda|(?<!1)|\S){0,255}` takes over 20 s to compile, both in core `regexp_matches` and in our check hook. Only statement_timeout limited it, by cancelling the `ALTER SYSTEM`. If a superuser sets such a pattern with no statement_timeout, the check hook accepts it. Then every backend compiles it lazily on its first tagged statement (§4.2 regex), stalling a user query for seconds. A cancel or timeout during that compile is re-thrown into the user's query, which is worse than a stall.

The execution-time CPU limits (item -10) don't cover compile. Options (decide and document):
- Bound compile with a complexity heuristic in the check hook, e.g. reject `{m,n}` with large n around a group that can match empty, or cap pattern length and number of groups.
- Measure compile time in the check hook and reject patterns over a threshold (e.g. 100 ms). The check hook runs in the postmaster at reload, which is acceptable because the hook already compiles there.
- Use the engine's cancel mechanism (`rcancelrequested` callback) to abort a compile after N ms in backends, and treat it as a compile failure (counted in `regex_compile_failures`, extractor disabled for the backend) instead of re-throwing into the user's query.

**Acceptance criteria:**
- The fuzzer's pathological pattern is rejected at SET/reload, or, if it is accepted, a backend never spends more than the documented bound compiling it on the hot path and never fails the user's query because of it.
- Normal patterns from docs/extractors.md are unaffected.
- A test covers the case (TAP or pg_regress).

**Depends on:** 20261005-091225-10
**Open questions:** none
**Status:** done

### 20261006-080948-1: Regex compile retry: discard allocations of interrupted attempts

**Description:** Found in the round-2 review of 20261006-021334-1. On PG16–18 a compile stopped by the 100 ms limit throws out of `pg_regcomp()` before its cleanup runs. The retry (an attempt that used < half the limit in CPU time is retried, up to 3 attempts) reuses the same slot memory context, so the interrupted attempt's allocations stay there, and the next compile overwrites the regex's ownership pointers. If a retry succeeds, the slot keeps the abandoned allocations until its configuration generation is released. Reviewer reproduced on PG18 with the real `{0,13}` pathological pattern and a test-only 1000 ms limit: ~4 MB abandoned (5,956,832 vs 1,796,472 bytes used). Existing stall injections miss it because they interrupt before the engine allocates. See `src/regex_runtime.c` compile_slot / run_compile.

**Acceptance criteria:**
- Each attempt compiles in its own disposable context; an interrupted attempt's context is deleted before retrying; only the successful attempt's context is kept.
- A failing-first test (e.g. a test-module injection that interrupts after the engine has allocated, then checks the slot context's size or that a retry's context equals a clean compile's) turns green.
- Full harness passes on PG 14–18.

**Depends on:** 20261006-021334-1
**Open questions:** none
**Status:** done

### 20261005-091225-39: Roadmap: `pg_stat_statement_context_activity` view

**Description:** Add a view that shows the **current** tags of each backend, as a companion to `pg_stat_activity` (§8 v2). Keep per-backend shared slots, sized `MaxBackends × max_tagset_bytes`, and update them when top-level frames are activated. The visibility rules from §6.11 apply.

**Acceptance criteria:**
- The view shows the running statement's tags joinable on `pid`.
- Tags are `NULL` for other roles without `pg_read_all_stats`.
- Hot-path overhead is benchmarked.

**Depends on:** 20261005-091225-18, 20261005-091225-20
**Open questions:** none
**Status:** done

### 20261006-093831-1: Flaky TAP 007: boundary-value bucket tests depend on the wall clock

**Description:** `test/t/007_store.pl` lines ~162–175 restart with `bucket_count=10000` (default `bucket_interval=300`), record once via the clock (`rec(q{1, ...})`, no explicit bucket), then expect `record_at(..., 0)` to land in slot/bucket 0. If the clock bucket has advanced to 1 by then (a 300 s boundary crossed since the store's epoch, or a slow host), writes clamp to the current watermark and land in bucket 1. Seen once on PG18 during -39's harness: got `2::1:1:1:1 2::9999:9999:1:1`, expected `2::0:0:1:1 2::9999:9999:1:1`; passed on rerun.

**Acceptance criteria:** the test pins the clock (the store has a debug clock offset, `PSSC_DEBUG_CLOCK_MAX_OFFSET`) or otherwise avoids depending on the wall clock; 007 passes reliably; audit other tests in 007/008/018 that use `record_at` after a clock-based record for the same pattern.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-30: Roadmap: `tags_override` session/transaction context

**Description:** Add a `USERSET` GUC, `pg_stat_statement_context.tags_override`, that can be set with `SET` or `SET LOCAL` (§8 v2, §6.3). It works with prepared statements and with drivers that can't add comments.
- **Syntax:** sqlcommenter style, `k='v',k2='v2'`, with URL-encoded values (for example `SET LOCAL pg_stat_statement_context.tags_override = 'controller=''users'',action=''show'''`). Parse it in a `check_hook` with the sqlcommenter parser (task 20261005-091225-6) into a flat `extra` blob; malformed values are rejected.
- **Combination:** override tags **merge** with tags from comments; on a key conflict the override value wins.
- **Pipeline:** override tags go through the same §6.11 pipeline as comment tags (decode and validation, `rename`, allowlist/denylist, truncation), except the per-extractor `keys` step.
- Apply it when top-level frames are created, so nested frames inherit it through the normal rules.

If task 20261005-091225-27 decides on go, this task moves into v1.

**Acceptance criteria:**
- With `SET LOCAL`, statements in the transaction get the override tags, and they no longer apply after commit.
- Prepared statements executed after the `SET` pick up the override.
- An override and a comment with disjoint keys produce the union; on a shared key the override value is stored.
- Override keys are renamed, filtered by the allowlist/denylist, and truncated exactly like comment tags.
- Invalid values (bad syntax, bad `%` escapes) are rejected at `SET` time.
- The feature is documented.

**Decisions:**
- 2026-10-05: Value syntax is sqlcommenter-style `k='v',k2='v2'` with URL-encoded values.
- 2026-10-05: Override tags merge with comment tags; the `SET` value wins on key conflicts.
- 2026-10-05: Override tags go through the same §6.11 pipeline (rename, allowlist/denylist, truncation).
- 2026-10-05: Item -27 decided **no-go** for v1 (no driver reuses prepared statements across comments; see DESIGN.md §6.3). This stays a roadmap item.

**Depends on:** 20261005-091225-18, 20261005-091225-27
**Open questions:** none
**Status:** done

### 20261006-101139-1: Confirm `tags_override` rename rule and nested `scan` behavior

**Description:** Item -30 landed `tags_override` with two builder decisions that need the user's confirmation (DESIGN.md §6.11 and §8). If either is rejected, change the code, tests (test/t/023_tags_override.pl, test/sql/tags_override.sql) and docs (docs/configuration.md `tags_override`, docs/extractors.md pipeline) accordingly.

**Acceptance criteria:** the user's answers are recorded as decisions; code, tests and docs match them.

**Decisions:**
- 2026-10-06: Q1 confirmed: override keys use the comment `sqlcommenter` extractors' `rename` lists.
- 2026-10-06: Q2 confirmed: with `nested_tags = scan`, a function's `SET tags_override` applies to its nested statements.

**Depends on:** 20261005-091225-30
**Open questions:** none (answered 2026-10-06)
**Status:** done

### 20261006-092320-1: Flaky TAP 004: "every alternating reload replaced the extractors" (28 of 30)

**Description:** `test/t/004_extractors.pl` (around line 530) alternates `extractors` between two big values for 30 reloads and expects the session's config generation (`pssc_guc_test_generation()`) to grow by exactly 30. It has failed intermittently with 28 on PG18, twice: once during 20261006-021334-1's harness and once during -39's harness (both before and after the regex CPU-time retry was added). `alter_and_reload()` waits for each session to see a sentinel `scan_window` value, so reloads can't simply coalesce. Likely cause: in that backend the check hook rejected one of the values while re-reading the config file (e.g. a regex compile hitting the 100 ms limit on a busy host, or another transient failure), so the backend kept the old value silently (logged only at DEBUG3). If so, backends can disagree on the config, which is a real (if rare) product issue, not just a test issue.

**Acceptance criteria:**
- Root cause identified (e.g. by logging the rejection reason in the test, or raising the backend log level for the session) and documented.
- If it's the compile limit: decide whether a value already accepted by the postmaster should be rejected in a backend because of time alone (e.g. skip the time limit in the SIGHUP re-check, or apply it only in SET/ALTER SYSTEM), fix, and add a test.
- 004 passes 20 consecutive runs on PG18 under load (e.g. run alongside another harness).

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261005-091225-32: Roadmap: per-key cardinality caps (overflow → JSON `null`)

**Description:** Cap the number of distinct values per allowed key (§6.1, §8 v1.x). Values beyond the cap collapse to JSON `null` before the key is built (§6.11 step 8, after truncation). A client can only send strings, so `null` can't collide with a real value. Count collapses in `_info()`.
- **Configuration:** a global default cap GUC plus optional per-key overrides.
- **Scope:** distinct values are counted globally per key (not per bucket or per `queryid`).

*Design notes* (propose and document; non-blocking):
- GUC names and the per-key override syntax, for example `cardinality_cap = 100` and `cardinality_cap_overrides = 'route:500|job:50'`.
- The shared structure that tracks distinct values per key (for example a fixed-size shared hash of `(key, value hash)`), its memory budget (postmaster-sized), and what happens when that structure itself is full.
- Whether the distinct-value sets are cleared by `_reset()` and/or decay over time.
- The canonical serialization of a `null` value in the key (it must differ from every string, e.g. a flag byte), and its `jsonb` output.

**Acceptance criteria:**
- Flooding an allowed key with random values produces at most *cap* distinct string values plus `null` for that key.
- Per-key overrides take precedence over the global default.
- `null` values appear as JSON `null` in `tags`, and are never produced by client input.
- Collapses are visible in `_info()`, and the upgrade script is provided.
- The hot path stays lock-free until the store write.

**Decisions:**
- 2026-10-05: Overflow values are represented as JSON `null`, replacing the earlier `<other>` literal.
- 2026-10-05 (adopted proposal, non-blocking): a global default cap GUC plus optional per-key overrides; distinct values counted globally per key. Remaining details are design notes.

**Depends on:** 20261005-091225-17, 20261005-091225-21
**Open questions:** none
**Status:** done

### 20261006-113156-1: Make the 006 compile-limit tests tolerate VM steal time

Found while fixing 20261006-092320-1. At load average ~100 (CPU hogs both in the Docker VM and on the host), two older `test/t/006_regex.pl` cases still fail, because time the host takes the virtual CPU away is counted as backend CPU time:
- the "large config" `ALTER SYSTEM` is rejected for compile time;
- in "compile over the time limit (sleep)", the extractor is not disabled.

The same accounting can make `SET`/`ALTER SYSTEM` reject a normal pattern on an overloaded host (documented in `docs/extractors.md`). Options: make these tests use the test-only limit hooks so they don't depend on real CPU time, and/or find a steal-resistant way to tell a busy compile from a stalled one.

**Acceptance criteria:**
- Both cases pass 10 of 10 runs under the load recipe in `tmp/flaky004/` (or an equivalent documented one).
- Any product change is test-first and documented in DESIGN §4.2.

**Depends on:** 20261006-092320-1
**Open questions:** none
**Status:** done

### 20261006-010149-1: Exporter-friendly SQL surface: monotonic counters and bucket metadata

**Description:** Found while writing the exporter recipes (item -42). None of the views has a counter that only grows: `_totals` is a sliding window and bucket rows expire. So Prometheus `rate()` can't be used, and the recipes export gauges over the last closed bucket instead. They also hard-code the bucket length GUC and the hidden 2000-01-01 starting point for buckets. Proposed additions:
1. Bucket metadata in `_info()`: `bucket_seconds`, `current_bucket_start`, `last_closed_bucket_start`.
2. Optionally a `pg_stat_statement_context_last_bucket` view, which gives the last closed bucket per entry.
3. Optionally counters per entry that only grow (`calls_total`, `exec_time_total` plus `stats_since`) and survive bucket expiry until the entry is evicted. This costs extra shared-memory bytes per entry.
4. An epoch-number form of `stats_reset`, or let the recipes keep converting it.

Once this lands, simplify the recipes in `docs/integrations/` and update `scripts/test-integrations.sh`.

**Acceptance criteria:**
- The chosen columns or views exist, are documented in §7 and `docs/sql-interface.md`, and are tested in TAP or 019.
- The recipes no longer depend on the epoch or bucket-length GUC.

**Decisions:**
- 2026-10-06: Build additions 1 (bucket metadata in `_info()`: `bucket_seconds`, `current_bucket_start`, `last_closed_bucket_start`), 2 (`pg_stat_statement_context_last_bucket` view) and 4 (epoch-number form of `stats_reset`). Addition 3 was initially deferred; see the next decision.
- 2026-10-06: Do it together with 20261005-213120-1 in one builder run, before `--1.0.sql` is frozen.
- 2026-10-06 (later): The owner wants addition 3 in v1 as well: pgss-style monotonic per-entry counters (`calls_total`, `exec_time_total`, `stats_since`) that only reset on eviction or `_reset()`, so exporters can use `rate()`. Expect about 16–24 bytes more shared memory per entry; update the §5 sizing numbers.

**Depends on:** 20261005-091225-42
**Open questions:** none
**Status:** done

### 20261005-213120-1: `_info()`: distinguish live eviction from expired-entry reclamation

**Description:** Found while documenting (item -28). `evicted_entries` counts both expired entries reclaimed by an eviction pass and live entries evicted, and `dealloc` counts passes. `dropped_records` (calls lost because a pass freed nothing) is not exposed. So `_info()` alone cannot tell an operator that `max_entries` is too small, contrary to DESIGN §5.3 step 3. The docs currently give a workaround: compare the row count of `pg_stat_statement_context_totals` with `max_entries`.

Proposed: split the counter into `reclaimed_entries` (expired or dead, harmless) and `evicted_entries` (live, history lost), and expose `dropped_records`. Update §5.3, §7, the docs and tests.

**Acceptance criteria:**
- Expired-only reclamation moves `reclaimed_entries`, not `evicted_entries`.
- Undersized churn moves `evicted_entries`.
- A full table with nothing to free moves `dropped_records`.
- The docs' undersizing guidance uses the new counters.

**Decisions:**
- 2026-10-06: Approved: split into `reclaimed_entries` (expired/dead), `evicted_entries` (live only) and add `dropped_records`. Do it in the same builder run as 20261006-010149-1 (one `_info()` change), before `--1.0.sql` is frozen (no upgrade script).

**Depends on:** 20261005-091225-21
**Open questions:** none
**Status:** done

### 20261006-143225-1: Close the deadline-postponement race in the test module's sleep injection

Split from 20261006-113156-1 (final review finding, not fixed within 2 rounds). In `test/modules/pssc_extract_test/pssc_extract_test.c` (around lines 342–355), the `sleep`/`regsleep` injections snapshot whether the compile deadline is pending, then postpone it with `pssc_regex_test_expire_in(60000)`. If the original 100 ms deadline fires between the snapshot and the postponement (the backend is descheduled there), the postponement doesn't clear the already-pending self-cancel. The loop then treats it as a genuine cancel and returns after almost no CPU time, so the attempt is classified as a stall and retried without the injection. That is the original load-dependent 006 failure, now in a much narrower window.

Fix options: block SIGALRM around the snapshot and postponement, or expose deadline ownership (the runtime's own "our cancel vs foreign cancel" state) through a PGDLLEXPORT test helper so the loop can tell a raced self-cancel from a genuine one.

**Acceptance criteria:**
- A deterministic test (for example a test hook that fires the deadline between the snapshot and the postponement) fails before the fix and passes after.
- The 006 cancel/terminate/statement_timeout tests from 20261006-113156-1 still pass; the full harness passes on PG14–18.

**Depends on:** 20261006-113156-1
**Open questions:** none
**Status:** done

### 20261006-075124-1: Fewer eviction passes under sustained churn (adaptive batch or compact scan)

**Description:** Follow-up to 20261006-043919-1. Partial selection made a pass ~40% faster, but under the `evict` benchmark at `max_entries=10000` p99 is still ~2.15× pgss alone (target ~1.5×). The remaining cost is the single scan of ~10,000 entries (~870 B each, ~8.7 MB) under the exclusive lock. Options:
1. Adaptive batch: evict a larger fraction (e.g. up to 20%) when passes come close together. A throwaway build gave Δp99 +34% (TPS −7.6%) on a noisy run. Changes §5.3 semantics (target is no longer a fixed ~5%), loses more history of rare combinations.
2. A compact per-entry array of (`last_bucket`, `usage`, entry pointer) maintained in shared memory, so the pass scans ~16–24 B per entry instead of whole entries. Keeps §5.3 semantics; more code and shared memory.

**Acceptance criteria:** eviction-benchmark p99 at `max_entries=10000` ≤ ~1.5× pgss alone on PG 18 (or gap explained); §5.3 updated if semantics change; TAP 008/009/018 pass; numbers in docs/benchmarks.md.

**Decisions:**
- 2026-10-06: Do option 2 (compact per-entry array, keeps §5.3 semantics) first; only if p99 is still over ~1.5× pgss, add option 1 (adaptive batch) and update §5.3.

**Depends on:** 20261006-043919-1
**Open questions:** none
**Status:** done

### 20261005-091225-34: Roadmap: background worker reclaiming dead entries

**Description:** Add an optional background worker that, on idle systems, advances `current_bucket` and reclaims dead entries (every ring slot expired) so their space is free before the next insert needs it (§8 v1.x, §5.2, §5.3). It isn't needed for correctness, because readers already filter expired slots and eviction reclaims dead entries first. Enable it with a postmaster GUC.

**Acceptance criteria:**
- With the worker enabled, dead entries are freed without any query traffic, and `_info().entries` drops accordingly.
- Disabling it changes nothing else.

**Depends on:** 20261005-091225-15
**Open questions:** none
**Decisions** (2026-10-06, made while building; recorded in DESIGN.md §4.1, §5.3):
- GUCs `reclaim_worker` (bool, postmaster, default `off`: no worker is registered at all) and `reclaim_worker_interval` (ms, sighup, default 10 s, 100 ms – 1 day; a reload wakes the worker).
- The worker has shared-memory access only (no database connection), so it is not in `pg_stat_activity`; it is identified by its process title.
- Each wake-up raises `current_bucket` like a reader and, only if it moved since the previous pass, runs the dead-entry scan of an eviction pass (shared helper) under the exclusive lock.
- It never decays usage or evicts live entries, counts what it removes in `reclaimed_entries`, and does not increment `dealloc` (passes forced by a full table) or `evicted_entries`.
**Status:** done

### 20261005-091225-35: Roadmap: persist stats across clean restarts

**Description:** Dump the stats at shutdown and load them at startup, like `pg_stat_statements.save` (§8 v1.x). Use a versioned file format with a header that records the extension version, epoch, `bucket_interval`, `bucket_count`, and sizing. Follow pgss's lead on mismatches:
- On a file-format or extension-version mismatch, discard the file.
- If `bucket_interval` or `bucket_count` changed, discard the file (pgss has no bucket analogue).
- If `max_entries` shrank, load what fits and evict the rest using the §5.3 order.
- Otherwise keep the stored epoch, so `bucket_id`s stay valid, and drop slots that expired during the downtime.

*Design note* (non-blocking): `max_tagset_bytes` changes are not covered by the decision. Proposed: load entries whose tag set still fits the new limit and skip (and log a count of) the rest.

**Acceptance criteria:**
- Stats survive a clean restart.
- Each mismatch case above behaves as specified, with a log message.
- A crash or a corrupt file starts empty with a log message.
- The behavior is controlled by a GUC.

**Decisions:**
- 2026-10-05: Follow pg_stat_statements: discard on format/version mismatch; if `max_entries` shrank, load what fits and evict the rest; if `bucket_interval` or `bucket_count` changed, discard.
- 2026-10-07 (implementation, DESIGN.md §5.5):
  - **GUC:** `pg_stat_statement_context.save` is on by default and `sighup`, matching `pg_stat_statements.save` for familiarity.
  - **File:** `pg_stat/pg_stat_statement_context.stat`. It is written as `.tmp` and then renamed with `durable_rename()`.
  - **Who saves:** only the postmaster saves, from an `on_shmem_exit` callback registered when `!IsUnderPostmaster`, as in pgss. Backends do not save.
  - **When it saves:** only if the exit code is 0 and `pg_control` says `DB_SHUTDOWNED` or `DB_SHUTDOWNED_IN_RECOVERY`. All children have then exited and the shutdown checkpoint is done, so the store is immutable and is read without locks.
    - This is stricter than pgss, which also saves after an immediate shutdown with a possibly torn store.
    - `pg_control` is read directly rather than with `get_controlfile()`, because an ERROR inside `proc_exit()` would be promoted to FATAL.
  - **Header:** a magic number, a binary format version separate from the SQL version, `PG_MAJORVERSION_NUM`, the extension version (injected from the control file by the Makefile), epoch, interval, `bucket_count`, `max_entries`, `max_tagset_bytes`, the watermark, and every header counter plus `stats_reset`.
  - **Body and checksum:** the records follow in eviction-array order, each carrying the stored `tags_hash`, usage, totals, `stats_since`, `encoding`, and the whole ring. A CRC-32C trailer covers the file, and nothing may follow it.
  - **Load:** happens when the postmaster creates the store, and the file is unlinked whatever the outcome.
    - A missing file loads silently.
    - Every other failure LOGs and starts empty. This covers save off, bad magic, CRC, truncation, a malformed record, a duplicate key, or trailing bytes; a version mismatch; and a bucket-setting change.
  - **Epoch and watermark:** the epoch is kept. `current_bucket` becomes max(the saved value, the clock's bucket).
  - **Expiry:** entries that are dead at that watermark are dropped and counted in `reclaimed_entries`. Expired slots are cleared.
  - **`max_tagset_bytes` design note** (implemented as proposed): entries whose tag set exceeds the current `max_tagset_bytes` are skipped, with one LOG line giving the count.
  - **`max_entries` shrink:** the excess live entries are chosen with `pssc_evict_sort()` in §5.3 order (last_bucket, usage, saved array order; no decay). This counts as one eviction pass (`dealloc` +1, `evicted_entries` += the excess) and is logged.
  - **Not persisted:** the cardinality-cap tracking table, the debug clock and collision mode, and `bucket_advances`.
  - **Load safety:** tag sets are validated with `pssc_tagset_next()`, so capped-null values round-trip. File-sized allocations use `MCXT_ALLOC_NO_OOM`, and an allocation failure discards the file (LOG) rather than failing startup.
  - **Existing tests:** `007_store` and `024_cardinality_caps` expect each restart to start from an empty store, so they set `save = off`.

**Depends on:** 20261005-091225-15, 20261005-091225-21
**Open questions:** none
**Status:** done

### 20261005-091225-33: Roadmap: exemplars for excluded high-cardinality keys

**Description:** Store the most recent value of explicitly listed high-cardinality keys (for example `traceparent`) per entry, so users can jump from an aggregate to a real trace (§8 v1.x). The visibility rules from §6.11 apply.

*Owner's note (2026-10-05):* the task is kept. An exemplar stores the most recent value of a high-cardinality key (e.g. `traceparent`) per entry.

**Acceptance criteria:**
- The exemplar column shows the latest value without adding new entries.
- It is `NULL` for unprivileged roles viewing other roles' rows.
- Only keys in the exemplar GUC are stored. Total exemplar memory never exceeds the configured cap.
- The upgrade script is provided.

**Depends on:** 20261005-091225-17, 20261005-091225-20
**Decisions (2026-10-05):**
- Exemplar keys come from an explicit list in a dedicated GUC (e.g. `pg_stat_statement_context.exemplar_keys`). The denylist (`exclude_tags`) does not double as the exemplar list. A key may need to be both denylisted (so it isn't grouped by) and listed as an exemplar.
- Exemplar storage has a memory cap set by a config value (a postmaster-level GUC, since it sizes shared memory). Values that would exceed the cap are truncated or dropped (implementer's choice, documented and counted in `_info()`).

**Decisions (implementation, 2026-10-07; DESIGN.md §4.1, §5.1, §6.11, §6.13, §7):**
- GUCs: `exemplar_keys` (postmaster, default `''` = off and no memory, at most 8 keys of at most 63 bytes, no `*`, case-sensitive, duplicates ignored) and `exemplar_memory` (postmaster, kB, default 2MB). `max_tag_value_len` is not reused: exemplars are a separate budget.
- Sizing: each entry gets `exemplar_memory / max_entries` bytes (rounded down to MAXALIGN), split evenly per key; a value may take that share minus a 2-byte length, at most 256 bytes. Total (`_info().exemplar_shmem_bytes`) ≤ `exemplar_memory`.
- Overflow: **dropped**, not truncated (a truncated trace id is useless); the slot keeps its previous value; counted in `_info().exemplar_values_dropped`.
- Capture: after the step-4 rename, before the step-5 allowlist/denylist, independently of it; per-extractor `keys` apply, normalize/truncation/caps don't. First occurrence in a statement wins (override > comments > footer > appname). Nested statements with `nested_tags = inherit` inherit the outer exemplars.
- Storage: fixed per-entry slots after the counter ring, in exemplar_keys order; written under the entry spinlock with the call (no table-lock upgrade). Not saved across restarts; cleared by `_reset()`.
- (Superseded 2026-10-06 by 20261005-091225-29: 1.1 was folded into the unreleased 1.0 before v1.0.0; there is no upgrade script and no `_1_1` symbols.) SQL: extension 1.1 adds `exemplars jsonb` as the last column of the SRF/`_last_bucket`/the three views and `exemplar_shmem_bytes`, `exemplar_value_bytes`, `exemplar_values_dropped` at the end of `_info()`, via `--1.0--1.1.sql` (drop and recreate, new `_1_1` C symbols; the 1.0 symbols stay). `exemplars` is `NULL` exactly when `tags` is.

**Open questions:** none

**Status:** done

### 20261007-070036-1: Scope cardinality caps per (role, database) (security)

**Description:** Found by the 2026-10-07 security reviews (GPT 6 Astra finding 1, MEDIUM; Opus 5.5 P1, LOW). The cardinality-cap table (`src/cardcap.c`, §6.1) is server-wide: `cap_check()` hashes only (key, value). Two problems follow:
- **Membership oracle.** Once a key is at its cap, a value already admitted by another role stays a string while an unseen value becomes JSON `null`. Any role can therefore send a candidate value in its own statement comment and read its own row (the activity view, or the store) to learn whether another role or database already sent that value. This breaks §6.11's promise that tag visibility is "at least as strict as pgss".
- **Poisoning.** Any role can use up a key's cap, so every other role's values become `null` until a superuser runs `_reset()`.

Fix (decided by the owner 2026-10-07): add a postmaster GUC `pg_stat_statement_context.cardinality_cap_scope`, an enum with values `'server' | 'database' | 'role'` and default **`'role'`**:
- `role` mixes (userid, dbid) into the key and value hashes, so both the admitted set and the per-key counts are kept per (role, database). This matches pgss's entry key.
- `database` mixes only dbid.
- `server` keeps today's behaviour.

Use the same userid/dbid the store records for the entry. The table size stays `cardinality_cap_slots` (a single shared global limit, like `pgss.max`), so the layout of shared memory does not change. Scoping only makes the table fill faster, so document that busy multi-tenant servers may need more slots. When the table is full, values become `null`, as now. The `--1.0` SQL does not change. Land this before the v1.0.0 tag: update DESIGN.md (§6.1 and the GUC table, plus a §6.11 note), README, CHANGELOG and `docs/release-notes/v1.0.0.md`.

**Acceptance criteria:**
- A TAP test with two roles and `cardinality_cap = 1`, under the default scope: role B's first value is kept (not `null`) after role A filled the cap, and B can't tell whether A's value was admitted (the same candidate value gives the same result whether or not A sent it).
- Same isolation across two databases for the same role.
- With `cardinality_cap_scope = 'database'`, two roles in one database share the cap, and two databases do not.
- With `'server'`, the current server-wide behaviour, including the existing cap tests, is unchanged.
- The GUC is a postmaster GUC, rejects invalid values, and shows in `pg_settings`.
- The full suite passes on PG 14–18, and `scripts/check-frozen-sql.sh` passes.

**Depends on:** none
**Open questions:** none (scope toggle and default `role` decided 2026-10-07)
**Status:** done

### 20261007-070036-2: Apply cardinality caps under the identity that records the tags

**Description:** Found by the round-1 review of 20261007-070036-1 (gpt-6.1-sol). Caps are applied under `GetUserId()`/`MyDatabaseId` at extraction (`src/extract.c`), but the identity that records the tags can differ:
- An executor frame refreshes its userid at `ExecutorEnd` (`src/executor.c` ~174-179). A cursor opened under role A and closed under role B is therefore capped against A's scope and recorded under B. B can end up with more distinct values than its cap (e.g. cap 1: `a1` admitted for A, `b1` for B; the cursor records `a1` under B).
- A `SECURITY DEFINER` child inherits the caller's already-capped tags without calling the cap hook (`src/context.c` ~185-205) and records them under the definer's userid.
- With `nested_tags = scan`, a cursor created by a definer and then fetched or closed by the caller can show the definer's cap decisions in the caller's rows.

Each case needs membership in both roles, or a function whose author controls the tag text. So the impact is limited to inaccurate cap accounting plus a narrow decision leak between roles that already share a trust boundary. It is documented as a residual in DESIGN.md §6.1/§6.11. Fix: keep the uncapped normalized tags, and the scope they were capped under, on the frame. Re-apply the caps for the receiving identity when tags cross roles (inheritance, activity publication, recording). Keep the existing pgss-compatible end-time userid semantics.

**Acceptance criteria:**
- Regression tests for a cursor that changes role (opened under A, closed under B, cap 1) and for `SECURITY DEFINER` inheritance and returned cursors. Every recorded tag set obeys the cap of the identity it is recorded under.
- No measurable hot-path regression when roles don't change (one identity comparison).
- Full suite passes on PG 14-18.

**Depends on:** 20261007-070036-1
**Open questions:** none
**Status:** done

### 20261006-173342-1: CI fuzz smoke: oracle misses `capped_tags`; failure artifacts unreadable

**Description:** The first CI run (run 37535041023) failed the "Fuzz smoke" job for two reasons:

1. `fuzz/sql/regex_fuzz.pl` checks that the `_extract()` result has exactly `@RESULT_KEYS`. Item 20261005-091225-32 added `capped_tags` to the result (`src/extract_fn.c`) without updating the oracle, so every call reports `result keys ...`. Add the key. The driver sets no `cardinality_cap*` GUCs (the default cap is 0, meaning none), so the oracle should also require `capped_tags = 0`, and treat a negative value as a negative counter. The `--self-test` should catch this kind of drift in future: for example, compare `@RESULT_KEYS` with the keys that `src/extract_fn.c` pushes.
2. The "Upload artifacts" step failed with `EACCES` on `tmp/fuzz-sql/fail-round-1/server.log`, which the container wrote as another user. The upload steps in `.github/workflows/ci.yml` that collect files the Docker harness wrote to `tmp/` (fuzz-smoke, linux and linux-source) need to make those files readable first, for example with a `sudo chown -R` step on failure, or by having the harness write them readable.

**Acceptance criteria:**
- `perl fuzz/sql/regex_fuzz.pl --self-test` fails before the fix (it detects the key drift) and passes after.
- `fuzz/sql/run.sh --pgdg --pg 18 -- --duration 30` passes.
- The failure-upload steps of the Docker-based jobs can read what the harness wrote.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261006-192058-2: macOS: 022 superuser name, 006 malloc accounting, 017 utility slack in CI

**Description:** These are the remaining failures in the macOS CI cells (run 37535041023):
- **022:** `session_open('postgres')` (lines 276, 277, 348) assumes the bootstrap superuser is called `postgres`. On a host run it's the OS user (`FATAL: role "postgres" does not exist`). Use the cluster's actual superuser name.
- **006 #209, #219, #305 (PG14/15):** `pssc_extract_test_mem()`'s `malloc_used` (and `pssc_guc_test_malloc_used()`) only works through glibc's `mallinfo2()`, so on macOS it is NULL. PG14/15's regex engine mallocs directly, so the memory checks see nothing (growth −7168 bytes, 16384 bytes per generation). PG16+ allocates regexes in memory contexts, so it isn't affected. Implement `malloc_used` on macOS with `malloc_zone_statistics(NULL, &st)` (`size_in_use`) in both test modules. The precondition check ("a leak of the compiled regexes would show") must keep failing loudly, not skip, when neither source sees the regexes.
- **017 #10, #80 (PG14/15 CI only, not locally):** the utility-time excess, which is pgss's own `pgss_store()` of a new entry inside our clock reads, reached 3–14 ms on the `macos-latest` runners, against the 2 ms default slack. Set `PSSC_TEST_UTILITY_SLACK_MS` (for example 50) in the macOS job's "Run tests" step of `.github/workflows/ci.yml`, with a comment explaining why. Don't loosen the default.

**Acceptance criteria:**
- 022 passes on a host run where the superuser is not `postgres`, and still passes in Docker.
- On macOS PG14, `pssc_extract_test_mem()` reports a non-null `malloc_used`, and 006 passes. A test shows `malloc_used` grows after a known large malloc-backed allocation (it must fail before the fix).
- The Docker harness passes on PG14–18.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261006-192058-1: TAP tests: detect pg_stat_statements portably; fix the macOS failures in 010–019

**Description:** In CI run 37535041023, the macOS cells failed. Tests 010–019 detect pg_stat_statements with `-e "$pkglibdir/pg_stat_statements.so"`, but on PG16+ macOS the module suffix is `.dylib`. So on macOS PG16–18, every pgss parity check was silently skipped (`plan skip_all` in 014, 017 and 019). Fix the detection (any of `.so`, `.dylib`, `.dll`), ideally through one shared helper instead of eight copies. Make a missing pgss a hard failure when the harness sets `PSSC_REQUIRE_PGSS=1`, so this can't silently recur. Set that variable in `docker/run-tests.sh`, because every harness image and the macOS build install pgss. Then fix the failures that the corrected detection, or macOS itself, exposes:
- **013 #19:** the `set_conf('extractors', marginalia(position=prepend))` that test 19 relies on sits inside the pgss `SKIP` block. Move the setup out of it so test 19 doesn't depend on pgss.
- **014:** the real-load "spelling variant" cases hardcode `.so` (`pg_stat_statements.so`, `"$libdir/$P.so"`). Use the platform's suffix. The matcher-only cases (`pssc_guc_test_load_order_wrong`) stay as they are, because `src/utility.c` strips every known suffix.
- **012 #17–20:** with `track_utility = off`, a CALL/DO only nests on PG14–16 when pgss tracks it. Without pgss loaded, that falls back to this extension's own settings (src/utility.c `pgss_nests_utility`), so the children are top level there. Today the expectation assumes pgss is loaded. Make it follow `$have_pgss` and the version (on PG17+ it always nests).
- **012 #202:** `ORDER BY step, tags::text` depends on collation. In C or byte order, `tx_outer` sorts before `tx`. Make the order deterministic, e.g. `COLLATE "C"` with the expected list adjusted.

Verified locally on macOS (arm64, source builds, `docker/run-tests.sh`): with `.dylib` detection on PG18, 013, 014, 016, 017 and 019 pass, and only 012 #202 and 022 still fail.

**Acceptance criteria:**
- On macOS PG18 (host run of `docker/run-tests.sh` with `PSSC_REQUIRE_PGSS=1`), the pgss-dependent tests run rather than skip, and 010–019 pass; 012 and 013 also pass with pgss absent on PG14–16 and PG17+.
- The Docker harness passes on PG14–18 (`scripts/docker-test.sh <major>`).
- With `PSSC_REQUIRE_PGSS=1` and pgss genuinely missing, the tests fail rather than skip.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261006-192058-3: Host runs of docker/run-tests.sh: skip worktrees/, stop the server on failure

**Description:** `docker/run-tests.sh` (used on the host by the macOS CI cells and locally) has two problems:
- **It copies `worktrees/`.** The source copy excludes `./tmp` but not `./worktrees`, so the version-guard check scans every worktree's `src/compat.h` and fails. The same applies to `scripts/docker-test.sh` run from the main checkout, which mounts the whole repo. Exclude `./worktrees` (and keep the check scanning only the copied tree).
- **It leaks a server on failure.** `fail()` exits without stopping the server that `pg_start` started. In Docker the container dies with it, but on a host it stays up on the default port, and the next run then fails with "Address already in use". Stop it (`pg_ctl -m immediate`, tolerating "not running") from the failure and exit paths.

**Acceptance criteria:**
- A run from a checkout that contains `worktrees/` passes the version-guard step (a test that fails before the fix).
- After a failing host run, no postmaster from `$PSSC_WORK` is left running (a test that fails before the fix).
- The Docker harness passes on PG14–18.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261007-064749-1: 006: retry-cleanup precondition must not depend on runner speed

**Description:** In `test/t/006_regex.pl` (around line 662), the "attempt expired mid-compile, retried" checks from 20261006-080948-1 assert `cmp_ok($res{0}[1], '>', 0.6, "...compiling takes long enough to be interrupted mid-compile")`. The 'expire' injection fires 300 ms into the compile of `$MID`, so the clean compile only has to outlast 300 ms (plus margin) for the interrupt to land mid-compile. On fast GitHub runners the clean compile took 0.56–0.59 s, which failed this precondition (CI run 37630525613: Linux PG14 assert #208, Linux PG15 PGDG #208 and #218) even though the interruption itself worked. Make the precondition check what actually matters, with margin that doesn't depend on how fast the runner is. For example: confirm the expiry happened while the compile was still in progress, scale the expiry point to the measured clean compile time, or use a pattern that is reliably several times slower than the expiry point without making the test much slower.

**Acceptance criteria:**
- The precondition no longer fails just because a clean compile takes 0.3–0.6 s. Demonstrate this with a failing-before test, for example by making the compile faster in a test-only way or by running the check against the recorded CI timings.
- The precondition still fails, so the test is not vacuous, if the expiry lands after the compile has finished.
- The Docker harness passes on PG14–18, and on the macOS host for PG18.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261006-220356-1: Flaky TAP 017: utility time parity with pgss under assert builds

**Description:** Found during 20261005-091225-29 (release matrix, `scripts/docker-test.sh --assert 16`, PG 16.15). `test/t/017_lifecycle.pl` check 80, "track = all, track_utility = on (both): per-(userid, dbid, queryid, toplevel) calls and total_exec_time equal pgss's", failed once: `utility time 5.647375 vs pgss's 0.108292 (allowed difference 2.00108292)` for `RELEASE SAVEPOINT s2` (ours=1, pgss=1). It passed on rerun and in every other cell. Our `ProcessUtility` hook wraps pgss's, so a scheduling stall between the two timers lands in our time only; the 2 ms per-call slack (`PSSC_TEST_UTILITY_SLACK_MS`) is not enough on a loaded laptop Docker VM. Make the check robust without hiding real timing bugs (e.g. retry the workload once on a utility-time-only mismatch, or compare against a bound that tolerates rare single-call stalls while still failing on systematic differences). The failing log was `tmp/release-clone/tmp/logs/assert-16-flake.log` (scratch, may be gone).

**Acceptance criteria:**
- The check still fails if our utility timing is systematically wrong (demonstrated with a deliberate fault in a scratch copy).
- 017 passes 10 of 10 runs of `--assert 16` (or under an equivalent documented load recipe).

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261007-095213-1: 017: the stall rerun must not erase first-run non-timing failures

**Description:** Split from 20261006-220356-1 (its round-2 review finding, gpt-6.1-sol, not fixed within the 2-round limit). In `test/t/017_lifecycle.pl`, `compare()` reruns a configuration's workload once if the first pgss-parity comparison failed only on utility time and passed the coverage checks. Other per-configuration invariants are asserted after `compare()` returns, on state the rerun replaces:
- the `utility_missing_queryid` counter in the wrong-load-order case (the callback around line 1121 overwrites `$m0`);
- the lifecycle and tag-attribution checks;
- the presence checks for differing settings.

So a first-run failure in one of them, together with a utility stall, is erased when the rerun passes. The reviewer showed it in memory: complete coverage, a utility excess, and a first-run counter of 999 where 5 was expected. Everything passed after the rerun. A deterministic bug fails again on the rerun, so only a non-deterministic bug that coincides with a utility stall could be hidden.

**Acceptance criteria:**
- Each configuration's non-timing invariants are evaluated on the first run before any reset. The workload is retried only if they all pass; otherwise the first run's failures are reported.
- A scratch stub demonstrates it: a wrong first-run counter plus a utility excess fails and is not retried.
- 017 still passes repeated `--assert 16` runs under load; the full suite passes on PG 14-18.

**Depends on:** 20261006-220356-1
**Open questions:** none
**Status:** done

### 20261007-064749-2: 006: calibrate the retry-cleanup expiry from engine time, not client round trip

Split from 20261007-064749-1 (round-2 review finding, not fixed within 2 rounds). In `test/t/006_regex.pl` (around lines 674 and 691–705), the 'expire' point for the "attempt expired mid-compile, retried" checks is half of the previous `sex()` call's wall-clock round trip. That round trip includes SQL processing, IPC and time when Perl is descheduled, not just the engine's compile. A heavily delayed baseline (for example a 500 ms compile plus 600 ms of client delay) can push the expiry past the next compile. The required `attempts|interrupted = 2|1` then fails (`1|0` or `2|0`) even though the runtime behaves correctly.

**Acceptance criteria:**
- The expiry is calibrated from engine-local elapsed time, for example a test-module counter of the last compile's duration measured inside the backend. Alternatively, a bounded recalibration retries with a smaller expiry when the counters show the expiry missed the compile.
- `2|1` stays mandatory for the sample used by the allocation assertion.
- A test that injects client-side delay into the baseline (or simulates an inflated baseline) fails before the fix and passes after.
- The Docker harness passes on PG14–18.

**Depends on:** 20261007-064749-1
**Open questions:** none
**Status:** done

### 20261007-133120-1: Per-database/role settings: untagged, tags, exclude_tags, scan_window (superuser context)

**Description:** Reported by the owner on 2026-10-07: `ALTER DATABASE canvas SET pg_stat_statement_context.untagged = 'record'` fails with "cannot be changed now", because the GUC is `PGC_SIGHUP`. The audit decided (owner, 2026-10-07) to move four GUCs from `sighup` to `superuser` (`PGC_SUSET`) context, so that a superuser (or a role granted `SET`, PG 15+) can set them per database, per role, or per session: `untagged`, `tags`, `exclude_tags` and `scan_window`. Leave the others alone:
- `save` and `reclaim_worker_interval` stay `sighup`: they are server-wide (postmaster/background worker).
- `extractors`, `normalize`, `cardinality_cap` and `cardinality_cap_overrides` stay `sighup` for now. Mid-statement changes to the compiled regexes and caps need more work; they could be a later item.

Each backend reads the four GUCs at extraction, and store entries are keyed by dbid, so differing values per database don't collide. Check:
- **Mid-statement changes:** a function `SET` clause, `SET LOCAL`, or a GUC rollback on (sub)transaction abort can change `tags`/`exclude_tags` while an outer frame still holds data that came from the old parsed value. Examples are recap candidates (20261007-070036-2), which are re-filtered under the current allowlist at recap time. The assign hooks must not free anything that is still referenced. Assign hooks must also not throw.
- **Parallel workers** must use the leader's values (PostgreSQL restores GUCs).
- **`_extract()` and the activity view** follow the session's values.

Update DESIGN.md (GUC table and any "sighup" wording, e.g. §4.1), docs/configuration.md (Reference table and each section; the context explanation added in 80c6675), CHANGELOG.md and docs/release-notes/v1.0.0.md. The frozen 1.0 SQL does not change. Land this before the v1.0.0 tag, then rerun the release matrix.

**Acceptance criteria:**
- TAP:
  - `ALTER DATABASE ... SET` and `ALTER ROLE ... SET` work for all four GUCs, and take effect in new sessions of that database or role.
  - With `untagged = 'record'` only in database A, an untagged statement is recorded in A and not in B.
  - With a per-database `tags` allowlist, each database keeps different keys.
  - A non-superuser can't `SET` them (permission error). On PG 15+, `GRANT SET ON PARAMETER` lets a role set them.
  - `pg_settings.context` shows `superuser` for the four, and `sighup` for the others.
  - Changing `tags` through a function's `SET` clause during a statement whose outer frame has recap candidates (role-scoped caps, a SECURITY DEFINER function) doesn't crash and gives a coherent result. Also run this under assert/Valgrind if practical.
- Full suite passes on PG 14-18; `scripts/check-frozen-sql.sh` passes.

**Depends on:** none
**Open questions:** none (scope decided by the owner 2026-10-07)
**Status:** done

### 20261008-065635-4: Standby, restart and promotion qualification

**Description:** TST-3 and DOC-4. There is no primary/streaming-standby test. The persistence condition that accepts a clean standby shutdown (`src/store.c` about lines 1874–1875, `DB_SHUTDOWNED_IN_RECOVERY`) can be deleted and `028_persist.pl` still passes. Add a TAP test with a primary and a streaming standby (PostgreSQL::Test::Cluster `init_from_backup` with `has_streaming`) that checks:
- read-only tagged statements on the standby are recorded in the standby's own store
- the store is instance-local: the primary's entries don't appear on the standby, and vice versa
- with `save = on`, a clean standby restart reloads its statistics (the mutation above must make the test fail)
- after promotion, the new primary keeps recording and its existing in-memory history survives
- an immediate (crash) shutdown of the standby discards saved statistics as designed

Document the behavior in a new "Replicas and failover" section of docs/limitations.md (or another fitting doc), linked from the README: histories are per instance and not replicated; `shared_preload_libraries` and GUCs must be set on each instance; and what happens on failover.

**Acceptance criteria:**
- The new TAP test passes on PG 14–18 and fails with the `DB_SHUTDOWNED_IN_RECOVERY` condition removed (state in the commit that this was checked).
- The docs section exists and is accurate.

**Depends on:** none
**Open questions:** none
**Status:** done

### 20261008-065635-10: Consolidated shared-memory sizing guidance

**Description:** DOC-6 and PERF-8. The sizing information is spread across the docs, and the worked "about 8.6 MB" example is low (measured: 9,261,312 bytes at the defaults; 75,719,568 bytes at `bucket_count = 288`). Write one sizing section (in docs/configuration.md) with:
- a formula, derived from `src/store.c` (about lines 230–323), `src/cardcap.c` (about lines 390–465) and `src/activity.c` (about lines 59–93), covering entries × (header + tags + bucket slots), dynahash overhead, exemplars when enabled, the cap table, and the activity slots per `MaxBackends`
- a table of totals for representative settings: defaults, 24 h of history, `max_entries` 50k, exemplars on, `max_connections` 5000
- guidance on suitable upper limits for small instances

The `_info()` columns `shmem_bytes`, `cap_shmem_bytes` and `exemplar_shmem_bytes` should together account for the whole request. If activity memory isn't reported anywhere, either add it to the startup log line (no SQL change), or explain how to compute it.

**Acceptance criteria:**
- A TAP test checks the documented formula against `_info()` for at least three settings combinations (to within the stated rounding). The docs table is produced by a script or by that test, not typed by hand.

**Depends on:** none
**Open questions:** none
**Status:** done

## Dropped

Items removed from BACKLOG.md without being built, with the reason.

### 20261005-091225-31: Roadmap: extractor config file (conditional)

**Description:** If Q3 is decided in favor, add `pg_stat_statement_context.config_file`, which holds the extractor DSL for complex setups. Read and validate it at reload, with the same all-or-nothing semantics as the GUC `check_hook`. If Q3 is decided against, close this task.

**Acceptance criteria:**
- Reloading with a valid file applies it.
- An invalid file is rejected, and the previous config stays active.
- The behavior is documented.

**Depends on:** 20261005-091225-8
**Open questions:**
- §11 Q3: should the extractor DSL live in one GUC or in a separate config file? If in a file, what is the file format, and how does it interact with the `extractors` GUC (precedence)?

**Status:** dropped
**Dropped:** 2026-10-05 — §11 Q3 decided against: configuration stays in GUCs only. The from-SQL path is `ALTER SYSTEM SET ...; SELECT pg_reload_conf();` (the `extractors` GUC is `sighup`), documented by task 20261005-091225-28.

### 20261005-091225-36: Roadmap: `track_planning` (planning time)

**Description:** Add an optional `planner_hook` that records planning time and plan counts per tag set, as in pgss `track_planning` (§3.2, §8). The planner hook runs before `ExecutorStart`, so no frame exists yet. Resolve tags at plan time without adding cross-execution memoization that violates §3.3.

**Acceptance criteria:**
- Planning statistics appear in new columns (with an upgrade script) and match pgss `total_plan_time` per `queryid`.
- Overhead is benchmarked.

**Depends on:** 20261005-091225-17, 20261005-091225-20
**Open questions:** none
**Status:** dropped
**Dropped:** 2026-10-05 — out of scope for a pg_stat_statements companion, which stores only `calls` and `total_exec_time` per context; pgss `track_planning` already records planning time per `queryid`.

### 20261005-091225-37: Roadmap: `utility_textid` column for PG14/15

**Description:** Add a separate `utility_textid` column (§6.6, §8). It hashes the utility statement with comments removed by the lexer: literal and quoted-identifier contents stay unchanged, and whitespace is collapsed only outside tokens. It never replaces `queryid`.

**Acceptance criteria:**
- On PG14/15, DDL that differs only in comments shares one `utility_textid` but keeps distinct `queryid`s.
- Literal differences still produce distinct IDs.

**Depends on:** 20261005-091225-4, 20261005-091225-18, 20261005-091225-20
**Open questions:**
- §11 Q4: is a separate `utility_textid` worth adding at all, given that `queryid` must stay equal to core and pgss? If it is, should it also be populated on PG16+?

**Status:** dropped
**Dropped:** 2026-10-05 — §11 Q4 answered "not worth it": pgss has the same PG14/15 utility `queryid` behavior.

### 20261005-091225-40: Roadmap: error and cancellation counts per tag set

**Description:** Count errors at the hook exception boundaries (`PG_CATCH` in Run/Finish/ProcessUtility) while the frame is still known (§6.9, §8 v2). Note each error in backend-local memory, rethrow, and flush it to shared memory later, for example from an abort callback. Attribute an error only to the innermost recorded frame. Don't use `emit_log_hook` as the counter source.

**Acceptance criteria:**
- A failing statement increments the error count for its tag set exactly once.
- Errors caught in PL/pgSQL `EXCEPTION` blocks don't count against the outer statement.
- Parse and planning errors are not counted.

**Depends on:** 20261005-091225-18, 20261005-091225-20
**Open questions:**
- §11 Q6: what exact rules deduplicate nested errors and errors caught by subtransactions?
- Should cancellations (and timeouts) be counted separately from errors?

**Status:** dropped
**Dropped:** 2026-10-05 — out of scope for a pg_stat_statements companion; §11 Q6 is moot.

### 20261005-091225-43: Roadmap: wait-event sampling attributed to tags

**Description:** Add a background worker that samples backends' wait events together with their current tags from the activity view, and aggregates the samples per tag set (§8 v3).

**Acceptance criteria:**
- Under a lock-contention workload, wait events are attributed to the right tags.
- The worker's overhead is measured.

**Depends on:** 20261005-091225-39
**Open questions:**
- Where are samples stored: extra counters per entry, or a separate structure and view?
- How are the sampling rate and retention configured?

**Status:** dropped
**Dropped:** 2026-10-05 — out of scope for a pg_stat_statements companion (needs a sampling worker and separate store).

### 20261005-091225-44: Roadmap: OS-level CPU/I/O (`getrusage`) per tag set

**Description:** Record user and system CPU time and OS-level read and write bytes per execution via `getrusage`, in the style of `pg_stat_kcache` (§8 v3). Add the counters and columns with an upgrade script.

**Acceptance criteria:**
- A CPU-bound statement shows matching CPU time.
- Values are inclusive in the same sense as §6.4, which is documented.
- Overhead is benchmarked on Linux and macOS.

**Depends on:** 20261005-091225-18, 20261005-091225-20
**Open questions:** none
**Status:** dropped
**Dropped:** 2026-10-05 — out of scope for a pg_stat_statements companion; OS-level resource usage is `pg_stat_kcache`'s job.
