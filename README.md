# pg_stat_statement_context

`pg_stat_statement_context` is a PostgreSQL extension that attributes query
execution statistics to the **application context** carried in SQL comments,
such as the comments emitted by [marginalia], Rails `query_log_tags`, and
[SQLCommenter].

`pg_stat_statements` answers *"which query fingerprints are expensive?"*. This
extension answers *"which parts of my application run query fingerprint X, how
often, and at what cost?"*:

```
 queryid  | controller  | action |    route     | calls | total_exec_time
----------+-------------+--------+--------------+-------+-----------------
 -8812... | users       | show   | /users/:id   | 41022 |         1833.20
 -8812... | admin/users | index  | /admin/users |   210 |          912.75
```

It supports PostgreSQL 14, 15, 16, 17 and 18 from a single source tree.

## A companion to pg_stat_statements

The extension is a **companion** to `pg_stat_statements` (pgss), not a
replacement. For each combination of (database, user, query fingerprint,
`toplevel`, tag set) it stores only two counters per time bucket:

- `calls`: the number of completed executions;
- `total_exec_time`: their execution time in milliseconds, measured the same
  way pgss measures it.

Everything else (query text, rows, buffers, WAL, I/O timing, JIT,
min/max/mean/stddev, planning time) comes from `pg_stat_statements`. You join
the two on `(userid, dbid, queryid, toplevel)`. The `queryid` is always the
core query ID, identical to pgss's. See
[Joining to pg_stat_statements](docs/sql-interface.md#joining-to-pg_stat_statements).

How it works, in short: PostgreSQL parses each statement only once. In the
executor and utility hooks, the extension lexically scans the original
statement text for comments (only near the start or end of long statements),
extracts `key/value` tags from them, and adds the call to a fixed-size
shared-memory table that keeps a rolling, time-bucketed history (by default
12 buckets of 5 minutes). It does not store query text and doesn't parse SQL a
second time.

## Requirements

- PostgreSQL 14–18, with the server development files (`pg_config`, PGXS) to
  build.
- The library must be listed in `shared_preload_libraries`, which needs a
  server restart. Managed services (RDS, Cloud SQL, Azure, ...) only allow
  extensions on their allowlists, so the extension can't be installed there
  until the provider adds it.
- `pg_stat_statements` is optional but strongly recommended: it provides the
  query text and every other metric.

## Installation

Build and install with PGXS:

```sh
make
make install                   # may need sudo
# for a specific server: make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

Add the library to `shared_preload_libraries` in `postgresql.conf`. If you use
`pg_stat_statements` too, it **must come first**:

```ini
shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'
```

Then restart the server and create the extension (plus `pg_stat_statements`,
if you use it) in each database where you want to query the statistics:

```sql
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
CREATE EXTENSION pg_stat_statement_context;
```

Statistics are collected for **all** databases of the cluster as soon as the
library is preloaded; `CREATE EXTENSION` only installs the views and
functions. Without preloading, the extension's GUCs don't exist and its SQL
functions fail with
`pg_stat_statement_context must be loaded via "shared_preload_libraries"`.

### Load order

pgss clears the query ID of utility statements (DDL, `VACUUM`, ...) before it
calls the next `ProcessUtility` hook. The library listed last installs the
outermost hook, so `pg_stat_statement_context` must be listed **after**
`pg_stat_statements`. If the order is wrong, the server logs at startup:

```
WARNING:  pg_stat_statements is loaded after pg_stat_statement_context in shared_preload_libraries
DETAIL:  In this order the ProcessUtility hook of pg_stat_statements runs first and clears the query identifier of utility statements, so they are not recorded; they are counted in utility_missing_queryid instead.
HINT:  Set shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context' (pg_stat_statements first) and restart the server.
```

The warning is the only effect. Plannable statements (`SELECT`, DML) are
recorded either way, but utility statements are then not recorded; they are
counted in `pg_stat_statement_context_info().utility_missing_queryid`.

### Query IDs

The extension calls `EnableQueryId()` at startup, so the default
`compute_query_id = auto` behaves like `on`, just as with pgss. Don't set
`compute_query_id = off`: statements without a query ID are not recorded.

## Quick start

With the default configuration, the extension reads SQLCommenter and
marginalia comments appended to the statement, keeps the tags `action`,
`controller`, and `job`, and skips statements that carry none of them.

```sql
SELECT 1 /*controller:users,action:show*/;
SELECT 1 /*controller='users',action='show'*/;
SELECT 2 /*controller:admin,action:index,request_id:42*/;

SELECT queryid, toplevel, tags, calls, total_exec_time
  FROM pg_stat_statement_context_totals
 ORDER BY calls DESC;
```

The first two statements use different comment formats but produce the same
tag set, so they land in the same row with `calls = 2`. The `request_id` tag
isn't in the allowlist and is ignored. Note that `SELECT 1` and `SELECT 2` are
the same query fingerprint (`SELECT $1`). Your query IDs and timings will differ:

```
       queryid       | toplevel |                    tags                    | calls | total_exec_time
---------------------+----------+--------------------------------------------+-------+-----------------
 2800308901962295548 | t        | {"action": "show", "controller": "users"}  |     2 |        0.006542
 2800308901962295548 | t        | {"action": "index", "controller": "admin"} |     1 |        0.003375
```

To see which tags the current configuration would extract from a statement,
without running it, a superuser can call the debug function:

```sql
SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show,application:app*/') -> 'tags';
```

## Documentation

| Document | Contents |
|---|---|
| [docs/configuration.md](docs/configuration.md) | Every GUC, changing the configuration from SQL, capacity sizing, time buckets, eviction. |
| [docs/extractors.md](docs/extractors.md) | Where comments are found, the extractor DSL, SQLCommenter / marginalia / regex formats, the tag pipeline, allowlist/denylist and cardinality guidance. |
| [docs/sql-interface.md](docs/sql-interface.md) | The views, `pg_stat_statement_context()`, the `_activity` view (current tags per backend), `_info()`, `_reset()`, `_extract()`, the join to `pg_stat_statements`, nested statements and `toplevel`, visibility, encodings. |
| [docs/limitations.md](docs/limitations.md) | Prepared statements, scanner caveats, PG14/15 utility query IDs, failed statements, PII, managed providers. |
| [docs/integrations/](docs/integrations/README.md) | Recipes for postgres_exporter, sql_exporter and the OpenTelemetry Collector, a Grafana dashboard, a monitoring role, and the metric semantics (gauges, `toplevel`, cardinality). |
| [docs/benchmarks.md](docs/benchmarks.md) | pgbench overhead and latency measurements against pg_stat_statements alone: the method, results for PG 18 and PG 14, findings (bucket boundaries, IN lists, eviction), and how to re-run them. |

## Testing

```sh
make installcheck                # needs a running server with the library preloaded
scripts/docker-test.sh 17        # build and run the regression and TAP suites in Docker
scripts/docker-test.sh 15.0      # same, against an exact release built from source
scripts/docker-test.sh --assert 17    # source build with --enable-cassert
scripts/docker-test.sh --valgrind 18  # regression suite with the server under Valgrind
make unittest                    # standalone scanner/parser unit tests and fuzz corpus
fuzz/run-libfuzzer.sh -t 600     # libFuzzer (clang, ASan+UBSan) targets in Docker, 10 min each
fuzz/sql/run.sh -- --duration 600     # regex extractor SQL fuzzer, assert build in Docker
scripts/test-integrations.sh 17  # exporter recipes + Grafana dashboard, end to end in Docker
bench/run.sh --major 18 [--quick]    # pgbench overhead benchmarks in Docker (docs/benchmarks.md)
```

[fuzz/README.md](fuzz/README.md) describes the fuzz targets, their invariants
and how to replay a failure.

CI (`.github/workflows/ci.yml`) runs the same `scripts/docker-test.sh` commands
for its Linux cells (PG 14–18 from PGDG, assert builds of 14–18, Valgrind
on 18), runs `docker/run-tests.sh` directly on macOS against PostgreSQL
built by `docker/build-postgres.sh`, and runs a short fuzz smoke job. The TAP tests use the PG 15+ module names
(`PostgreSQL::Test::Cluster`/`Utils`). PG 14 ships them as aliases of
`PostgresNode`/`TestLib` from 14.3 but only installs them from 14.6, so
14.0–14.5 can't run the TAP suite.

`DESIGN.md` describes the design and its rationale in detail.

## License

See [LICENSE](LICENSE).

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
