# Exporter recipes and Grafana dashboard

Ready-to-use configurations that turn `pg_stat_statement_context` into Prometheus / OpenTelemetry metrics, plus a Grafana dashboard. All three collectors export **the same metric names and labels**, so the dashboard works with any of them.

| Recipe | Tool (tested version) | Notes |
|---|---|---|
| [monitoring-role.sql](monitoring-role.sql) | PostgreSQL 14–18 | A `pssc_monitor` login role with `pg_monitor`. |
| [postgres_exporter/queries.yaml](postgres_exporter/queries.yaml) | `quay.io/prometheuscommunity/postgres-exporter:v0.20.1` | `--extend.query-path` is deprecated upstream but still works. |
| [sql_exporter/](sql_exporter/) | `burningalchemist/sql_exporter:0.24.9` | Upstream's recommended tool for arbitrary SQL. |
| [otel-collector/config.yaml](otel-collector/config.yaml) | `otel/opentelemetry-collector-contrib:0.162.0` | `sql_query` receiver (alpha), Prometheus + debug exporters. |
| [grafana/pg_stat_statement_context.json](grafana/pg_stat_statement_context.json) | `grafana/grafana:13.2.3`, `prom/prometheus:v3.15.0` | Uses a Prometheus datasource. |

[`scripts/test-integrations.sh`](../../scripts/test-integrations.sh) tests every recipe end to end in Docker (see [Testing](#testing)).

## Metrics

| Metric | Type | Labels |
|---|---|---|
| `pssc_tag_calls_per_second` | gauge | `datname`, `toplevel`, `tag_<key>`… |
| `pssc_tag_exec_seconds_per_second` | gauge | as above |
| `pssc_query_calls_per_second` | gauge | as above, plus `queryid` (top 5 per tag set) |
| `pssc_query_exec_seconds_per_second` | gauge | as above, plus `queryid` |
| `pssc_info_entries`, `_max_entries`, `_live_entries` | gauge | |
| `pssc_info_buckets`, `_bucket_interval_seconds`, `_oldest_bucket_age_seconds`, `_last_closed_bucket_timestamp_seconds` | gauge | |
| `pssc_info_shmem_bytes`, `_stats_reset_timestamp_seconds` | gauge | |
| `pssc_info_{dealloc,reclaimed_entries,evicted_entries,dropped_records,invalid_tags,dropped_tags,heuristic_scans,regex_compile_failures,utility_missing_queryid,capped_tags,cap_table_full,exemplar_values_dropped}_total` | counter | |

Each exporter adds its own labels: postgres_exporter adds `server`, and the OTel Prometheus exporter adds `otel_scope_*`. Example scrape output:

```
pssc_tag_calls_per_second{datname="shop",tag_action="show",tag_controller="users",tag_job="",toplevel="true"} 9.5
pssc_tag_exec_seconds_per_second{datname="shop",tag_action="show",tag_controller="users",tag_job="",toplevel="true"} 0.0021
pssc_info_invalid_tags_total 12
```

`exec_seconds_per_second` is execution time per wall-clock second, which is the average number of statements executing at once. Divide it by `calls_per_second` to get the mean latency.

## Design considerations

### Gauges, not counters: why the recipes don't use `rate()`

The extension stores calls and time **in time buckets**. Buckets expire after `bucket_interval × bucket_count`, and an entry can be evicted at any time:

* `pg_stat_statement_context_totals` is a **sliding-window** sum over the live buckets. It drops whenever the oldest bucket expires, so `rate()` on it is meaningless, and Prometheus would read every drop as a counter reset.
* Per-bucket rows (`pg_stat_statement_context`) are complete only once their bucket has closed, and then they expire.

The recipes therefore export **the last closed bucket**, divided by `bucket_interval`, as a gauge. The store tells them which bucket that is: the [`pg_stat_statement_context_last_bucket`](../sql-interface.md#pg_stat_statement_context_last_bucket) view returns just that bucket, and [`pg_stat_statement_context_counters()`](../sql-interface.md#pg_stat_statement_context_counters) reports `bucket_seconds` and `last_closed_bucket_start` (exported as `pssc_info_last_closed_bucket_timestamp_seconds`):

```sql
SELECT tags ->> 'controller', sum(calls)::float8 / min(i.bucket_seconds)
  FROM pg_stat_statement_context_last_bucket,
       pg_stat_statement_context_counters() i
 GROUP BY 1;
```

The last closed bucket is the one before the store's current bucket, a monotonic watermark that follows the clock forwards but never moves back (calls are written into it). Consequences:

* **Plot the gauges as they are.** Never use `rate()`/`increase()` on them. To aggregate, use `sum by (...)`, which is exact because the values are rates over the same interval.
* **Resolution is `bucket_interval`** (5 min by default). The value steps once per bucket, so scraping faster than that adds freshness but no detail. Use a shorter `bucket_interval` (and a larger `bucket_count`) for finer graphs.
* A tag set that is still live but had no calls in the closed bucket exports `0` (the recipes add a zero row per live entry from `_totals`), so its series stays continuous for the history window. After the history window it disappears (the series goes stale).
* Calls are counted in the bucket in which they **finish**. A 10-minute statement adds all of its time to one bucket.
* **Clock steps backwards.** After the system clock steps back, the store keeps writing into its (now future) current bucket, and no bucket closes until the clock catches up. The recipes keep exporting the same last closed bucket meanwhile, never an older one, so the gauges hold their value. The first bucket that closes afterwards holds all the work done during the gap, divided by one `bucket_interval`, so it shows as a spike. `scripts/test-integrations.sh` simulates a 40 s step back and checks this behaviour.
* **Startup.** Bucket boundaries fall on wall-clock multiples of `bucket_interval`, so the bucket in progress at server start began before the server did. Until the first boundary after startup, the last closed bucket predates any data and the gauges read `0`. At that boundary, which can be well under one `bucket_interval` after startup, the startup bucket becomes the last closed one. It holds only the work done since startup but is divided by a whole `bucket_interval`, so the first exported value can read low.
* A forward clock step skips buckets. The skipped buckets are empty, so the gauges read `0` for them.
* **Eviction** of live entries removes their buckets, so the gauges **undercount** but never produce a false counter reset. Watch `rate(pssc_info_evicted_entries_total[...])`: it counts only live entries, and if it is not 0, raise `max_entries` or reduce tag cardinality. `pssc_info_reclaimed_entries_total` counts expired entries recycled when the table is full, which is normal. `pssc_info_dropped_records_total` should always be 0 (see [Eviction](../configuration.md#eviction)).
* **`pg_stat_statement_context_reset()`** (or a restart that does not load [saved statistics](../configuration.md#save)) clears the buckets. The gauges read `0` until the next bucket closes, and the `pssc_info_*_total` counters restart from 0, which `rate()` handles as an ordinary counter reset. `pssc_info_stats_reset_timestamp_seconds` records when this happened.

The `pssc_info_*_total` counters come from `_counters()`. They are real cumulative counters, so `rate()`/`increase()` is correct for them. Watch `pssc_info_capped_tags_total` and `pssc_info_cap_table_full_total` if you configure [cardinality caps](../configuration.md#cardinality_cap), and `pssc_info_exemplar_values_dropped_total` if you record [exemplars](../configuration.md#exemplar_keys).

**Cost per scrape.** `_counters()` reads the counters and settings without scanning the store, so the recipes never call `_info()`, whose `oldest_bucket` walks every entry. Each scrape reads the store four times: twice for the tag metrics (the last bucket and the live tag sets), once for the query metrics, and once for `pssc_info_live_entries` and `pssc_info_oldest_bucket_age_seconds`, which come from one `pg_stat_statement_context(false, true)` scan (`min(bucket_start)` over merged rows is `_info().oldest_bucket`). Drop that last scan if you don't need those two gauges.

**Per-entry counters.** The views also have `calls_total` and `exec_time_total`, which only grow for as long as an entry exists (like `pg_stat_statements`' counters, with `stats_since` as their start time). Exported per entry (one series per `queryid` × tag set), `rate()` works on them and catches every call, at full scrape resolution. The recipes don't export them by default, because:

* their series have the cardinality of entries, not of tag sets;
* an entry that is evicted or reclaimed and comes back starts again from 0, which `rate()` treats as a counter reset; but if you `sum()` them per tag set *before* `rate()`, one entry disappearing looks like a reset of the whole sum and inflates the rate. Always `rate()` per entry first, then `sum by (...)`.

Read them from `_totals` (one row per entry): in the per-bucket view they repeat on each of the entry's bucket rows. To export them, add a query like this (with `calls_total` and `exec_time_total` as counters, and `stats_since` as their start time in the OTel recipe):

```sql
-- The labels must identify the entry: userid, and every tag key you allow.
SELECT coalesce(d.datname, c.dbid::text) AS datname, c.userid::text AS userid,
       c.toplevel::text AS toplevel, c.queryid::text AS queryid,
       coalesce(c.tags ->> 'controller', '') AS tag_controller,
       c.calls_total::float8, c.exec_time_total / 1000 AS exec_seconds_total
  FROM pg_stat_statement_context_totals c
  LEFT JOIN pg_database d ON d.oid = c.dbid
 WHERE c.tags IS NOT NULL;
```

### `toplevel`: avoid double counting

With `pg_stat_statement_context.track = all` (and `pg_stat_statements.track = all`), statements run from functions are recorded with `toplevel = false`, **and** their time is already included in the top-level statement that ran them. Summing both values counts that time twice.

* The dashboard's `toplevel` variable **defaults to `true`**: per-context totals then equal the time the application spent waiting.
* A separate "Nested statements" row shows `toplevel="false"`, which tells you which function bodies are expensive.
* Both values are always exported. Filter in queries, not in the exporter.

### Cardinality: tags → labels

Every distinct label combination is a separate time series.

* Export **specific tag keys** as `tag_<key>` labels with `tags ->> 'key'`. Never export the whole `tags` jsonb as one label: the key order and the set of keys would vary, and any new key would multiply the number of series.
* Only export keys with bounded values (`controller`, `action`, `route`, `job`, …). Leave out request ids, user ids and trace ids. Better still, keep them out of the extension's allowlist (`pg_stat_statement_context.tags`); see [docs/extractors.md](../extractors.md).
* Missing keys become `""`, so that every series has the same label set. A value collapsed to JSON `null` by a [cardinality cap](../configuration.md#cardinality_cap) becomes `""` too (`->>` returns SQL `NULL`); to tell them apart, label it explicitly, e.g. `CASE WHEN c.tags -> 'route' = 'null' THEN '(capped)' ELSE coalesce(c.tags ->> 'route', '') END`. Caps are also a way to bound a label's values server-side.
* The `job` tag would collide with Prometheus's `job` target label; the `tag_` prefix avoids that (`tag_job`).
* `queryid` is exported only for the **top 5 query ids per tag set** (by time in the closed bucket, at most 500 rows). Join `queryid` to `pg_stat_statements` in SQL for the query text. Never export the text as a label.
* To change the keys, edit the three `tag_*` columns in the `pssc_tag` and `pssc_query` queries (and the label lists in sql_exporter / OTel `attribute_columns`), then the dashboard's `controller` variable and legends.

### Privileges

Run [monitoring-role.sql](monitoring-role.sql) (`psql -f`) as a superuser, then set a password or configure certificate/peer authentication. The role gets `pg_monitor`, which includes `pg_read_all_stats`. Without `pg_read_all_stats`, other roles' rows show `queryid` and `tags` as `NULL`. Their `bucket_start`, `userid`, `dbid`, `toplevel`, `calls` and `total_exec_time` stay visible (see [Visibility and privacy](../sql-interface.md#visibility-and-privacy)). Those rows can't be attributed to a context, so the recipes skip them (`WHERE tags IS NOT NULL`). Without the privilege, the exported per-tag rates therefore cover only the monitoring role's own statements. `pg_read_all_stats` alone is enough if you don't want the broader `pg_monitor`. The role also gets a `statement_timeout`.

Connect the collector to a database where the extension is installed (`CREATE EXTENSION pg_stat_statement_context`). The views show statistics for **all** databases, labelled by `datname`, so one connection per server is enough. postgres_exporter's `master: true` makes it run the queries only once when `--auto-discover-databases` is on.

## postgres_exporter

```sh
postgres_exporter --extend.query-path=queries.yaml
# DATA_SOURCE_NAME=postgresql://pssc_monitor:...@db:5432/postgres?sslmode=verify-full
```

Upstream marks `--extend.query-path` as **deprecated**: v0.20.1 logs a warning but still runs the queries. Upstream recommends [sql_exporter](https://github.com/burningalchemist/sql_exporter) for custom SQL, which is why the same metrics are also published for sql_exporter. If a future postgres_exporter release removes the flag, switch to sql_exporter. Metric names are `<namespace>_<column>` (for example `pssc_tag` + `calls_per_second`).

## sql_exporter

Copy both files into one directory and run:

```sh
sql_exporter -config.file=sql_exporter.yml
# or override the target DSN: SQLEXPORTER_TARGET_DSN=postgres://pssc_monitor:...@db:5432/postgres
```

`min_interval: 0s` makes every scrape run the queries. To share one result between several Prometheus servers, set `min_interval` to about `bucket_interval / 2`.

## OpenTelemetry Collector

The core `postgresql` receiver collects a fixed set of server metrics; it **cannot run custom queries** and so cannot read this extension. Use the **`sql_query` receiver** from the contrib distribution (`otelcol-contrib`). It was formerly named `sqlquery`, which is now a deprecated alias, and its stability is alpha.

```sh
PSSC_PG_HOST=db PSSC_PG_PORT=5432 PSSC_PG_DATABASE=postgres \
PSSC_PG_USER=pssc_monitor PSSC_PG_PASSWORD=... \
otelcol-contrib --config=config.yaml
```

* The rate metrics are OTel **gauges**. The `_counters()` counters are **cumulative monotonic sums** whose start time (`start_ts_column`) is `_counters().stats_reset` in Unix nanoseconds, so OTLP backends see a reset as a counter restart.
* The Prometheus exporter (`:8889`) appends `_total` to monotonic sums, so the SQL column and metric names omit it. `metric_expiration: 90s` drops series that are no longer returned.
* To send metrics elsewhere, replace the `prometheus`/`debug` exporters with `otlp`/`otlphttp`. The dashboard needs Prometheus-compatible storage with the names above (for example via a Prometheus OTLP receiver or remote-write).

## Grafana dashboard

Import [grafana/pg_stat_statement_context.json](grafana/pg_stat_statement_context.json) (Dashboards → New → Import) or provision it from a file. It needs a Prometheus datasource, which you choose with the `datasource` variable.

* **Variables:** `job` (the scrape job of your exporter), `instance`, `datname`, `controller`, and `toplevel` (default `true`).
* **Panels:**
  * Calls/s, execution time/s and mean latency by tag set.
  * A table of the top tag sets: total execution time and calls over the dashboard's time range.
  * The top 10 query ids over time, and a table of the top query ids with totals over the time range.
  * Nested statements (`toplevel="false"`).
  * Health: entries and live entries against `max_entries`, bucket settings, the age of the oldest bucket, and the last reset.
  * Eviction rate, and the rates of tag problems (invalid, dropped, regex failures, missing utility query ids, heuristic scans, capped tags, a full cap table, dropped exemplar values).
* The panels use the gauges as they are (`sum by`), and `rate()` only on `pssc_info_*_total`.
* **Totals over the time range.** The tables integrate the gauges: `sum_over_time(x[$__range])` multiplied by the scrape interval. The scrape interval is derived as `$__range_s` divided by the number of `pssc_info_max_entries` samples, since every scrape returns that metric. Scrapes where a series is absent therefore count as 0. That matters because the series are sparse: a tag set disappears once its buckets expire, and a query id is only exported while it is in its tag set's top 5. `avg_over_time()` would average only the samples that are present, which inflates sparse series and can reverse rankings. Query id totals are lower bounds for query ids that were not always in the top 5. The totals assume the exporter was scraped throughout the range.

## Testing

```sh
scripts/test-integrations.sh            # PostgreSQL 17
scripts/test-integrations.sh 14         # another major version
scripts/test-integrations.sh --remove-images 18   # also delete the pulled third-party images
```

The script runs everything in Docker on a private network, with ports bound to 127.0.0.1 only:

* It builds the extension into the `scripts/docker-test.sh` image, with `bucket_interval = 10s`, and starts a tagged workload that includes nested plpgsql statements and a malformed tag. psql runs the workload with `ON_ERROR_STOP`, and the checks fail at once if the workload stops.
* It runs all three collectors with these recipes, plus Prometheus and Grafana with the dashboard provisioned.
* `test/integrations/check.py` then checks:
  * The expected series and values on each `/metrics`. The call rates must match the workload's ratios within 4%: users#show : posts#index : orders#create : job=cleanup : the malformed tag = 5 : 2 : 1 : 1 : 1, and nested calls = 3 × orders#create. A missing or extra statement in the workload can't pass. The `_counters()` counters and settings must have the expected values.
  * That Prometheus has scraped every job.
  * That every dashboard panel query returns data for each exporter job, through Grafana's `/api/ds/query`.
  * A backward clock step. The TEST-ONLY `pssc_store_test` module's debug clock runs the store 40 s ahead for a moment, then returns it to the real clock. postgres_exporter and sql_exporter must then stop exporting the per-second series, then recover once the clock catches up. These two run the queries on every scrape. The OTel collector keeps its last values until `metric_expiration`, so it isn't checked for the gap, but it runs the same SQL.

The script exits non-zero on failure and leaves the container logs in `tmp/integrations-<major>/`. It removes all of its containers and networks on exit. Set `PSSC_INTEGRATION_TIMEOUT` (seconds, default 240) to change how long the checks wait for data.
