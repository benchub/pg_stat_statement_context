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
