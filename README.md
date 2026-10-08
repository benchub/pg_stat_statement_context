# pg_stat_statement_context

`pg_stat_statement_context` is a PostgreSQL extension that attributes query execution statistics to the **application context** carried in SQL comments, such as the comments emitted by [marginalia], Rails `query_log_tags`, and [SQLCommenter]. Clients without these libraries to decorate their queries with comments can supply the context to `pg_stat_statement_context` via `application_name` or `SET` commands.

The familiar `pg_stat_statements` answers *"which query fingerprints are expensive?"*.

**This** extension answers *"which parts of my application run query fingerprint X, how often, and at what cost?"*.

For example:

```
 queryid  | controller  | action |    route     | calls | total_exec_time
----------+-------------+--------+--------------+-------+-----------------
 -8812... | users       | show   | /users/:id   | 41022 |         1833.20
 -8812... | admin/users | index  | /admin/users |   210 |          912.75
```

It supports PostgreSQL 14–18. A newer major fails the build with a clear error until it is validated; `make PSSC_ALLOW_UNTESTED_PG=1` tries it anyway.

## A companion to pg_stat_statements

The extension is most useful as a **companion** to `pg_stat_statements`, not a replacement. For each combination of (database, user, query fingerprint, `toplevel`, tag set) it stores only two counters per time bucket:

- `calls`: the number of completed executions
- `total_exec_time`: execution time in milliseconds

