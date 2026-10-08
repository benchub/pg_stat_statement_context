# Limitations

## Prepared statements carry the comment from PREPARE time

Comments are read from the statement text saved when the statement was **prepared** (protocol-level Parse, or SQL `PREPARE`). Bind/Execute messages and SQL `EXECUTE` don't send new statement text, so a comment on an `EXECUTE` is ignored, and every execution of a prepared statement is attributed to the tags in its original text. If an application prepares a statement once and executes it from different code paths, every execution gets the first caller's tags. This applies to named and unnamed statements alike (an unnamed statement gets fresh context only if the driver sends a new Parse each time). `pg_stat_statements` has the same limitation for query text.

```sql
PREPARE q AS SELECT 1 /*controller:users*/;
EXECUTE q /*controller:admin*/;   -- recorded with {"controller": "users"}
DEALLOCATE q;
```

**In practice, common drivers are not affected.** We tested (Oct 2026) pgx 5.11, pgjdbc 42.7.13, psycopg 3.3.6, ActiveRecord 7.0–8.1 with the pg gem 1.7 and marginalia 1.11, and pgbouncer 1.26 in transaction mode, in their default and most aggressive prepare settings. None reuses a prepared statement across different comments: they key their statement caches on the full SQL text, comment included, or re-Parse on every call, and pgbouncer shares server-side statements by query text. Stale tags appear only when the **application** holds on to one prepared handle (for example a pgx `conn.Prepare` name or a long-lived JDBC `PreparedStatement`) and reuses it across requests. Details are in [research/driver-prepared-statements](../research/driver-prepared-statements/README.md).

Related findings:

- **Comment cardinality costs prepared statements.** Because the comment is part of the drivers' cache key, every distinct comment value creates its own server-side prepared statement per query shape. High-cardinality values in comments (request IDs, `traceparent`) defeat statement caching and churn the caches (pgx's LRU, pgjdbc `preparedStatementCacheQueries`, psycopg `prepared_max`, Rails `statement_limit`, pgbouncer `max_prepared_statements`). Keep comment tags low-cardinality (controller, action, route, job).
- **Rails 7.1+ `query_log_tags` disables prepared statements** (`ActiveRecord.disable_prepared_statements = true`), which changes the protocol and plan-caching behavior. That is Rails' own trade-off.
- **marginalia on Rails 8.0+ doesn't annotate ordinary model queries** (`where`/`pick`/`find_by` carry no comment; they are simply untagged). Use Rails' built-in `query_log_tags` instead.

For application-held prepared statements, and for drivers that can't add comments, set the tags with [`tags_override`](configuration.md#tags_override) instead (for example `SET LOCAL` at the start of each request's transaction): it is read when each statement executes, not when it was prepared.

## Scanner caveats

- **`standard_conforming_strings`.** The scanner uses the *current* value of `standard_conforming_strings`, because the setting in effect when the statement was parsed isn't available to the hooks. If it is changed between `PREPARE` and `EXECUTE`, plain string literals containing backslashes may be mis-scanned. Leave it at its default (`on`).
- **Heuristic scans.** For a statement longer than `scan_window`, an `append` extractor only looks at the last `scan_window` bytes, without knowing the lexical state at the start of that window. A string literal that ends in `*/`, or a `--` line comment that started before the window, can then produce tags from text that isn't really a comment. Such scans are counted in `_info().heuristic_scans`. Use `position=prepend` (exact) or a larger `scan_window` if this matters. See [Where comments are found](extractors.md#where-comments-are-found).
- **Comments in PL/pgSQL (`nested_tags = scan`).** A nested statement is scanned with the usual position rules, using the text PL/pgSQL passes on:
  - In a SQL statement (`SELECT`, `INSERT`, `UPDATE`, `DELETE`), a trailing comment is kept. `SELECT ... INTO n FROM t /*controller:x*/` works, because PL/pgSQL blanks out only the `INTO n` clause.
  - In `PERFORM` and in expressions (`RETURN (SELECT ...)`, `n := (SELECT ...)`, `IF` conditions), PL/pgSQL drops a comment that ends the expression. A comment inside the expression is mid-statement, so only an extractor with `position=any` sees it.
- Some statements are never recorded because they have no query ID: the query inside `DECLARE CURSOR` (its execution is recorded under the `DECLARE` statement and its tags), and PL/pgSQL simple expressions (`x := 1 + y`), which bypass the executor.

