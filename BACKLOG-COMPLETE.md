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
