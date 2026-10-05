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