## Utility statement query IDs on PostgreSQL 14 and 15

On PostgreSQL 14 and 15, core computes the query ID of a utility statement (DDL, `VACUUM`, ...) by hashing its **text**, comments included. Every distinct tag set therefore gives a different `queryid` for the same DDL, and utility fingerprints fragment. PostgreSQL 16 and later compute it from the parse tree, so comments have no effect. The extension always reports the core `queryid`, which `pg_stat_statements` shows too, so joins still work.

## `toplevel` on PostgreSQL 14–16

`toplevel` matches `pg_stat_statements` on every version (see [Nested statements](sql-interface.md#nested-statements-toplevel-and-inclusive-costs)). On 14–16 this relies on reading pgss's `track` and `track_utility` settings: a utility statement only makes its children nested when pgss tracks it. As in pgss on those versions, statements run while a parent is being planned are top-level. There is no divergence to work around, but be aware that changing `pg_stat_statements.track_utility` on 14–16 changes which statements this extension reports as top-level.

## Failed statements are not counted

A statement that raises an error (or is cancelled) never reaches `ExecutorEnd`, so it isn't counted, the same as in `pg_stat_statements`. Error and cancellation counts per tag set are out of scope.

## Only `calls` and `total_exec_time`

Rows, buffers, WAL, I/O timing, JIT, min/max/mean/stddev and planning time are not stored; get them from `pg_stat_statements` (see [Joining](sql-interface.md#joining-to-pg_stat_statements)). Apportioning them to contexts by execution time is an approximation.

## Statistics survive only clean restarts

The statistics live in shared memory. With [`save`](configuration.md#save) on (the default) they are saved at a clean shutdown and loaded at the next start, but they are lost after a crash or an immediate shutdown, and when `bucket_interval`, `bucket_count` or the extension version changes. Exemplars are never saved. The history covers only the last `bucket_count × bucket_interval`.

## Replicas and failover

Each server keeps its own statistics in its own shared memory, and nothing about them is written to WAL. A streaming (hot) standby records the read-only statements that run on it in its own store. The primary's statistics never appear on a standby, and a standby's never appear on the primary, so to see a whole cluster you query every instance (for example one exporter per instance, see [integrations](integrations/README.md)). `pg_stat_statement_context_reset()` and `pg_stat_statement_context_info()` act on the instance you are connected to.

Set the library in `shared_preload_libraries`, and the GUCs, on every instance. A base backup copies `postgresql.conf` and `postgresql.auto.conf` as they were at backup time, but later changes to them are not replicated. Per-database and per-role settings (`ALTER DATABASE ... SET`, `ALTER ROLE ... SET`) are stored in the catalogs, so they do reach the standbys. `CREATE EXTENSION` is run on the primary only, and its objects reach the standbys through WAL; on an instance without the library preloaded, the functions and views fail with "must be loaded via shared_preload_libraries".

[`save`](configuration.md#save) works on a standby as on a primary: a clean (smart or fast) standby restart saves its statistics and loads them again, and an immediate shutdown or a crash discards them.

On failover, a promoted standby keeps the statistics it had in memory, which cover only the statements that ran on it while it was a standby, and keeps recording as the new primary. The old primary's statistics stay on the old primary: they are not carried over to the new one, and they are lost if that instance is rebuilt or does not shut down cleanly. Clients and dashboards that follow the primary will therefore see the counters start from the standby's own history after a failover.

## Visibility and PII

Tag values come from clients and can contain personal data. Other roles' `tags` and `queryid` are hidden unless the caller has the privileges of `pg_read_all_stats`, but anyone with those privileges sees every tag value. Don't put personal data in comments, and keep it out of the `tags` allowlist. See [Visibility and privacy](sql-interface.md#visibility-and-privacy).

## Deployment

The library must be in `shared_preload_libraries` (after `pg_stat_statements`), so enabling it requires a server restart. Managed PostgreSQL providers (Amazon RDS, Google Cloud SQL, Azure, ...) only allow extensions on their allowlists; until a provider adds this one, it can't be used there.

## Not in v1

The [cardinality caps](configuration.md#cardinality_cap) don't decay: a value admitted under a key's cap keeps its place until `pg_stat_statement_context_reset()` or a restart, even after its entries are evicted.
