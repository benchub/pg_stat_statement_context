# Maintaining pg_stat_statement_context

This page is for maintainers. It has three parts:

- the pg_stat_statements (pgss) behaviors this extension mirrors, and where each one lives on both sides;
- what to re-check when a new PostgreSQL minor or major release ships;
- how coexistence with other hook-using extensions is tested.

`DESIGN.md` §3.2, §6.4–6.12 and §9 explain why. This page says *where*.

pgss references are to `contrib/pg_stat_statements/pg_stat_statements.c` in the PostgreSQL source tree, on the `REL_<N>_STABLE` branch for each supported major. Line numbers drift, so search for the names given here.

## 1. Mirrored pg_stat_statements behaviors

The goal is parity: with the same `track` and `track_utility` settings, every recorded `(userid, dbid, queryid, toplevel)` has the same `calls` as pgss (`test/t/017_lifecycle.pl`, `012`, `013`, `036`). Each behavior below is a place where that parity can break when upstream changes.

### 1.1 Nesting level and `toplevel`

| Behavior | This extension | pgss |
|---|---|---|
| The executor nests in `ExecutorRun` and `ExecutorFinish`. Statements run while a statement executes are not top level. | `src/executor.c`: `pssc_ExecutorRun`, `pssc_ExecutorFinish` raise `pssc_nesting_level` (`src/context.c`, `pssc_frame_enter`/`pssc_frame_leave`) | `pgss_ExecutorRun`, `pgss_ExecutorFinish`: `exec_nested_level++` (PG14–16), `nesting_level++` (PG17+) |
| Planning is a nesting level on PG17+ only, even when pgss does not track planning. | `src/context.c`: `pssc_planner`, gated by `PSSC_HAS_PLANNER_NESTING` (`src/compat.h`) | `pgss_planner`: `plan_nested_level++` (PG14–16, not used for `toplevel`), `nesting_level++` in both branches (PG17+) |
| A utility other than EXECUTE/PREPARE nests always on PG17+. On PG14–16 it nests only when pgss itself tracks it, which depends on pgss's own `track_utility`, `track` and `PGSS_HANDLED_UTILITY`. | `src/utility.c`: `pgss_nests_utility`, gated by `PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES` (`src/compat.h`) | `pgss_ProcessUtility`: `exec_nested_level++` only inside the tracking branch (PG14–16); `nesting_level++` in both branches (PG17+) |
| EXECUTE and PREPARE never nest and are never recorded. Their cost goes to the prepared statement through the executor hooks. | `src/utility.c`: `is_execute_or_prepare`, early chain in `pssc_ProcessUtility` | `pgss_ProcessUtility`: the `!IsA(parsetree, ExecuteStmt) && !IsA(parsetree, PrepareStmt)` branch |
| `toplevel` and `track = top` use the nesting level at record time, so a cursor left open and closed at transaction end counts as top level. | `src/context.c`: `pssc_frame_refresh` (called from `pssc_ExecutorEnd` and after a utility returns) | `pgss_ExecutorEnd` / `pgss_ProcessUtility` call `pgss_store(..., nesting_level == 0)` (the PG14–16 name is `exec_nested_level`) |
| `track = top` / `all` / `none`. | `src/executor.c` and `src/utility.c`: `tracked_at_level` | `#define pgss_enabled(level)` |
| Parallel workers record nothing. | `IsParallelWorker()` checks in `pssc_ExecutorStart` and `pssc_ProcessUtility` | `pgss_enabled()` tests `!IsParallelWorker()` |

### 1.2 Which utility statements count

| Behavior | This extension | pgss |
|---|---|---|
| Every utility counts except EXECUTE and PREPARE. DEALLOCATE counts from PG17 and is excluded on PG14–16. | `src/utility.c`: `pgss_records_utility`; `PSSC_PGSS_RECORDS_DEALLOCATE` (`src/compat.h`) | `#define PGSS_HANDLED_UTILITY(n)` (PG14–16 lists `DeallocateStmt`; PG17+ has no such macro, and DEALLOCATE gets a jumbled queryId) |
| Utility time is measured around the whole chained call. Ours wraps pgss's, so our `total_exec_time` is at least pgss's. | `pssc_ProcessUtility` (`INSTR_TIME_*` around `chain()`) | `pgss_ProcessUtility` (`INSTR_TIME_*` around `prev_ProcessUtility`) |

### 1.3 GUCs read by name

On PG14–16, `src/utility.c: pgss_utility_settings` (through `loaded_guc`) reads these pgss settings, because whether pgss nests a utility there depends on its settings, not ours:

- `pg_stat_statements.track_utility`
- `pg_stat_statements.track` (`top` / `all` / `none`)

A placeholder (pgss not loaded) falls back to this extension's own settings. If pgss renames either GUC or changes its values, `pgss_utility_settings` silently falls back. Test 012 (differing settings, both orders) catches that.

