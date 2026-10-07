# SQL interface

`CREATE EXTENSION pg_stat_statement_context` installs these objects (in the
extension's schema; the extension is relocatable):

| Object | Kind | Default access |
|---|---|---|
| [`pg_stat_statement_context`](#the-views) | view, one row per entry and live bucket | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context_totals`](#the-views) | view, one row per entry, live buckets summed | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context_last_bucket`](#pg_stat_statement_context_last_bucket) | view, one row per entry, last closed bucket only; `pg_stat_statement_context_last_bucket(showtags)` is the function behind it | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context(showtags, merge_buckets)`](#pg_stat_statement_contextshowtags-merge_buckets) | set-returning function behind both views | `PUBLIC` |
| [`pg_stat_statement_context_activity`](#pg_stat_statement_context_activity) | view, current tags of each backend (join `pg_stat_activity` on `pid`); `pg_stat_statement_context_activity()` is the function behind it | `SELECT` granted to `PUBLIC` |
| [`pg_stat_statement_context_info()`](#pg_stat_statement_context_info) | store and diagnostic counters | `PUBLIC` |
| [`pg_stat_statement_context_reset()`](#pg_stat_statement_context_reset) | clears all statistics | superuser only |
| [`pg_stat_statement_context_extract(query, stmt_location, stmt_len)`](#pg_stat_statement_context_extract) | debug: show the tags extracted from a statement | superuser only |

A superuser can `GRANT EXECUTE` on the restricted functions to other roles.
The views and functions work only when the library is in
`shared_preload_libraries`; otherwise they fail with
`pg_stat_statement_context must be loaded via "shared_preload_libraries"`.

Statistics are cluster-wide: every database's statements are collected, and
the views show all of them (filter on `dbid` if needed), as in
`pg_stat_statements`.

## The views

```sql
SELECT * FROM pg_stat_statement_context;          -- per bucket
SELECT * FROM pg_stat_statement_context_totals;   -- per entry, over the whole window
```

[`pg_stat_statement_context_last_bucket`](#pg_stat_statement_context_last_bucket)
has the same columns too.

Both have the same columns:

| Column | Type | Description |
|---|---|---|
| `bucket_start` | `timestamptz` | Start of the time bucket. In `_totals`, the start of the oldest live bucket of the entry. |
| `userid` | `oid` | Role that ran the statement. |
| `dbid` | `oid` | Database in which it ran. |
| `queryid` | `bigint` | Query ID, identical to `pg_stat_statements.queryid`. |
| `toplevel` | `bool` | True if the statement was run by the client, false if it was nested (only with `track = all`). |
| `tags` | `jsonb` | The tag set, an object of string values, e.g. `{"action": "show", "controller": "users"}`; `{}` for statements recorded with `untagged = record`. A value is JSON `null` (never a string) when its key had reached its [cardinality cap](configuration.md#cardinality_cap); client input can't produce `null`. |
| `calls` | `bigint` | Number of completed executions in the bucket (or window). |
| `total_exec_time` | `float8` | Total execution time in milliseconds. |
| `calls_total` | `bigint` | Calls of the **entry** since `stats_since`, whatever bucket they were counted in. Never decreases while the entry exists: expired buckets don't lower it. |
| `exec_time_total` | `float8` | Execution time of the entry since `stats_since`, in milliseconds, like `calls_total`. |
| `stats_since` | `timestamptz` | When the entry was created, i.e. when `calls_total` and `exec_time_total` started counting. |
| `exemplars` | `jsonb` | The most recent value of each key in [`exemplar_keys`](configuration.md#exemplar_keys) seen for the entry, e.g. `{"traceparent": "00-…-01"}`, even if the key is not a grouping tag; `{}` when none is stored. Per entry, like `calls_total`. `NULL` whenever `tags` is (other roles' rows without `pg_read_all_stats`, `showtags = false`). |

An entry is one (`userid`, `dbid`, `queryid`, `toplevel`, `tags`)
combination. `pg_stat_statement_context` returns one row per **live** bucket
of each entry, oldest first within an entry; `_totals` returns one row per
entry. Expired buckets (older than `bucket_count × bucket_interval`) are
never shown.

**Per-entry counters.** `calls_total`, `exec_time_total` and `stats_since`
belong to the entry, not to a bucket, like `pg_stat_statements`' own
counters and its `stats_since` (PostgreSQL 17):

- They **repeat on every bucket row** of an entry in
  `pg_stat_statement_context`. Don't `sum()` them over that view; read them
  from `_totals` (one row per entry), or `DISTINCT ON` the entry.
- They only grow while the entry exists, so they suit counter-based
  monitoring (Prometheus `rate()`, see [integrations](integrations/README.md)).
  They start over, with a new `stats_since`, when the entry is created
  again: after `pg_stat_statement_context_reset()`, or after the entry was
  [reclaimed or evicted](configuration.md#eviction). An entry whose buckets
  have all expired is not shown, but keeps its counters until it is
  reclaimed; if it is called again before that, they continue.
- Unlike `calls`, they cover the entry's whole life, not just the live
  window, so they don't match `sum(calls)` once a bucket has expired.

**Counter semantics.**

- `calls` counts completed executions: for plannable statements, executor
  runs that reached `ExecutorEnd`; for utilities, completed utility calls. A
  cursor fetched many times, or a portal executed in several `Execute`
  messages, is **one** call, attributed to the bucket in which it finishes.
  Failed statements are not counted.
- `total_exec_time` is measured exactly as pgss measures it.

See [Time buckets](configuration.md#time-buckets) for bucket semantics
("completions per interval").

Statements by bucket for the last hour, for example:

```sql
SELECT bucket_start, tags->>'controller' AS controller,
       sum(calls) AS calls, round(sum(total_exec_time)::numeric, 2) AS ms
  FROM pg_stat_statement_context
 WHERE toplevel
 GROUP BY 1, 2
 ORDER BY 1, 4 DESC;
```

## `pg_stat_statement_context(showtags, merge_buckets)`

```
pg_stat_statement_context(showtags boolean DEFAULT true,
                          merge_buckets boolean DEFAULT false)
  RETURNS SETOF (bucket_start, userid, dbid, queryid, toplevel, tags,
                 calls, total_exec_time, calls_total, exec_time_total, stats_since,
                 exemplars)
```

The view `pg_stat_statement_context` is `pg_stat_statement_context(true, false)`
and `pg_stat_statement_context_totals` is `pg_stat_statement_context(true, true)`.

- `merge_buckets = true` sums `calls` and `total_exec_time` over each entry's
  live buckets and returns one row per entry, with `bucket_start` set to the
  oldest live bucket.
- `showtags = false` returns `tags` as `NULL` in every row, which is cheaper
  when you only need the counters.

## `pg_stat_statement_context_last_bucket`

```sql
SELECT * FROM pg_stat_statement_context_last_bucket;
```

The same columns as [the views](#the-views), for the **last closed bucket**
only: the bucket just before the store's current one, whose start is
`pg_stat_statement_context_info().last_closed_bucket_start`. No call can be
recorded in it any more, so its counts are final. This is the bucket to
export as a per-interval gauge (see [integrations](integrations/README.md)).

- One row per entry that has calls in that bucket; entries without any are
  left out. `calls` and `total_exec_time` are that bucket's alone;
  `calls_total`, `exec_time_total` and `stats_since` are the entry's, as in
  the other views.
- The bucket is chosen once per query, from the store's current bucket,
  which never moves backwards (see
  [Time buckets](configuration.md#time-buckets)): after a backward clock
  step it stays the newest bucket that was ever closed. It is empty if no
  statement finished in it.
- Bucket boundaries fall on wall-clock multiples of `bucket_interval`, so
  the bucket in progress at server start began before the server did. Until
  the first boundary after startup, the last closed bucket predates any
  data. At that boundary, which can be well under one `bucket_interval`
  after startup, the startup bucket becomes the last closed bucket. It holds
  only the calls since startup, so it is a partial interval.
- With `bucket_count = 1` it is always empty: only the current bucket is
  kept.
- Visibility is the same as for the other views. The function
  `pg_stat_statement_context_last_bucket(showtags boolean DEFAULT true)` is
  behind the view; `showtags = false` returns `tags` as `NULL`.

## Joining to pg_stat_statements

This extension stores only `calls` and `total_exec_time` per context. Query
text and every other metric come from `pg_stat_statements`, joined on
`(userid, dbid, queryid, toplevel)`. The top 20 contexts by execution time,
with the query text:

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

The `LEFT JOIN` keeps rows whose statement pgss has evicted or doesn't track
(for example when its own `track` setting differs).

pgss stores one query text per `queryid`, taken from whichever call created
its entry, so that text includes *that* call's comment. Read the context from
`c.tags`, never from `s.query`.

**Apportioning other metrics.** pgss metrics such as rows or buffer reads can
be attributed to a context approximately, by the context's share of the
statement's execution time:

```sql
SELECT c.tags, s.query,
       s.shared_blks_read * c.total_exec_time / nullif(s.total_exec_time, 0)
         AS est_shared_blks_read
FROM pg_stat_statement_context_totals c
JOIN pg_stat_statements s USING (userid, dbid, queryid, toplevel)
WHERE c.toplevel;
```

This is an **estimate**, with two caveats:

- It assumes the metric is proportional to execution time, which is not true
  when contexts use the same query differently (one passes a selective
  parameter, another a non-selective one).
- pgss accumulates since its last reset, while this extension covers only the
  live bucket window (1 hour by default). Reset both together
  (`pg_stat_statements_reset()` and `pg_stat_statement_context_reset()`) and
  use a window at least as long as the time since the reset, or compare like
  with like, for the ratio to be meaningful.

## Nested statements, `toplevel` and inclusive costs

With `track = all`, statements run inside functions, procedures, `DO` blocks
and triggers are recorded too, with `toplevel = false`. By default
(`nested_tags = inherit`) they carry the tags of the top-level statement that
ran them, which answers questions like "which controller caused this
trigger's queries?".

**Costs are inclusive.** A top-level statement's `total_exec_time` already
includes the time of everything it ran, including nested queries and `AFTER`
triggers. Adding parent and child rows double-counts. For per-application
totals, filter on `toplevel`:

```sql
SELECT tags->>'controller' AS controller, sum(calls) AS calls,
       sum(total_exec_time) AS ms
  FROM pg_stat_statement_context_totals
 WHERE toplevel
 GROUP BY 1
 ORDER BY ms DESC;
```

**`toplevel` matches pgss on every version**, so the join is one-to-one:

- A plan run by SQL `EXECUTE` is top-level when the `EXECUTE` is; `EXECUTE`
  and `PREPARE` themselves are not recorded as utility statements.
- On PostgreSQL 17 and later, every other utility statement (`CALL`, `DO`,
  `CREATE TABLE AS`, `EXPLAIN`, ...) makes the statements it runs nested, and
  so do functions evaluated while a statement is being planned.
- On PostgreSQL 14–16, pgss counts a utility statement as a nesting level only
  when it tracks that utility itself. The extension mirrors this by reading
  pgss's own settings (`pg_stat_statements.track` and
  `pg_stat_statements.track_utility`) on each utility statement. When pgss
  isn't loaded, its own `track` and `track_utility` are used instead.

Whether a statement is *recorded* always follows this extension's own
`track`, `track_utility` and `untagged` settings.

## Visibility and privacy

Tags can contain personal data, for example an e-mail address in a route.
Visibility is at least as strict as in pgss:

- Rows of other roles show `queryid` and `tags` as `NULL`, unless the caller
  has the privileges of `pg_read_all_stats` (or is a superuser). The check is
  made inside the function, so calling `pg_stat_statement_context()` directly
  doesn't bypass it.
- Your own rows are always complete.
- On PostgreSQL 14 the check (`has_privs_of_role`) is slightly stricter than
  pgss's there: a `NOINHERIT` member of `pg_read_all_stats` doesn't get to see
  other roles' tags.

```sql
GRANT pg_read_all_stats TO monitoring;   -- monitoring: an existing role
```

## Encodings and `SQL_ASCII`

Tags are stored in the encoding of the database where the statement ran, and
converted to the encoding of the database you query from.

- **`SQL_ASCII` origin.** Tags recorded in a `SQL_ASCII` database have no
  known encoding, so they are escaped instead of converted: every byte
  ≥ 0x80 becomes `\xHH` (two lowercase hex digits) and every `\` becomes
  `\\`, in both keys and values. The escaping is reversible, and distinct
  keys stay distinct. For example, the bytes `café` written in UTF-8 in a
  `SQL_ASCII` database are shown as `caf\xc3\xa9`. (In jsonb text output a
  backslash itself is shown escaped, so this appears as `"caf\\xc3\\xa9"`; use
  `->>` to get the plain text.)
- **Unconvertible tags.** If a tag set can't be converted to the current
  database's encoding, that whole tag set is shown with the same `\xHH`
  escaping, so one bad entry never makes the query fail.
- In a `SQL_ASCII` database, tags recorded in other databases are shown
  unconverted.

`pg_stat_statement_context_extract()` escapes its output the same way when
run in a `SQL_ASCII` database.

## `pg_stat_statement_context_activity`

Shows each backend's current tags. It is a companion to `pg_stat_activity`:
join the two on `pid`.

```sql
SELECT a.pid, a.usename, a.state, c.state AS tags_state, c.tags,
       left(a.query, 60) AS query
  FROM pg_stat_activity a
  JOIN pg_stat_statement_context_activity c USING (pid)
 WHERE a.state <> 'idle' OR c.state = 'active'
 ORDER BY a.query_start;
```

There is one row per backend whose last top-level statement had tags
resolved, which happens when the extension is enabled and the statement has a
query ID. The row holds that statement's tags. `state` is `active` while the
statement runs and `idle` after it ends. This mirrors
`pg_stat_activity.query`, which also keeps showing the last statement of an
idle backend. That way you can still see which code left a session
`idle in transaction`.

| Column | Type | Description |
|---|---|---|
| `pid` | `integer` | Process ID of the backend, as in `pg_stat_activity.pid`. |
| `userid` | `oid` | Role the statement runs as: the current user when its execution started, for example after `SET ROLE`. `pg_stat_activity.usesysid` shows the session user instead. |
| `dbid` | `oid` | Database of the backend. |
| `queryid` | `bigint` | Query ID of the top-level statement, as in `pg_stat_activity.query_id`, or `NULL` if it has none. |
| `state` | `text` | `active` while the statement runs, `idle` once it has ended (successfully or not). |
| `tags` | `jsonb` | The statement's tag set, in the same format as the views; `{}` if it had no tags. |

Some details:

- **Only top-level statements.** The row shows the statement the client
  sent. Statements run by functions, procedures and `DO` blocks don't change
  the row, whatever `nested_tags` and `track` say. A `CALL` shows the tags of
  the `CALL`.
- **Multi-statement strings.** Each statement of the string replaces the row
  with its own tags when it starts.
- **Prepared statements.** `EXECUTE` and the extended protocol show the tags
  of the prepared statement's own text, the same tags the statistics record,
  and its `queryid`; for `EXECUTE`, `pg_stat_activity.query_id` is the
  `EXECUTE` statement's own query ID instead.
  `PREPARE` leaves the row unchanged; `DEALLOCATE` is a utility statement
  and shows its own tags.
- **When the row changes.** The row is updated when execution starts: when
  the executor runs, or when a utility statement starts. Until then, time
  spent parsing, planning or waiting for a lock that the planner needs is
  shown against the previous statement's row, and so are statements run
  while planning (for example by an `IMMUTABLE` function that is
  constant-folded), on every server version. Compare `queryid` with
  `pg_stat_activity.query_id` to tell the two apart. A portal that is
  bound but never executed (extended protocol `Bind` followed by `Close` or
  `Sync`) doesn't change the row, nor does a cursor closed at the end of
  its transaction.
- **When the row disappears.** A top-level statement without tags resolved,
  for example with `pg_stat_statement_context.enabled = off` or without a
  query ID, removes the row, and so does the exit of the backend. Parallel
  workers have no rows.
- **Visibility.** The same rules as for the statistics views apply (see
  [Visibility and privacy](#visibility-and-privacy)). For other roles'
  backends, `queryid`, `state` and `tags` are `NULL` unless you have the
  privileges of `pg_read_all_stats`. `pid`, `userid` and `dbid` are always
  shown, as in `pg_stat_activity`.
- **Cost.** Each backend has one shared slot of `max_tagset_bytes` bytes plus
  a small header, for each of `MaxBackends` backends; at the defaults that is
  about 70 kB. The extension writes the slot when a top-level statement
  starts and ends, without taking a lock. A reader never blocks a writer: it
  retries the copy of any slot that is being written while it reads. See
  [benchmarks](benchmarks.md#activity-view) for the measured overhead.
- **Not counted in `_info().shmem_bytes`.** That column reports the
  statistics store only.

## `pg_stat_statement_context_info()`

One row of store-wide counters, readable by everyone (like
`pg_stat_statements_info`):

```sql
SELECT * FROM pg_stat_statement_context_info();
```

| Column | Type | Description |
|---|---|---|
| `entries` | `bigint` | Entries currently in the table (including dead ones not yet reclaimed). |
| `max_entries` | `bigint` | The `max_entries` setting. |
| `dealloc` | `bigint` | Eviction passes run because the table was full. |
| `reclaimed_entries` | `bigint` | Dead entries (every bucket expired) reclaimed by those passes or by the [reclaim worker](configuration.md#reclaim_worker): normal housekeeping, no history lost. See [Eviction](configuration.md#eviction). |
| `evicted_entries` | `bigint` | Live entries evicted by those passes because reclaiming dead ones did not free enough: if it grows, `max_entries` is too small. |
| `dropped_records` | `bigint` | Calls not recorded at all because a pass could free nothing; any non-zero value means severe undersizing (or memory pressure). |
| `buckets` | `int` | The `bucket_count` setting. |
| `bucket_seconds` | `int` | The `bucket_interval` setting, in seconds. |
| `oldest_bucket` | `timestamptz` | Start of the oldest live bucket of any entry (equal to `min(bucket_start)` in the view), or `NULL` if no bucket is live. It is judged against the same current bucket as `current_bucket_start`, so it is never older than `current_bucket_start - (buckets - 1) * bucket_seconds`. |
| `current_bucket_start` | `timestamptz` | Start of the store's current bucket, the newest one observed (it never moves backwards, even if the clock does). |
| `last_closed_bucket_start` | `timestamptz` | Start of the bucket before it, `current_bucket_start - bucket_seconds`: the newest bucket that can no longer receive calls, shown by [`_last_bucket`](#pg_stat_statement_context_last_bucket). The first one after startup covers only part of an interval. |
| `shmem_bytes` | `bigint` | Exact shared memory size requested at startup for the statistics store. |
| `cap_shmem_bytes` | `bigint` | Exact shared memory size requested at startup for the separate [cardinality caps](configuration.md#cardinality_cap_slots) table (allocated even when no cap is set; up to about 576 MiB at the maximum `cardinality_cap_slots`). |
| `invalid_tags` | `bigint` | Tags rejected as malformed: NUL bytes, invalid encoding, keys over 63 bytes, malformed pairs (see [the tag pipeline](extractors.md#the-tag-pipeline)). |
| `dropped_tags` | `bigint` | Valid tags dropped because the tag set would exceed `max_tags` or `max_tagset_bytes`. |
| `heuristic_scans` | `bigint` | Statements whose comments were found with the heuristic tail scan (`position=append` on statements longer than `scan_window`). |
| `regex_compile_failures` | `bigint` | Regex extractors and [`normalize`](configuration.md#normalize) rules that failed to compile in some backend at run time (including compiles stopped at the [100 ms compile time limit](extractors.md#regex)) and were disabled there. |
| `utility_missing_queryid` | `bigint` | Tracked utility statements that arrived without a query ID and were not recorded; normally a sign of the wrong `shared_preload_libraries` order (see the [README](../README.md#load-order)). It also rises, in the correct order, when a utility statement is re-executed from a plan cache (for example a named prepared `SET` over the extended protocol): pg_stat_statements clears its query ID after the first execution, so neither extension counts the re-executions. |
| `capped_tags` | `bigint` | Tag values recorded as JSON `null` because their key had reached its [cardinality cap](configuration.md#cardinality_cap), including those counted in `cap_table_full`. |
| `cap_table_full` | `bigint` | Of `capped_tags`, the values collapsed because the shared table of admitted values was full ([`cardinality_cap_slots`](configuration.md#cardinality_cap_slots)). |
| `stats_reset` | `timestamptz` | Time of the last `pg_stat_statement_context_reset()`, or of the server start that began with an empty store (statistics [loaded at startup](configuration.md#save) keep their saved `stats_reset`). |
| `stats_reset_epoch` | `bigint` | `stats_reset` in whole Unix epoch seconds, for exporters that need a number. |
| `exemplar_shmem_bytes` | `bigint` | Shared memory used by the [exemplar](configuration.md#exemplar_keys) values, part of `shmem_bytes`; never more than `exemplar_memory`, 0 when `exemplar_keys` is empty. |
| `exemplar_value_bytes` | `int` | The longest exemplar value that can be stored, in bytes (at most 256), derived from `exemplar_memory`, `max_entries` and the number of keys. |
| `exemplar_values_dropped` | `bigint` | Exemplar values not stored because they were longer than `exemplar_value_bytes` (the entry keeps its previous value). |

The extraction counters (`invalid_tags`, `dropped_tags`, `heuristic_scans`,
`capped_tags`, `cap_table_full`) are collected per backend and added to the
shared counters when a statement finishes; `_info()` includes the calling session's own pending counts.

## `pg_stat_statement_context_reset()`

```sql
SELECT pg_stat_statement_context_reset();
```

Removes every entry, zeroes every counter of `_info()` and sets
`stats_reset`. It also forgets the values admitted by the
[cardinality caps](configuration.md#cardinality_cap), so every key can take
its cap of distinct values again. Statements still running in other sessions are recorded after
the reset when they finish. Superuser-only by default; to delegate it:

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

A debug function: runs the same extraction as the hooks, with the current
configuration, on one statement of `query`, and returns what it found. It
doesn't execute the query and records nothing. [`appname`](extractors.md#appname)
extractors read the calling session's current `application_name`, and the
session's current [`tags_override`](configuration.md#tags_override) is
applied as for a real statement; these tags appear in `tags` with the others.

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
| `tags` | The tag set that would be recorded. |
| `ntags`, `tagset_bytes` | Number of tags and their serialized size (compare with `max_tags`, `max_tagset_bytes`). |
| `footer` | The tags came from a comment after the statement's range (the multi-statement fallback). |
| `heuristic` | The heuristic tail scan was used. |
| `oom` | Extraction ran out of memory (the hooks would then record no tags). |
| `stmt_start`, `stmt_end` | The byte range of the statement that was scanned, after extending it backwards over leading comments. |
| `invalid_tags`, `dropped_tags`, `heuristic_scans`, `regex_compile_failures` | This call's contribution to the `_info()` counters of the same names. |
| `normalized_tags` | Tags whose value the [`normalize`](configuration.md#normalize) rules changed. |
| `normalize_failures` | Tags dropped because a `normalize` rule failed (or was disabled by a compile failure). |
| `capped_tags` | Values shown as `null` because their key has reached its [cardinality cap](configuration.md#cardinality_cap). The function only looks at the caps, in the caller's [scope](configuration.md#cardinality_cap_scope): it never admits a value, so calling it doesn't use up any key's cap, and its count isn't added to `_info().capped_tags`. |

**Arguments.** `stmt_location` and `stmt_len` select one statement of a
multi-statement string, in bytes, the way the parser reports it:
`stmt_location = -1` means the whole string and `stmt_len = 0` means "to the
end". Out-of-range values raise an error.

```sql
SELECT pg_stat_statement_context_extract('SELECT 1; SELECT 2; /*controller:x*/', 0, 8) -> 'tags' AS first,
       pg_stat_statement_context_extract('SELECT 1; SELECT 2; /*controller:x*/', 9, 9) -> 'tags' AS second;
-- first: {}, second: {"controller": "x"}
```

**Notes.**

- It is **superuser-only by default**, because it runs the regex engine on
  arbitrary input (CPU cost) and reveals the extractor configuration. A
  superuser may `GRANT EXECUTE ON FUNCTION
  pg_stat_statement_context_extract(text, int, int) TO ...`.
- It works when `enabled = off`, but needs the library preloaded.
- It doesn't change the statistics or `_info()` counters, except
  `regex_compile_failures`: a regex compile failure disables the extractor
  (or `normalize` rule) in the calling backend and is counted, as it would be
  in the hooks.
- For a single statement, or the first statement of a string, its result is
  the same as the hooks'. For a later statement that starts more than
  `scan_window` bytes into a multi-statement string, the hooks may see
  leading comments that this function doesn't (on PostgreSQL 18).
- In a `SQL_ASCII` database the output is escaped as described
  [above](#encodings-and-sql_ascii); `tagset_bytes` counts the stored bytes.