Every other performance indicator (query text, rows, buffers, WAL, I/O timing, JIT, min/max/mean/stddev, planning time) is best found in `pg_stat_statements`. You can join the two on `(userid, dbid, queryid, toplevel)`. See [Joining to pg_stat_statements](docs/sql-interface.md#joining-to-pg_stat_statements).

How it works, in short: In the executor and utility hooks, the `pg_stat_statement_context` lexically scans the already-parsed statement text for comments (only near the start or end, for long statements), extracts `key/value` tags from them, and adds the call to a fixed-size shared-memory table that keeps a rolling, time-bucketed history (by default 12 buckets of 5 minutes). It does not store query text.

`pg_stat_statement_context` was written to be used in conjunction with `pg_stat_statements` to power [rotten]. Both are requirements for that project, but `pg_stat_statement_context` has the potential to be useful on its own. And so, here it is.

## Requirements

- PostgreSQL 14–18, with the server development files (`pg_config`, PGXS) to build.
- `pg_stat_statement_context` must be listed in the `shared_preload_libraries` GUC, which needs a server restart. On a replicated cluster, set it (and the GUCs) on every instance: each instance keeps its own statistics, which are not replicated (see [Replicas and failover](docs/limitations.md#replicas-and-failover)).
- `pg_stat_statements` is technically optional but strongly recommended: it provides the query text and every other metric.

## Installation

### Compile
Build and install with PGXS:

```sh
make

make install # may need sudo
# for a specific server: make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

This builds the release library. `make PSSC_TESTING=1` builds a testing library instead. It adds test-only hooks and exports the internal API that the TEST-ONLY modules in `test/modules/` need, so don't install it on a production server. See [DESIGN.md §9](DESIGN.md#9-testing-strategy).

### Binaries
To build Ubuntu 24.04 (noble) `.deb` packages for PostgreSQL 14–18 from PGDG, on amd64 and arm64, run `scripts/build-debs.sh`. It needs Docker, builds the committed tree, and writes the packages to `binaries/`. Each package is named `postgresql-<major>-pg-stat-statement-context`.

### Configure PostgreSQL
Add the library to `shared_preload_libraries` in `postgresql.conf`. If you use `pg_stat_statements` too, it **must come first**:

```ini
shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'
```

Then restart the server and create the extension (plus `pg_stat_statements`, if you use it) in each database where you want to query the statistics:

```sql
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
CREATE EXTENSION pg_stat_statement_context;
```

Statistics are collected for **all** databases of the cluster as soon as the library is preloaded; `CREATE EXTENSION` only installs the views and functions.

### Load order

`pg_stat_statements` clears the query ID of utility statements (DDL, `VACUUM`, ...) before it calls the next `ProcessUtility` hook, and so does `pg_stat_monitor`. The library listed last installs the outermost hook, so `pg_stat_statement_context` must be listed **after** both of them: `'pg_stat_statements, pg_stat_monitor, pg_stat_statement_context'` (leave out the ones you don't use). If the order is wrong, the server logs at startup, once for each library listed after `pg_stat_statement_context`:

```
WARNING:  pg_stat_statements is loaded after pg_stat_statement_context in shared_preload_libraries
DETAIL:  In this order the ProcessUtility hook of pg_stat_statements runs first and clears the query identifier of utility statements, so they are not recorded; they are counted in utility_missing_queryid instead.
HINT:  Set shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context' and restart the server.
```

With `pg_stat_monitor` listed after `pg_stat_statement_context`, the warning names `pg_stat_monitor` instead, and the hint gives the order of all the listed libraries, for example `'pg_stat_statements, pg_stat_monitor, pg_stat_statement_context'`.

If you see that warning, the symptoms will be that plannable statements (`SELECT`, DML) are recorded, but utility statements (`VACUUM`, DDL) will be invisible to `pg_stat_statement_context`, even if a context exists. Instead, they get counted in `pg_stat_statement_context_info().utility_missing_queryid`.

### Query IDs

`pg_stat_statement_context` uses QueryIDs, so if you have configured PostgreSQL with `compute_query_id = auto`, this does count as an extension that wants IDs and so your queries will get IDs if they aren't already. 

Obviously, if you have `compute_query_id = off` instead, `pg_stat_statement_context` will do nothing for you. Don't do that.

## Quick start

With the default configuration, the extension:
* reads SQLCommenter and marginalia comments **appended** to the statement
* keeps the tags `action`, `controller`, and `job`
* ignores statements that carry none of those tags

```sql
SELECT 1 /*controller:users,action:show*/;
SELECT 1 /*controller='users',action='show'*/;
SELECT 2 /*controller:admin,action:index,request_id:42*/;

SELECT queryid, toplevel, tags, calls, total_exec_time
  FROM pg_stat_statement_context_totals
 ORDER BY calls DESC;
```

The first two statements use different comment formats but produce the same tag set, so they land in the same row with `calls = 2`. For the third query, the `request_id` tag isn't in the default allowlist and is therefore ignored. Note that `SELECT 1` and `SELECT 2` are the same query fingerprint (`SELECT $1`). Your query IDs and timings will differ, but this should be the shape of what you see:

```
       queryid       | toplevel |                    tags                    | calls | total_exec_time
---------------------+----------+--------------------------------------------+-------+-----------------
 2800308901962295548 | t        | {"action": "show", "controller": "users"}  |     2 |        0.006542
 2800308901962295548 | t        | {"action": "index", "controller": "admin"} |     1 |        0.003375
```

To see which tags the current configuration would extract from a statement, without running it, a superuser or the role that created the extension can call the debug function (others need `GRANT EXECUTE`):

```sql
SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show,application:app*/') -> 'tags';
```

## Documentation

| Document | Contents |
|---|---|
| [docs/configuration.md](docs/configuration.md) | Every GUC (including the session/transaction `tags_override`), changing the configuration from SQL, capacity sizing, time buckets, eviction. |
| [docs/extractors.md](docs/extractors.md) | Where comments are found, the extractor DSL, SQLCommenter / marginalia / regex formats, the tag pipeline, allowlist/denylist and cardinality guidance. |
| [docs/sql-interface.md](docs/sql-interface.md) | The views (including `_last_bucket`, the last closed bucket), `pg_stat_statement_context()`, the `_activity` view (current tags per backend), `_info()`, `_counters()`, `_reset()`, `_extract()`, the join to `pg_stat_statements`, nested statements and `toplevel`, visibility, encodings. |
| [docs/limitations.md](docs/limitations.md) | Prepared statements, scanner caveats, PG14/15 utility query IDs, failed statements, statistics across restarts, [replicas and failover](docs/limitations.md#replicas-and-failover) (per-instance histories), PII, managed providers. |
| [docs/integrations/](docs/integrations/README.md) | Recipes for postgres_exporter, sql_exporter and the OpenTelemetry Collector, a Grafana dashboard, a monitoring role, and the metric semantics (gauges, `toplevel`, cardinality). |
| [docs/managed-services.md](docs/managed-services.md) | Running without superuser on a managed service: what each GUC's context means with parameter groups, raw parameter-group values for the DSL settings, who can call `_reset()`/`_extract()` and read the views, and a SQL-only troubleshooting checklist. |
| [docs/benchmarks.md](docs/benchmarks.md) | pgbench overhead and latency measurements against pg_stat_statements alone: the method, results for PG 18 and PG 14, findings (bucket boundaries, IN lists, eviction), and how to re-run them. |

## Testing

```sh
make installcheck                     # needs a running server with the library preloaded;
                                      # TAP tests that need the testing build skip on a release one
make PSSC_TESTING=1 install install-test-modules  # testing build + TEST-ONLY modules, for every TAP test
scripts/check-release-exports.sh      # release build exports only test/release-exports.txt
scripts/docker-test.sh 17             # testing build: every test; release build: exports, pg_regress, TAP, in Docker
scripts/docker-test.sh 15.0           # same, against an exact release built from source
scripts/docker-test.sh --assert 17    # source build with --enable-cassert
scripts/docker-test.sh --valgrind 18  # regression suite with the server under Valgrind
make unittest                         # standalone scanner/parser unit tests and fuzz corpus
fuzz/run-libfuzzer.sh -t 600          # libFuzzer (clang, ASan+UBSan) targets in Docker, 10 min each
fuzz/sql/run.sh -- --duration 600     # regex extractor SQL fuzzer, assert build in Docker
scripts/test-integrations.sh 17       # exporter recipes + Grafana dashboard, end to end in Docker
bench/run.sh --major 18 [--quick]     # pgbench overhead benchmarks in Docker (docs/benchmarks.md)
```

[docs/maintaining.md](docs/maintaining.md) lists the pg_stat_statements behaviors this extension mirrors, what to re-check when a new PostgreSQL release ships, the coexistence tests with other hook-using extensions, and the oldest-minor CI policy.

[fuzz/README.md](fuzz/README.md) describes the fuzz targets, their invariants and how to replay a failure.

`DESIGN.md` describes the design and its rationale in detail.

## License

See [LICENSE](LICENSE).

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
[rotten]: https://github.com/benchub/rotten