`src/utility.c: pssc_load_order_wrong` reads `shared_preload_libraries` and looks for the library names `pg_stat_statements` (`PGSS_LIBRARY_NAME`) and `pg_stat_monitor` (`zeroing_libraries`).

### 1.4 Query IDs

| Behavior | This extension | pgss / core |
|---|---|---|
| The core queryId (`compute_query_id`) is used unchanged. This extension never computes, changes or zeroes `queryId`. | `pssc_frame_create`, `pssc_utility_frame_init` (`src/context.c`) copy `plannedstmt->queryId` / `pstmt->queryId` | When `pg_stat_statements.track_utility` is on, `pgss_post_parse_analyze` zeroes the queryId of EXECUTE, PREPARE and DEALLOCATE on PG14–16 (`!PGSS_HANDLED_UTILITY`) and of EXECUTE only on PG17+. When `track_utility` is on and `pgss_enabled(level)` holds, `pgss_ProcessUtility` zeroes `pstmt->queryId` of every utility, EXECUTE and PREPARE included, *before* chaining. |
| queryId 0 is never recorded. A tracked utility that arrives with queryId 0 is counted in `_info().utility_missing_queryid`. | `pssc_ProcessUtility` (`frame->queryId == 0`), `pssc_store_count_utility_missing_queryid` (`src/store.c`) | `pgss_store` returns at once when `queryId == 0`, on PG14–18. pgss records a utility under the queryId core gave it, which `pgss_ProcessUtility` saved (`saved_queryId`) before zeroing `pstmt->queryId`. |
| Core computes utility queryIds, not pgss. On PG14/15 the queryId is a hash of the statement text, comments included, so each tag set gives a different queryId. On PG16+ it is a parse-tree jumble that ignores comments, so the same utility shares one queryId across tag sets and untagged runs. | Nothing to do; documented in `docs/limitations.md` | core `src/backend/utils/misc/queryjumble.c` (`JumbleQuery` → `compute_utility_query_id`, PG14/15) / `src/backend/nodes/queryjumblefuncs.c` (PG16+) |
| The queryId is an `int64` on PG18 and a `uint64` on PG14–17. It is stored as `int64`. | `src/store.h` `PsscEntryKey.queryid`, `src/context.h` `PsscFrame.queryId` | PG18 `pgss_store(..., int64 queryId, ...)`; PG14–17 `uint64` |
| Load order: because pgss zeroes the queryId before chaining, pgss must be listed **before** this extension in `shared_preload_libraries`. A WARNING at startup reports the wrong order (also for pg_stat_monitor, §4). | `src/utility.c`: `pssc_load_order_wrong`, `pssc_utility_check_load_order`; tests 014, 041 | `pgss_ProcessUtility` (the comment "Force utility statements to get queryId zero") |

### 1.5 Executor timing

| Behavior | This extension | pgss |
|---|---|---|
| `queryDesc->totaltime` is allocated with `InstrAlloc(1, INSTRUMENT_ALL, false)` in the query context if nobody else has done it. Both read the same `totaltime`, so plannable `total_exec_time` is exactly equal. | `src/executor.c`: `pssc_ExecutorStart` | `pgss_ExecutorStart` |
| A statement is recorded at `ExecutorEnd`, only when it has `totaltime`, after `InstrEndLoop`. | `src/executor.c`: `pssc_ExecutorEnd`, `record_frame`; `src/counters.h`: `pssc_exec_ms_from_totaltime` | `pgss_ExecutorEnd` |
| `ExecutorEnd` records first and then chains, while the executor state is still intact. | `pssc_ExecutorEnd` | `pgss_ExecutorEnd` |

## 2. When a new PostgreSQL release ships

### Every new minor release

1. Run the matrix on the new minors: `scripts/docker-test.sh N` for each major (PGDG pulls the latest minor) and `scripts/docker-test.sh --assert N`. Each cell runs the suite on the testing build (`make PSSC_TESTING=1`, with the TEST-ONLY modules), then checks the release build's exports (`scripts/check-release-exports.sh`) and runs pg_regress and the TAP tests that need no TEST-ONLY module against it. Also run `scripts/docker-test.sh --valgrind 18` and `--valgrind-tap 18` (a TAP subset with its servers under Valgrind, see `DESIGN.md` §9).
2. Diff `contrib/pg_stat_statements/pg_stat_statements.c` between the previous and the new minor tag of each supported branch (`git diff REL_17_6..REL_17_7 -- contrib/pg_stat_statements`). Look for changes to anything in §1: `pgss_enabled`, `PGSS_HANDLED_UTILITY`, nesting counters, the queryId zeroing in `pgss_ProcessUtility` / `pgss_post_parse_analyze`, the `totaltime` allocation, and `pgss_store`'s `toplevel` argument. Back-patched fixes do happen. For example, upstream `8700851352a8` changed how a cached utility statement is re-parsed (14.10, 15.5, 16.1; see `DESIGN.md` §9).
3. Also diff `src/include/tcop/utility.h`, `src/include/executor/executor.h` and `src/include/optimizer/planner.h` for hook signature changes. They are rare in minors, but the macros in `src/compat.h` assume them.
4. If `.github/workflows/oldest-minors.yml` fails while `ci.yml` passes (or the reverse), a behavior changed within the major. Find the minor that changed it and add a runtime check (as `017` does), not a minor-version check.

