# SQL interface

`CREATE EXTENSION pg_stat_statement_context` installs these objects in the schema of the extension:

| Object | Kind | Default access |
|---|---|---|
| [`pg_stat_statement_context`](#the-historical-views) | View. One row for each entry and live bucket. | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context()`](#pg_stat_statement_context) | Set-returning function. The `pg_stat_statement_context` and `pg_stat_statement_context_totals` views use it. | `PUBLIC` |
| [`pg_stat_statement_context_activity`](#pg_stat_statement_context_activity) | View. The current tags of each backend. Join it to `pg_stat_activity` on `pid`. The view uses the function `pg_stat_statement_context_activity()`. | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context_counters()`](#pg_stat_statement_context_counters) | The store counters and diagnostic counters of `pg_stat_statement_context_info()`, without the table scan. It costs little, so use it for scrapers. | `PUBLIC` |
| [`pg_stat_statement_context_extract()`](#pg_stat_statement_context_extract) | Debug function. Shows the tags that the extension extracts from a statement. | `EXECUTE` revoked from `PUBLIC` |
| [`pg_stat_statement_context_info()`](#pg_stat_statement_context_info) | Store counters and diagnostic counters. | `PUBLIC` |
| [`pg_stat_statement_context_last_bucket`](#pg_stat_statement_context_last_bucket) | View. One row for each entry, for the last closed bucket only. The view uses the function `pg_stat_statement_context_last_bucket(showtags)`. | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context_reset()`](#pg_stat_statement_context_reset) | Clears all `pg_stat_statement_context` statistics. | `EXECUTE` revoked from `PUBLIC` |
| [`pg_stat_statement_context_totals`](#the-historical-views) | View. One row for each entry, with the live buckets added together. | `SELECT` granted to `PUBLIC` |

These roles can call the restricted functions:

- superusers,
- the owner of the extension (the role that ran `CREATE EXTENSION` in the database), and
- roles that have `EXECUTE` on the function.

The owner or a superuser can use `GRANT EXECUTE` to give access to other roles. For setups without a superuser, see [Managed services](managed-services.md#privileges).

The views and functions work only when the library is in `shared_preload_libraries`. If it is not, they fail with this error: `pg_stat_statement_context must be loaded via "shared_preload_libraries"`.

Statistics are cluster-wide, as with `pg_stat_statements`. The extension collects the statements of all databases, and the views show all of them. To see one database only, filter on `dbid`.

## The historical views

```sql
SELECT * FROM pg_stat_statement_context;             -- per bucket
SELECT * FROM pg_stat_statement_context_totals;      -- per entry, over the whole window
SELECT * FROM pg_stat_statement_context_last_bucket; -- just the last bucket
```

| Column | Type | Description |
|---|---|---|
| `bucket_start` | `timestamptz` | The start of the time bucket. In `pg_stat_statement_context_totals`, the start of the oldest live bucket of the entry. |
| `userid` | `oid` | The role that ran the statement. |
| `dbid` | `oid` | The database in which the statement ran. |
| `queryid` | `bigint` | The query ID. It is the same as `pg_stat_statements.queryid`. |
| `toplevel` | `bool` | `true` if the client sent the statement. `false` if the statement was nested (only with `track = all`). See [Nested statements](#nested-statements-toplevel-and-inclusive-costs). |
| `tags` | `jsonb` | The tag set. It is a JSON object with string values, for example `{"action": "show", "controller": "users"}`. It is `{}` for statements that the extension records with `untagged = record`. A value is JSON `null` (not a string) only when its key reached its [cardinality cap](configuration.md#cardinality_cap). Client input cannot make a `null` value. |
| `calls` | `bigint` | The number of completed executions in the bucket (or in the window, for `pg_stat_statement_context_totals`). |
| `total_exec_time` | `float8` | The total execution time, in milliseconds. |
| `calls_total` | `bigint` | The calls of the **entry** since `stats_since`, in all buckets. This value does not decrease while the entry exists. When buckets expire, this value does not change. |
| `exec_time_total` | `float8` | The execution time of the entry since `stats_since`, in milliseconds. It works like `calls_total`. |
| `stats_since` | `timestamptz` | The time when the extension created the entry. `calls_total` and `exec_time_total` count from this time. |
| `exemplars` | `jsonb` | The most recent value of each key in [`exemplar_keys`](configuration.md#exemplar_keys) for the entry, for example `{"traceparent": "00-…-01"}`. The key does not have to be a grouping tag. It is `{}` if the entry has no stored values. Like `calls_total`, this value is for the entry. It is `NULL` when `tags` is `NULL`: in rows of other roles without `pg_read_all_stats`, and with `showtags = false`. |

An **entry** is one {`userid`, `dbid`, `queryid`, `toplevel`, `tags`}. 

* The `pg_stat_statement_context` view returns one row for each bucket of every entry. The rows of an entry are ordered oldest first.
* The `pg_stat_statement_context_totals` view returns one row for each entry.
* The `pg_stat_statement_context_last_bucket` view returns the most recent bucket of every entry.

### Per-entry counters
`calls_total`, `exec_time_total`, and `stats_since` are values of the entry, not of a bucket. `pg_stat_statements` has the same type of counters, and it has `stats_since` on PostgreSQL 17+.

- These values **repeat on each bucket row** of an entry in `pg_stat_statement_context`. Do not use `sum()` on them in that view. These columns are not duplicated between rows in `pg_stat_statement_context_totals`.
- These values only increase while the entry exists. Thus, you can use them for counter-based monitoring, for example with Prometheus `rate()` (see [integrations](integrations/README.md)).
- These values start again, with a new `stats_since`, when the extension recreates the entry. This occurs after `pg_stat_statement_context_reset()`, or after the extension [reclaims or evicts](configuration.md#eviction) the entry.
- If all the buckets of an entry expire, the views do not show the entry. But the entry keeps its counters until the extension reclaims it. If the statement runs again before that, the counters continue.
- `sum(calls)` counts only the calls in the live buckets of the entry. `calls_total` counts all the calls since `stats_since`, including the calls in buckets that expired. When a bucket with calls expires, `sum(calls)` decreases, but `calls_total` does not change. For example, an entry has 100 calls since `stats_since`, and 10 of these calls are in live buckets. Then `sum(calls)` is 10, and `calls_total` is 100. `sum(total_exec_time)` and `exec_time_total` work in the same way.

### Counter semantics

- `calls` counts completed executions:
  - For a plannable statement, it counts executor runs that reached `ExecutorEnd`.
  - For a utility statement, it counts completed utility calls.
  - A cursor that you fetch from many times is **one** call. A portal that runs in many `Execute` messages is also one call. The extension adds the call to the bucket in which the call ends.
  - The extension does not count statements that fail.
- The extension measures `total_exec_time` in the same way as `pg_stat_statements`.

This example shows the calls and the time of each controller in each bucket:

```sql
SELECT bucket_start, tags->>'controller' AS controller,
       sum(calls) AS calls, round(sum(total_exec_time)::numeric, 2) AS ms
  FROM pg_stat_statement_context
 WHERE toplevel
 GROUP BY 1, 2
 ORDER BY 1, 4 DESC;
```

## `pg_stat_statement_context()`

```
pg_stat_statement_context(showtags boolean DEFAULT true,
                          merge_buckets boolean DEFAULT false)
  RETURNS SETOF (bucket_start, userid, dbid, queryid, toplevel, tags,
                 calls, total_exec_time, calls_total, exec_time_total, stats_since,
                 exemplars)
```

The view `pg_stat_statement_context` wraps `pg_stat_statement_context(true, false)`. The view `pg_stat_statement_context_totals` wraps `pg_stat_statement_context(true, true)`.

- `merge_buckets = true` adds together the `calls` and the `total_exec_time` of the live buckets of each entry. The function then returns one row for each entry. `bucket_start` is the start of the oldest live bucket.
- `showtags = false` returns `NULL` for `tags` in all rows. This costs less if you need only the counters.

## `pg_stat_statement_context_last_bucket`

```sql
SELECT * FROM pg_stat_statement_context_last_bucket;
```

This view has the same columns as [`pg_stat_statement_context()`](`pg_stat_statement_context()`), but it shows only the **last closed bucket**. This is the bucket just before the currently-filling bucket of the store. Its start is `pg_stat_statement_context_info().last_closed_bucket_start`. The extension cannot record more calls in this bucket, so its counts are fixed until the currently-filling bucket closes. Export this bucket as a per-interval gauge (see [integrations](integrations/README.md)).

- The view has one row for each entry that has calls in that bucket. It does not show entries without calls in that bucket.
- `calls` and `total_exec_time` are for that bucket only. `calls_total`, `exec_time_total`, and `stats_since` are for the entry, as in the other views.
- The extension selects the bucket one time for each query. It uses the current bucket of the store, which never moves backward (see [Time buckets](configuration.md#time-buckets)). If the clock moves backward, the last closed bucket stays the newest bucket that ever closed.
- The bucket is empty if no statement ended in it.
- Bucket boundaries are at wall-clock multiples of `bucket_interval`. Thus, the bucket that is current when the server starts began before the server started. Until the first boundary after startup, the last closed bucket is older than all data.
- At the first boundary after startup, the startup bucket becomes the last closed bucket. This can occur much less than one `bucket_interval` after startup. The startup bucket has only the calls since startup, so it is a partial interval.
- With `bucket_count = 1`, the view is always empty, because the extension keeps only the current bucket.
- Visibility is the same as for the other views.
- The view uses the function `pg_stat_statement_context_last_bucket(showtags boolean DEFAULT true)`. `showtags = false` returns `NULL` for `tags`.

## Joining to pg_stat_statements

This extension stores only `calls` and `total_exec_time` for each context. The query text and all other performance metrics come from `pg_stat_statements`. Join the two on `(userid, dbid, queryid, toplevel)`. This example shows the top 20 contexts by execution time, with the query text:

```sql
SELECT c.tags->>'controller' AS controller, c.tags->>'action' AS action,
       sum(c.calls) AS calls, sum(c.total_exec_time) AS ms,
       left(s.query, 80) AS query
FROM pg_stat_statement_context_totals c
LEFT JOIN pg_stat_statements s USING (userid, dbid, queryid, toplevel)
WHERE c.toplevel
GROUP BY 1, 2, s.query
ORDER BY ms DESC LIMIT 20;
```

The `LEFT JOIN` keeps the rows of statements that `pg_stat_statements` evicted or does not track. For example, this occurs when the `track` setting of `pg_stat_statements` is different.

`pg_stat_statements` stores one query text for each `queryid`. The text comes from the call that created the entry, so the text includes the comment of *that* call. Always read the context from `c.tags`, not from `s.query`.

**Apportioning other metrics.** You can attribute other `pg_stat_statements` metrics to a context approximately. Examples of these metrics are rows and buffer reads. Use the share of the execution time of the statement that belongs to the context:

```sql
SELECT c.tags, s.query,
       s.shared_blks_read * c.total_exec_time / nullif(s.total_exec_time, 0)
         AS est_shared_blks_read
FROM pg_stat_statement_context_totals c
JOIN pg_stat_statements s USING (userid, dbid, queryid, toplevel)
WHERE c.toplevel;
```

The result is only an **estimate**, for two reasons:

- The estimate assumes that the metric is proportional to the execution time. This is not true when contexts use the same query in different ways. For example, one context gives a selective parameter, and another context gives a non-selective parameter.
- `pg_stat_statements` counts from its last reset. This extension counts only the live bucket window (1 hour by default). To get a meaningful ratio, reset the two extensions at the same time (`pg_stat_statements_reset()` and `pg_stat_statement_context_reset()`). Then use a window that is at least as long as the time since the reset. Alternatively, compare values that cover the same period.

## Nested statements, `toplevel`, and inclusive costs

A *top-level* statement is a statement that the client sends. A *nested* statement is a statement that runs inside a function, a procedure, a `DO` block, or a trigger.

The [`track`](configuration.md#track) setting controls which statements the extension records:

- With `track = top` (the default), the extension records only top-level statements.
- With `track = all`, the extension also records nested statements. Nested statements have `toplevel = false`.

The [`nested_tags`](configuration.md#nested_tags) setting controls the tags of a nested statement. The default is `inherit`. With `inherit`, a nested statement gets the tags of the top-level statement that ran it. Use this to find the source of nested work. For example, you can find which controller caused the queries in a trigger.

**Costs are inclusive.** The `total_exec_time` of a top-level statement includes the time of all the statements that it ran. This includes nested statements and `AFTER` triggers. Do not add the top-level rows and the nested rows together, because the nested time is then counted two times. To get totals for each application, use only the top-level rows:

```sql
SELECT tags->>'controller' AS controller, sum(calls) AS calls,
       sum(total_exec_time) AS ms
  FROM pg_stat_statement_context_totals
 WHERE toplevel
 GROUP BY 1
 ORDER BY ms DESC;
```

**`toplevel` matches `pg_stat_statements` on all supported versions.** Thus, each row of this extension joins to a maximum of one `pg_stat_statements` row. The extension uses the same rules as `pg_stat_statements` to set `toplevel`:

- SQL `EXECUTE` runs a prepared plan. If the `EXECUTE` is top-level, the plan is also top-level. The extension does not record `EXECUTE` or `PREPARE` as utility statements.
- On PostgreSQL 17 and later, all other utility statements (for example `CALL`, `DO`, `CREATE TABLE AS`, and `EXPLAIN`) make the statements that they run nested. Functions that run while PostgreSQL plans a statement are also nested.
- On PostgreSQL 14–16, a utility statement makes the statements that it runs nested only if `pg_stat_statements` tracks that utility statement. To get the same result, the extension reads the `pg_stat_statements` settings `pg_stat_statements.track` and `pg_stat_statements.track_utility` for each utility statement. If `pg_stat_statements` is not loaded, the extension uses its own `track` and `track_utility` settings.

These rules set only the value of `toplevel`. The settings of this extension (`track`, `track_utility`, and `untagged`) control which statements the extension records.

## Visibility and privacy

Tags can contain personal data, for example an e-mail address in a route. The visibility rules are at least as strict as in `pg_stat_statements`:

- For queries that come from other roles, `queryid` and `tags` are `NULL`. This does not apply if the caller has the privileges of `pg_read_all_stats` or is a superuser. The function does this check, so a direct call to `pg_stat_statement_context()` also applies it.
- Queries that come from the same role are always complete.
- On PostgreSQL 14, the check (`has_privs_of_role`) is a little stricter than the check of `pg_stat_statements` on that version. A `NOINHERIT` member of `pg_read_all_stats` cannot see the tags of other roles.

```sql
GRANT pg_read_all_stats TO monitoring;   -- monitoring: an existing role
```

## Encodings and `SQL_ASCII`

The extension stores tags in the encoding of the database where the statement ran. When you query from a different database, the extension converts the tags to the encoding of that database.

- **`SQL_ASCII` origin.** Tags from a `SQL_ASCII` database have no known encoding. Thus, the extension escapes them and does not convert them. Each byte ≥ 0x80 becomes `\xHH` (two lowercase hex digits). Each `\` becomes `\\`. This applies to keys and values. You can reverse the escaping, and different keys stay different. For example, the bytes `café` written in UTF-8 in a `SQL_ASCII` database are shown as `caf\xc3\xa9`. (The jsonb text output also escapes the backslash, so this appears as `"caf\\xc3\\xa9"`. Use `->>` to get the plain text.)
- **Tags that cannot be converted.** If the extension cannot convert a tag set to the encoding of the current database, it shows the full tag set with the same `\xHH` escaping. Thus, one bad entry does not cause the query to fail.
- In a `SQL_ASCII` database, the extension shows tags from other databases without conversion.

`pg_stat_statement_context_extract()` uses the same escaping for its output in a `SQL_ASCII` database.

## `pg_stat_statement_context_activity`

This view shows the current tags of each backend. Use it with `pg_stat_activity`, and join the two views on `pid`.

```sql
SELECT a.pid, a.usename, a.state, c.state AS tags_state, c.tags,
       left(a.query, 60) AS query
  FROM pg_stat_activity a
  JOIN pg_stat_statement_context_activity c USING (pid)
 WHERE a.state <> 'idle' OR c.state = 'active'
 ORDER BY a.query_start;
```

The view has one row for each backend whose last top-level statement had resolved tags. The extension resolves tags when it is enabled and the statement has a query ID. The row contains the tags of that statement. `state` is `active` while the statement runs, and `idle` after the statement ends. `pg_stat_activity.query` works the same way: it also shows the last statement of an idle backend. Thus, you can see which code left a session `idle in transaction`.

| Column | Type | Description |
|---|---|---|
| `pid` | `integer` | The process ID of the backend, as in `pg_stat_activity.pid`. |
| `userid` | `oid` | The role that runs the statement. This is the current user when the execution of the statement started, for example after `SET ROLE`. `pg_stat_activity.usesysid` shows the session user. |
| `dbid` | `oid` | The database of the backend. |
| `queryid` | `bigint` | The query ID of the top-level statement, as in `pg_stat_activity.query_id`. It is `NULL` if the statement has no query ID. |
| `state` | `text` | `active` while the statement runs. `idle` after the statement ends, with or without success. |
| `tags` | `jsonb` | The tag set of the statement, in the same format as in the other views. It is `{}` if the statement had no tags. |

Details:

- **Only top-level statements.** The row shows the statement that the client sent. Statements that functions, procedures, and `DO` blocks run do not change the row. This is true for all values of `nested_tags` and `track`. A `CALL` shows the tags of the `CALL`.
- **Multi-statement strings.** When each statement of the string starts, it replaces the row with its own tags.
- **Prepared statements.** `EXECUTE` and the extended protocol show the tags of the text of the prepared statement. These are the same tags that the statistics record. The row also shows the `queryid` of the prepared statement. For `EXECUTE`, `pg_stat_activity.query_id` is different: it is the query ID of the `EXECUTE` statement. `PREPARE` does not change the row. `DEALLOCATE` is a utility statement, so it shows its own tags.
- **When the row changes.** The extension updates the row when execution starts. This is when the executor runs, or when a utility statement starts. Before that, the row of the previous statement stays. Thus, the row of the previous statement also shows the time for parsing, planning, and waiting for a lock that the planner needs. Statements that run during planning also show in the row of the previous statement, on all server versions. For example, an `IMMUTABLE` function that the planner folds to a constant runs during planning. To find these cases, compare `queryid` with `pg_stat_activity.query_id`.
- **What does not change the row.** A portal that is bound but never executed does not change the row (extended protocol `Bind`, then `Close` or `Sync`). A cursor that closes at the end of its transaction also does not change the row.
- **When the row disappears.** The extension removes the row when a top-level statement has no resolved tags. For example, this occurs with `pg_stat_statement_context.enabled = off`, or when the statement has no query ID. The extension also removes the row when the backend exits. Parallel workers have no rows.
- **Visibility.** The rules are the same as for the statistics views (see [Visibility and privacy](#visibility-and-privacy)). For backends of other roles, `queryid`, `state`, and `tags` are `NULL`, unless you have the privileges of `pg_read_all_stats`. The view always shows `pid`, `userid`, and `dbid`, as `pg_stat_activity` does.
- **Cost.** Each of the `MaxBackends` backends has one shared slot. The size of a slot is `max_tagset_bytes` bytes plus a small header. With the default settings, the total is 67–75 kB, depending on the PostgreSQL version (see [Shared memory sizing](configuration.md#shared-memory-sizing)). The extension writes the slot when a top-level statement starts and when it ends. It does not use a lock for this. A reader never blocks a writer. If a slot changes while a reader copies it, the reader copies it again. For the measured overhead, see [benchmarks](benchmarks.md#activity-view).
- **Not included in `pg_stat_statement_context_info().shmem_bytes`.** That column shows only the statistics store. To see the size of the activity slots, look in `pg_shmem_allocations` for the name `pg_stat_statement_context activity`.

## `pg_stat_statement_context_counters()`

```sql
SELECT * FROM pg_stat_statement_context_counters();
```

This function returns the row of [`pg_stat_statement_context_info()`](#pg_stat_statement_context_info) without `oldest_bucket`. It has all the other columns, in the same order and with the same meaning. It reads only the shared header of the store. Thus, its cost does not change with the number of entries. `pg_stat_statement_context_info()` must scan all entries to find `oldest_bucket`. Use `pg_stat_statement_context_counters()` for each scrape, as the [integrations](integrations/README.md) do.

Like `pg_stat_statement_context_info()`, this function:

- can be read by all roles,
- first moves the current bucket forward to the clock, and
- includes the pending extraction counts of the calling session.

## `pg_stat_statement_context_info()`

This function returns one row of counters for the full store. All roles can read it, as with `pg_stat_statements_info`:

```sql
SELECT * FROM pg_stat_statement_context_info();
```

| Column | Type | Description |
|---|---|---|
| `entries` | `bigint` | The number of entries in the table now. This includes dead entries that the extension did not reclaim yet. |
| `max_entries` | `bigint` | The `max_entries` setting. |
| `dealloc` | `bigint` | The number of eviction passes that ran because the table was full. |
| `reclaimed_entries` | `bigint` | Dead entries (all buckets expired) that those passes or the [reclaim worker](configuration.md#reclaim_worker) reclaimed. This is normal housekeeping, and no history is lost. See [Eviction](configuration.md#eviction). |
| `evicted_entries` | `bigint` | Live entries that those passes evicted, because the reclaim of dead entries did not free sufficient space. If this value increases, `max_entries` is too small. |
| `dropped_records` | `bigint` | Calls that the extension did not record, because a pass could not free an entry. A value that is not zero shows that the table is much too small, or that memory is low. |
| `buckets` | `int` | The `bucket_count` setting. |
| `bucket_seconds` | `int` | The `bucket_interval` setting, in seconds. |
| `oldest_bucket` | `timestamptz` | The start of the oldest live bucket of all entries. It is equal to `min(bucket_start)` in the view. It is `NULL` if no bucket is live. The function uses the same current bucket as `current_bucket_start` to find it. Thus, it is never older than `current_bucket_start - (buckets - 1) * bucket_seconds`. |
| `current_bucket_start` | `timestamptz` | The start of the current bucket of the store. This is the newest bucket that the extension saw. It never moves backward, even if the clock moves backward. |
| `last_closed_bucket_start` | `timestamptz` | The start of the bucket before the current bucket: `current_bucket_start - bucket_seconds`. This is the newest bucket that cannot get more calls. [`pg_stat_statement_context_last_bucket`](#pg_stat_statement_context_last_bucket) shows it. After startup, the first such bucket covers only part of an interval. |
| `shmem_bytes` | `bigint` | The exact shared memory size that the statistics store requested at startup (see [Shared memory sizing](configuration.md#shared-memory-sizing)). |
| `cap_shmem_bytes` | `bigint` | The exact shared memory size that the separate [cardinality caps](configuration.md#cardinality_cap_slots) table requested at startup. The extension allocates this table even if no cap is set. At the maximum `cardinality_cap_slots`, it is approximately 576 MiB. |
| `invalid_tags` | `bigint` | Tags that the extension rejected as malformed: NUL bytes, invalid encoding, keys longer than 63 bytes, and malformed pairs (see [the tag pipeline](extractors.md#the-tag-pipeline)). |
| `dropped_tags` | `bigint` | Valid tags that the extension dropped, because the tag set would be larger than `max_tags` or `max_tagset_bytes`. |
| `heuristic_scans` | `bigint` | Statements whose comments the extension found with the heuristic tail scan. The extension uses this scan for `position=append` on statements longer than `scan_window`. |
| `regex_compile_failures` | `bigint` | Regex extractors and [`normalize`](configuration.md#normalize) rules that did not compile in a backend at run time. The extension disabled them in that backend. This includes compiles that the [100 ms compile time limit](extractors.md#regex) stopped. |
| `utility_missing_queryid` | `bigint` | Tracked utility statements that had no query ID, which the extension did not record. Usually this shows that the `shared_preload_libraries` order is wrong (see the [README](../README.md#load-order)). It also increases when the order is correct, if a utility statement runs again from a plan cache. For example, this occurs for a named prepared `SET` over the extended protocol. pg_stat_statements clears the query ID after the first execution, so neither extension counts the later executions. |
| `capped_tags` | `bigint` | Tag values that the extension recorded as JSON `null`, because their key reached its [cardinality cap](configuration.md#cardinality_cap). This includes the values in `cap_table_full`. |
| `cap_table_full` | `bigint` | The part of `capped_tags` that the extension collapsed because the shared table of admitted values was full ([`cardinality_cap_slots`](configuration.md#cardinality_cap_slots)). |
| `stats_reset` | `timestamptz` | The time of the last `pg_stat_statement_context_reset()`. If no reset occurred, the time of the server start that began with an empty store. Statistics that the extension [loads at startup](configuration.md#save) keep their saved `stats_reset`. |
| `stats_reset_epoch` | `bigint` | `stats_reset` in whole Unix epoch seconds. Use it for exporters that need a number. |
| `exemplar_shmem_bytes` | `bigint` | The shared memory that the [exemplar](configuration.md#exemplar_keys) values use. This is part of `shmem_bytes`. It is never more than `exemplar_memory`. It is 0 when `exemplar_keys` is empty. |
| `exemplar_value_bytes` | `int` | The maximum length of an exemplar value that the extension can store, in bytes (maximum 256). The extension calculates it from `exemplar_memory`, `max_entries`, and the number of keys. |
| `exemplar_values_dropped` | `bigint` | Exemplar values that the extension did not store because they were longer than `exemplar_value_bytes`. The entry keeps its previous value. |

Each backend collects the extraction counters (`invalid_tags`, `dropped_tags`, `heuristic_scans`, `capped_tags`, and `cap_table_full`). The backend adds them to the shared counters when a statement ends. `pg_stat_statement_context_info()` includes the pending counts of the calling session.

To find `oldest_bucket`, the function scans all entries while it holds the shared lock of the store. [`pg_stat_statement_context_counters()`](#pg_stat_statement_context_counters) returns the other columns without this scan.

The current bucket can move during the scan. This occurs a maximum of one time in each `bucket_seconds`, unless the clock is stepped. If the current bucket moves, the scan starts again. The function does a maximum of 3 scans in total. The row always agrees with its own `current_bucket_start`, which is the current bucket of the last scan. If the bucket also moved during the last scan, the current bucket of the store is already newer when the function returns the row.

A cancel, a `statement_timeout`, or a termination stops all scans, including the last scan. The scan stops after a maximum of one more entry.

## `pg_stat_statement_context_reset()`

```sql
SELECT pg_stat_statement_context_reset();
```

This function removes all entries, sets all `pg_stat_statement_context_info()` counters to zero, and sets `stats_reset`. It also clears the values that the [cardinality caps](configuration.md#cardinality_cap) admitted. Thus, each key can again take its cap of different values. Statements that still run in other sessions are recorded after the reset, when they end.

By default, only superusers and the owner of the extension (the role that ran `CREATE EXTENSION`) can call this function. To let another role call it:

```sql
GRANT EXECUTE ON FUNCTION pg_stat_statement_context_reset() TO monitoring;   -- an existing role
```

## `pg_stat_statement_context_extract()`

```
pg_stat_statement_context_extract(query text,
                                  stmt_location int DEFAULT -1,
                                  stmt_len int DEFAULT 0)
  RETURNS jsonb
```

This is a debug function. It does the same extraction as the hooks, with the current configuration, on one statement of `query`. It returns the result. It does not run the query, and it records nothing.

- [`appname`](extractors.md#appname) extractors read the current `application_name` of the calling session.
- The function applies the current [`tags_override`](configuration.md#tags_override) of the session, as for a real statement.
- The tags from these two sources are in `tags` with the other tags.

```sql
SELECT jsonb_pretty(pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show,application:app*/'));
```

```
{
    "oom": false,
    "tags": {
        "action": "show",
        "controller": "users"
    },
    "ntags": 2,
    "footer": false,
    "stmt_end": 57,
    "heuristic": false,
    "stmt_start": 0,
    "capped_tags": 0,
    "dropped_tags": 0,
    "invalid_tags": 0,
    "tagset_bytes": 29,
    "heuristic_scans": 0,
    "normalized_tags": 0,
    "normalize_failures": 0,
    "regex_compile_failures": 0
}
```

| Key | Meaning |
|---|---|
| `capped_tags` | Values shown as `null` because their key reached its [cardinality cap](configuration.md#cardinality_cap). The function only reads the caps, in the [scope](configuration.md#cardinality_cap_scope) of the caller. It never admits a value, so a call does not use any part of the cap of a key. The function does not add this count to `pg_stat_statement_context_info().capped_tags`. |
| `footer` | `true` if the tags came from a comment after the range of the statement (the multi-statement fallback). |
| `heuristic` | `true` if the extension used the heuristic tail scan. |
| `invalid_tags`, `dropped_tags`, `heuristic_scans`, `regex_compile_failures` | The counts of this call for the `pg_stat_statement_context_info()` counters with the same names. |
| `normalize_failures` | Tags that the extension dropped because a `normalize` rule failed, or because a compile failure disabled the rule. |
| `normalized_tags` | Tags whose value the [`normalize`](configuration.md#normalize) rules changed. |
| `ntags`, `tagset_bytes` | The number of tags, and their size after serialization. Compare them with `max_tags` and `max_tagset_bytes`. |
| `oom` | `true` if the extraction ran out of memory. In that case, the hooks record no tags. |
| `stmt_start`, `stmt_end` | The byte range of the statement that the function scanned. The range includes the comments before the statement. |
| `tags` | The tag set that the extension would record. |

**Arguments.** `stmt_location` and `stmt_len` select one statement of a multi-statement string. They are in bytes, as the parser gives them. `stmt_location = -1` selects the full string. `stmt_len = 0` selects to the end of the string. Values that are out of range cause an error.

```sql
SELECT pg_stat_statement_context_extract('SELECT 1; SELECT 2; /*controller:x*/', 0, 8) -> 'tags' AS first,
       pg_stat_statement_context_extract('SELECT 1; SELECT 2; /*controller:x*/', 9, 9) -> 'tags' AS second;
-- first: {}, second: {"controller": "x"}
```

**Notes.**

- `EXECUTE` is **revoked from `PUBLIC`**. The function runs the regex engine on any input, which uses CPU. It also shows the extractor configuration. Superusers and the owner of the extension (the role that ran `CREATE EXTENSION`) can call it. They can give access with `GRANT EXECUTE ON FUNCTION pg_stat_statement_context_extract(text, int, int) TO ...`.
- The function works when `enabled = off`, but the library must be preloaded.
- The function does not change the statistics or the `pg_stat_statement_context_info()` counters, with one exception: `regex_compile_failures`. A regex compile failure disables the extractor (or the `normalize` rule) in the calling backend, and the extension counts the failure. The hooks do the same.
- For a single statement, or for the first statement of a string, the result is the same as the result of the hooks. On PostgreSQL 18, a later statement of a multi-statement string can start more than `scan_window` bytes into the string. For that statement, the hooks can see leading comments that this function does not see.
- In a `SQL_ASCII` database, the function escapes its output as described [above](#encodings-and-sql_ascii). `tagset_bytes` counts the stored bytes.
