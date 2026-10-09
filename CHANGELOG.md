# Changelog

All notable changes to `pg_stat_statement_context` are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/). The extension's SQL version (`default_version` in the `.control` file) is the release's `MAJOR.MINOR`; released SQL scripts are never edited, and SQL changes ship as upgrade scripts (developer reference: [DESIGN.md §7](DESIGN.md#7-sql-interface-v1)).

## [Unreleased]

### Added

- **Upgrade and uninstall guide** ([docs/upgrading.md](docs/upgrading.md)): library-only and SQL-version upgrades, restarts, when saved statistics are discarded, `pg_upgrade`, downgrading, uninstalling, identifying the loaded build. The maintainers' version discipline is a developer reference: [docs/maintaining.md](docs/maintaining.md#5-version-discipline).

### Changed

- **Release tarballs leave out development-only files.** `git archive` (and so GitHub's source downloads) no longer includes the agent instructions, the project backlog or the driver experiments (`export-ignore` in `.gitattributes`). Everything needed to build, install, test, package and regenerate `docs/benchmarks.md` is kept; `scripts/check-release-tarball.sh` builds and tests an extracted tarball. The user docs no longer point into development material, and label their links to developer references (`DESIGN.md`, `docs/maintaining.md`). The README has a glossary and a `NOTICE` file.
- **Release builds ship no test-only code.** A plain `make` (and the `.deb` packages) builds a library without the debug clock, forced collisions or fault-injection hooks. It exports only `_PG_init`, `Pg_magic_func`, the SQL functions and the reclaim worker's entry point; `scripts/check-release-exports.sh` checks this. `make PSSC_TESTING=1` builds the testing library that the TEST-ONLY modules and the full TAP suite need.
- **Benchmarks requalified.** `bench/run.sh` now runs, from one command, simple and prepared protocol, read-only and TPC-B-like write workloads at 1, N and 4N clients (256 opt-in), untagged and regex/normalize configurations, a store pre-populated with 5,000 entries, exporter-recipe readers every 15 s and 1 s, nested PL/pgSQL with `track = all`, 10,000-element `IN` lists (`append`/`any`, with and without a trailing `;`) and sustained eviction, against the release build. Configurations are paired with a `pg_stat_statements`-only baseline in randomized blocks, and the report gives server CPU per statement, TPS and p50/p95/p99 with 95% confidence intervals. Raw results are committed under `bench/results/`, and `docs/benchmarks.md` is generated from them; `bench/README.md` has instructions for dedicated Linux x86 and Graviton hardware. The performance statements in the README, DESIGN.md and the 1.0.0 release notes now match what this data supports (see [docs/benchmarks.md](docs/benchmarks.md)).
- **Supported versions are explicit.** The build fails with `#error` on PostgreSQL 19 or later ("not yet validated"); `make PSSC_ALLOW_UNTESTED_PG=1` overrides it. On PostgreSQL 18, `pg_get_loaded_modules()` shows the library's name and version.

### Security

- The statistics file loader recomputes each entry's `tags_hash` from its tags, and discards the file (LOG "ignoring invalid data") when they disagree.

## [1.0.0] - 2026-10-06

First release. SQL extension version `1.0`. Supports PostgreSQL 14, 15, 16, 17 and 18 from one source tree. Release notes: [docs/release-notes/v1.0.0.md](docs/release-notes/v1.0.0.md).

### Added

- **Per-context statement statistics.** `calls` and `total_exec_time` per (`userid`, `dbid`, `queryid`, `toplevel`, tag set), joinable to `pg_stat_statements` on (`userid`, `dbid`, `queryid`, `toplevel`); the `queryid` is always the core query ID. Executor and `ProcessUtility` hooks; `track` (`none`/`top`/`all`), `track_utility`, and `nested_tags` (`inherit`/`scan`/`none`) for nested statements from PL/pgSQL, triggers and SPI.
- **Tag extraction from SQL comments.** A backend-independent lexical comment scanner (prepend/append/any positions, `scan_window` heuristic tail scans for long statements) and an extractor DSL configured with GUCs: `sqlcommenter`, `marginalia` and `regex` extractors, validated by the GUC check hook and changeable at run time with `ALTER SYSTEM` + `pg_reload_conf()`. Backends bound each regex compile attempt to 100 ms and disable a pattern that can't compile (`regex_compile_failures`); see DESIGN.md §4.2 for the config-file and postmaster exceptions.
- **Context from `application_name`** with the `appname` extractor.
- **`tags_override`**: set tags per session or transaction (`SET LOCAL pg_stat_statement_context.tags_override = ...`), read at execution time, for drivers that can't add comments or that reuse prepared statements.
- **Tag pipeline:** key allowlist (`tags`, default `action, controller, job`) and denylist (`exclude_tags`), per-key value **normalization** rules (`normalize`, e.g. `/users/\d+` → `/users/:id`), size limits (`max_tags`, `max_tag_value_len`, `max_tagset_bytes`), and the **`untagged` policy** (default `skip`; `record` keeps untagged statements with `{}`). `tags`, `exclude_tags`, `untagged` and `scan_window` are `superuser` settings, so they can also be set per database, per role or per session (`ALTER DATABASE/ROLE ... SET`, `SET`, a function's `SET` clause) by a superuser or, on PG 15+, a role granted `SET` on them.
- **Per-key cardinality caps** (`cardinality_cap`, `cardinality_cap_overrides`, `cardinality_cap_slots`): values past a key's cap are recorded as JSON `null`. Caps are counted per (role, database) by default (`cardinality_cap_scope = role`), so a role can neither use up another role's caps nor learn from its own rows whether another role sent a value (scoped slot positions are keyed with a random secret, redrawn by `pg_stat_statement_context_reset()`); `database` and `server` share caps more widely and bring both risks back. Every row obeys the caps of the role it is recorded under, even when the tags were extracted under another role (cursors finished after `SET ROLE` or by a `SECURITY DEFINER` function's caller, tags inherited into `SECURITY DEFINER` code): the caps are re-applied for that role.
- **Rolling time buckets:** a fixed-size shared-memory store (`max_entries`) where each entry holds a ring of `bucket_count` buckets of `bucket_interval` (default 12 × 5 min), rolled over lazily with a lock-free boundary advance.
- **Eviction under pressure:** dead entries (every bucket expired) are reclaimed first, then live entries by pgss-style usage. Victims are chosen from a compact 24-byte-per-entry candidate array with a bounded heap, so a pass no longer walks or sorts the whole hash table.
- **Reclaim worker** (`reclaim_worker`, postmaster, default `off`; `reclaim_worker_interval`, sighup, default `10s`): an optional background worker that frees dead entries (all buckets expired) on idle systems, without waiting for an insert into a full table. It counts them in `pg_stat_statement_context_info().reclaimed_entries`, leaves `dealloc` and `evicted_entries` alone, and never evicts live entries or decays usage. With it off nothing changes.
- **Statistics survive clean restarts** (`save`, sighup, default `on`, as `pg_stat_statements.save`). After a clean shutdown the postmaster saves the store to `pg_stat/pg_stat_statement_context.stat`, and the next start loads it and removes the file. The loaded state keeps the entries, their buckets, usage, counters, `stats_reset` and the bucket epoch.
  - A file from another file format, PostgreSQL major or extension version is discarded, and so is one saved with a different `bucket_interval` or `bucket_count`. A corrupt file is also discarded.
  - A smaller `max_entries` evicts the excess in eviction order. Tag sets that no longer fit `max_tagset_bytes` are skipped.
  - Nothing is saved after an immediate shutdown or a crash.

  Each case is logged.
- **Exemplars** for high-cardinality keys that are not grouped by, such as `traceparent` (`exemplar_keys`, postmaster, default `''` = off, at most 8 keys; `exemplar_memory`, postmaster, default `2MB`): the most recent value of each listed key is stored per entry and shown in the `exemplars` jsonb column of the stats views (`{}` when none; `NULL` when `tags` is). Values longer than the per-value room (`pg_stat_statement_context_info().exemplar_value_bytes`) are dropped and counted in `pg_stat_statement_context_info().exemplar_values_dropped`; `pg_stat_statement_context_info().exemplar_shmem_bytes` reports their memory. Exemplars are not saved across restarts.
- **SQL interface:** `pg_stat_statement_context(showtags, merge_buckets)` with the `pg_stat_statement_context` (per bucket) and `pg_stat_statement_context_totals` (per entry) views; `pg_stat_statement_context_last_bucket` (the last closed, final bucket); `pg_stat_statement_context_activity` (current tags of every backend); `pg_stat_statement_context_info()`; `pg_stat_statement_context_counters()` (the `pg_stat_statement_context_info()` columns but `oldest_bucket`, without scanning the table: cheap enough for every scrape); `pg_stat_statement_context_reset()`; and the debug function `pg_stat_statement_context_extract()`.
- **Exporter-friendly surface:** monotonic per-entry counters (`calls_total`, `exec_time_total`, `stats_since`), bucket metadata in `pg_stat_statement_context_info()` (`buckets`, `bucket_seconds`, `current_bucket_start`, `last_closed_bucket_start`, `stats_reset_epoch`), and separate `reclaimed_entries` / `evicted_entries` / `dropped_records` counters.
- **Visibility rules** like `pg_stat_statements`: other roles' `queryid` and `tags` are `NULL` without `pg_read_all_stats`; tags from other databases are converted (or escaped) to the reader's encoding.
- **`shared_preload_libraries` load-order detection:** a `WARNING` if the library is loaded before `pg_stat_statements`.
- **Integrations:** recipes for postgres_exporter, sql_exporter and the OpenTelemetry Collector, a Grafana dashboard and a least-privilege monitoring role (`docs/integrations/`, tested by `scripts/test-integrations.sh`).
- **Documentation:** configuration, extractors, SQL interface, limitations, and benchmarks (`docs/`).
- **Testing:** unit tests (ASan/UBSan), a pg_regress suite, TAP tests (lifecycle, pgss parity, store, buckets, eviction, reconfiguration), libFuzzer and SQL-level fuzzers, pgbench benchmarks (`bench/`), and a CI matrix of PG 14–18 × Linux/macOS plus cassert and Valgrind builds.
- `scripts/check-frozen-sql.sh`: fails if a released SQL script is edited in place.

[Unreleased]: https://github.com/benchub/pg_stat_statement_context/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/benchub/pg_stat_statement_context/releases/tag/v1.0.0