### Every new major release

1. Everything under "Every new minor release", between the previous major's branch and the new one.
2. Re-check each `src/compat.h` macro against the new major. The comment above each `#if` says what to look at. Run `scripts/check-version-guards.sh`.
3. Check whether pgss changed any of the nesting rules in §1.1 (PG17 merged `exec_nested_level` and `plan_nested_level` into `nesting_level`, which changed planner and utility nesting), the utility list in §1.2 (PG17 dropped `DeallocateStmt` from `PGSS_HANDLED_UTILITY`), the GUC names in §1.3, or the queryId type or jumbling in §1.4 (PG16 jumbles utilities; PG18 made queryId `int64`).
4. Add the major to the `ci.yml` matrices, `<major>.0` to `.github/workflows/oldest-minors.yml`, and the PGDG package names to `docker/Dockerfile` (pgaudit and pg_hint_plan for 036 usually arrive a few weeks after the release; until then the cell skips them).
5. Run `test/t/036_hook_coexistence.pl` against the new major's auto_explain, pgaudit, pg_hint_plan and (if you can build it) pg_stat_monitor. These extensions also change their hooks across majors.

## 3. Oldest-minor policy

`ci.yml` tests the latest minor of each major, on push and on pull requests. `.github/workflows/oldest-minors.yml` adds one cell per major on the oldest release the harness can run. It builds from the official tarball with `docker/Dockerfile.source` and is cached like the `ci.yml` source builds. It runs weekly and on demand (`workflow_dispatch`), because a cold source build takes several minutes per cell.

| Major | Oldest cell | Why |
|---|---|---|
| 14 | 14.6 | 14.0–14.5 do not install the `PostgreSQL::Test::*` TAP modules, so the TAP suite can't run. Users on those releases are covered only by the pg_regress suite, which you can run by hand. |
| 15–18 | N.0 | |

Reproduce a cell locally with `scripts/docker-test.sh 14.6` (or `15.0`, ...).

## 4. Coexistence with other hook-using extensions

`test/t/036_hook_coexistence.pl` preloads this extension next to other libraries that install the same hooks. It loads `pg_stat_statements` first, then runs both orders of this extension and the other library. In each order it checks two things. First, the recorded tags and `calls`, including nested statements and a utility, exactly match the expected rows and pgss. Second, the other library's own output is intact.

| Library | Source in the harness | What 036 checks besides recording |
|---|---|---|
| auto_explain | contrib, every harness server (`docker/build-postgres.sh` installs it for source builds) | `log_analyze` plans for each top-level and nested statement |
| pgaudit | PGDG package `postgresql-N-pgaudit` (`docker/Dockerfile`) | one `AUDIT:` line per statement and nested statement |
| pg_hint_plan | PGDG package `postgresql-N-pg-hint-plan` | `/*+ SeqScan(t) */` changes the plan and is reported as used, also with sqlcommenter and marginalia tag comments in the same statement; the hint comment produces no tags |
| pg_stat_monitor | not in PGDG apt, so skipped in the images; checked by hand against 2.4.0 built from source (036 and 041) | `calls` match this extension's |

036 uses no TEST-ONLY module or hook, so `docker/run-tests.sh` runs it against both the testing build (`make PSSC_TESTING=1`) and the release build ([DESIGN.md §9](../DESIGN.md#9-testing-strategy)), with the same `PSSC_REQUIRE_MODULES`. A module that isn't installed is skipped, and the skip is reported on stderr. `docker/Dockerfile` writes the modules it installed to `/usr/local/share/pssc-test-modules`, and `docker/run-tests.sh` passes them in `PSSC_REQUIRE_MODULES`. A listed module that is missing makes the test fail rather than skip.

**pg_stat_monitor must also be loaded before this extension.** Like pgss, `pgsm_ProcessUtility` zeroes the queryId of the utilities it tracks before it chains. The working order is:

```
shared_preload_libraries = 'pg_stat_statements, pg_stat_monitor, pg_stat_statement_context'
```

In that order pgss sits inside pg_stat_monitor and records no utilities. That is pg_stat_monitor's doing and happens without this extension too. 036 pins both orders. The load-order WARNING (`pssc_load_order_wrong`) also reports pg_stat_monitor listed after this extension; `test/t/041_load_order_pgsm.pl` checks it in the server log and, like 036, is skipped unless pg_stat_monitor is installed (and fails if `PSSC_REQUIRE_MODULES` lists it). 014 checks the matcher for pg_stat_monitor without it. If pg_stat_monitor stops zeroing utility queryIds, drop it from `zeroing_libraries`.

When a new release of any of these extensions changes its hooks, re-run 036 against it.
