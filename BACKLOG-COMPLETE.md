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
