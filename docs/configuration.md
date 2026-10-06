# Configuration

All configuration uses GUCs (server settings). There is no separate
configuration file. Settings can be put in `postgresql.conf`, set with
`ALTER SYSTEM`, and, depending on their context, set per role, per database or
per session. All of them are visible in `pg_settings`:

```sql
SELECT name, setting, unit, context
  FROM pg_settings
 WHERE name LIKE 'pg_stat_statement_context.%'
 ORDER BY name;
```

The GUCs exist only when the library is in `shared_preload_libraries`.

## Reference

| GUC | Type | Default | Range / values | Context |
|---|---|---|---|---|
| [`enabled`](#enabled) | bool | `on` | | superuser |
| [`track`](#track) | enum | `top` | `none`, `top`, `all` | superuser |
| [`track_utility`](#track_utility) | bool | `on` | | superuser |
| [`nested_tags`](#nested_tags) | enum | `inherit` | `inherit`, `scan`, `none` | superuser |
| [`max_entries`](#max_entries) | integer | `10000` | 100 – 1073741823 | postmaster |
| [`bucket_count`](#bucket_count) | integer | `12` | 1 – 10000 | postmaster |
| [`bucket_interval`](#bucket_interval) | integer (seconds) | `300` (`5min`) | 1s – 1 day | postmaster |
| [`max_tags`](#max_tags) | integer | `8` | 1 – 64 | postmaster |
| [`max_tag_value_len`](#max_tag_value_len) | integer (bytes) | `64` | 1 – 4096 | postmaster |
| [`max_tagset_bytes`](#max_tagset_bytes) | integer (bytes) | `512` | 128 – 8192 | postmaster |
| [`scan_window`](#scan_window) | integer (bytes) | `2048` (`2kB`) | 64 B – 1 MB | sighup |
| [`extractors`](#extractors) | string | `'sqlcommenter, marginalia'` | extractor DSL | sighup |
| [`tags`](#tags) | string | `'action, controller, job'` | key list or `'*'` | sighup |
| [`exclude_tags`](#exclude_tags) | string | `'traceparent, tracestate, request_id'` | key list | sighup |
| [`untagged`](#untagged) | enum | `skip` | `skip`, `record` | sighup |
| [`normalize`](#normalize) | string | `''` | rule list | sighup |
| [`tags_override`](#tags_override) | string | `''` | `key='value'` pairs | user |
| [`cardinality_cap`](#cardinality_cap) | integer | `0` (off) | 0 – 1000000 | sighup |
| [`cardinality_cap_overrides`](#cardinality_cap_overrides) | string | `''` | `key:N` list | sighup |
| [`cardinality_cap_slots`](#cardinality_cap_slots) | integer | `16384` | 256 – 67108864 | postmaster |

Every name has the prefix `pg_stat_statement_context.`. The contexts mean:

- **superuser**: can be changed by a superuser at any time, including per
  session (`SET`), per role or per database (`ALTER ROLE/DATABASE ... SET`),
  and in `postgresql.conf` with a reload.
- **postmaster**: these size shared memory or fix the bucket layout; changing
  them needs a server restart.
- **sighup**: set in `postgresql.conf` or with `ALTER SYSTEM`, and applied on
  reload (`SELECT pg_reload_conf();` or `pg_ctl reload`).
- **user**: any user can change it, with `SET`, `SET LOCAL`, a function's
  `SET` clause or connection options, as well as per role or per database.

### `enabled`

Master switch. When `off`, the hooks do nothing: no tags are extracted and
nothing is recorded. Existing statistics are kept, and
`pg_stat_statement_context_extract()` still works.

### `track`

Which statements are recorded, as in `pg_stat_statements.track`:

- `none`: none.
- `top` (default): only top-level statements, i.e. the ones the client sent.
  A plan run by SQL `EXECUTE` counts as top-level.
- `all`: also nested statements, such as queries inside PL/pgSQL functions,
  procedures, `DO` blocks and triggers. Their rows have `toplevel = false`.
  See [Nested statements](sql-interface.md#nested-statements-toplevel-and-inclusive-costs)
  before summing them.

### `track_utility`

Whether utility statements (DDL, `VACUUM`, `CALL`, `DO`, `COPY`, ...) are
recorded. `EXECUTE` and `PREPARE` are never recorded as utilities, because the
executor records the plan they run. `DEALLOCATE` is recorded only on
PostgreSQL 17 and later, matching pgss.

### `nested_tags`

Which tags nested statements get:

- `inherit` (default): the tags of the statement that is running them (the
  "active" statement, ultimately the top-level one). A query inside a trigger
  is then attributed to the controller whose `UPDATE` fired the trigger.
- `scan`: tags from the nested statement's own source text (for example a
  comment inside the function body), plus [`appname`](extractors.md#appname)
  tags from `application_name` and the [`tags_override`](#tags_override)
  tags as they are when the nested statement starts (so a function's
  `SET pg_stat_statement_context.tags_override = ...` clause applies to the
  statements it runs).
- `none`: no tags. With `untagged = skip`, nested statements are then not
  recorded at all.

This matters only for nested statements that are recorded, i.e. with
`track = all`. Statements run while a parent statement is being *planned*
(such as constant-folded function calls) have no active statement, so they
always get only their own tags.

### `max_entries`

The maximum number of entries. An entry is one combination of (database, user,
`queryid`, `toplevel`, tag set), i.e. one **(query × context)** combination.
See [Sizing](#sizing-max_entries) and [Eviction](#eviction).

### `bucket_count`

The number of time buckets kept for each entry. History length is
`bucket_count × bucket_interval` (1 hour by default). See
[Time buckets](#time-buckets).

### `bucket_interval`

The width of each time bucket. It accepts time units, for example
`'1min'` or `'1h'`; a bare number is in seconds. It is fixed at startup so that
all backends agree on bucket boundaries.

### `max_tags`

The maximum number of tags stored per entry. It also limits the number of
capture groups in a `regex` extractor's pattern. Tags beyond the limit are
dropped (in priority order, see [the tag pipeline](extractors.md#the-tag-pipeline))
and counted in `_info().dropped_tags`.

### `max_tag_value_len`

The maximum length of a tag value, in bytes. Longer values are truncated on a
character boundary. Tag *keys* are always limited to 63 bytes; a longer key is
dropped.

### `max_tagset_bytes`

A hard cap on the size of an entry's serialized tag set, which is part of the
hash key. The size of a tag set is the sum of
`length(key) + 1 + length(value) + 1` bytes over its tags (a value
[capped](#cardinality_cap) to `null` counts as 2 bytes). Tags are kept
greedily in priority order (`tags` list order, or sorted key order with
`tags = '*'`). A tag that doesn't fit is dropped and counted in
`_info().dropped_tags`, and smaller lower-priority tags may still be kept.
This setting is the main factor in the size of an entry.

Size it comfortably above the largest tag set you expect: the sum over the
allowlisted keys of `length(key) + max_tag_value_len + 2`. The limit is
applied after the extractor chain has picked its winner. A tag set that
doesn't fit is therefore not replaced by another extractor's tags, and the
statement can end up untagged (see
[chain semantics](extractors.md#the-extractor-dsl)).

### `scan_window`

How many bytes at the start or end of a long statement are searched for
comments. Statements no longer than `scan_window` are always lexed exactly.
It also bounds the total comment bytes examined per statement. See
[Where comments are found](extractors.md#where-comments-are-found). It accepts
byte units, e.g. `'4kB'`.

### `extractors`

The extractor list, which says which comment formats are parsed and where in
the statement to look, and whether tags are also derived from
`application_name` ([`appname`](extractors.md#appname)). See
[the extractor DSL](extractors.md#the-extractor-dsl).
A malformed value is rejected when it is set or reloaded, and the previous
value stays in effect.

### `tags`

The allowlist: a comma-separated list of tag keys to keep. It is applied
**after** `rename`, so it must name the renamed key. Keys are case-sensitive,
at most 63 bytes, and may not contain whitespace or `*`; at most 1024 entries.
Tags whose key isn't listed are discarded. The list order is the tag priority
used by `max_tags` and `max_tagset_bytes`. An empty list keeps no tags at all.

`'*'` (alone) keeps every tag except those in `exclude_tags`. That is not
recommended; see [Cardinality](extractors.md#allowlist-denylist-and-cardinality).

### `exclude_tags`

The denylist: tag keys to discard. It applies **only when `tags = '*'`**.
Same syntax as `tags`, without `'*'`. The default lists well-known
high-cardinality keys.

### `untagged`

What to do with statements that end up with no tags:

- `skip` (default): don't record them, so untagged traffic doesn't use
  entries.
- `record`: record them with an empty tag set (`tags = {}`).

### `normalize`

Regex-replace rules that rewrite tag values, so that values with variable
parts (IDs, UUIDs, dates) share one entry. Empty by default (no rules). A
comma-separated list of rules, each

```
key: 'pattern' => 'replacement'
```

- `key` is the **final** tag key: rules run after `rename` and after the
  `tags` allowlist / `exclude_tags` denylist, so they see renamed keys and only
  tags that are kept (a rule for a key that isn't kept does nothing). It may be
  written bare or in single quotes; it follows the rules of `tags` keys.
- `pattern` and `replacement` are always in single quotes; write a quote as
  `''`. Commas, `:` and `=>` inside quotes need no escaping.
- Each rule replaces **every** match, exactly like
  `regexp_replace(value COLLATE "C", 'pattern', 'replacement', 'g')`: the same
  engine (PostgreSQL advanced regular expressions) and flags as the
  [`regex` extractor](extractors.md#regex), and no back-references in the
  pattern. Flags can be embedded, e.g. `'(?i)^job-'`. In the replacement,
  `\1` to `\9` insert a capture group, `\&` the whole match and `\\` a
  backslash; any other escape is rejected.
- Several rules for the same key run in order, each on the previous one's
  output. A rule may produce an empty value; the tag is kept.
- The result is then truncated to `max_tag_value_len` bytes, so a rule sees
  the whole value (even one longer than `max_tag_value_len`), and a value is
  truncated only once, after all rules.
- Limits: at most 32 rules; a pattern is 1 to 1024 bytes and a replacement
  at most 1024 bytes. A rule's output is cut at
  `max(value length, max_tag_value_len)` bytes, which also stops the
  matching there.

For example, in `postgresql.conf`, where quotes are doubled and backslashes
must be doubled too (the file's own string escaping turns `\\` into `\`):

```
pg_stat_statement_context.normalize = 'route: ''/users/\\d+'' => ''/users/:id'', route: ''/posts/\\d+'' => ''/posts/:id'''
```

or from SQL:

```sql
ALTER SYSTEM SET pg_stat_statement_context.normalize =
  $$route: '/\d+' => '/:id', job: '^(\w+)Job$' => '\1'$$;
SELECT pg_reload_conf();
```

Rules are compiled and fully validated when the value is set or reloaded;
an invalid rule (syntax, bad pattern, back-reference, reference to a missing
capture group, unknown escape, over a limit, a pattern that takes longer
than 100 ms to compile) rejects the whole value and
the previous one stays in effect. Test rules with
[`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract),
which shows the normalized tags and counts the changed values in
`normalized_tags`.

Rules run on every tagged statement, under the same safety limits as regex
extractors: each backend compiles a rule once (at first use after a
configuration change), with the same
[100 ms compile time limit](extractors.md#regex), and the engine's own
complexity limits and query cancellation apply. If a rule fails at run time (the regex engine runs out of
memory or reports an error), the tag is **dropped** rather than stored
unnormalized, which could create many entries; this is counted in the debug
function's `normalize_failures`. If a rule fails to compile in a backend at
run time (including a compile stopped at the time limit), it is disabled there until the next configuration change (counted
in `_info().regex_compile_failures`), and tags of its key are dropped. The
user's statement never fails.

### `tags_override`

Tags for the current session or transaction, for clients that cannot add
comments (some drivers and ORMs) and for prepared statements, whose text is
fixed when they are prepared. Empty by default (no tags). Any user can set
it:

```sql
BEGIN;
SET LOCAL pg_stat_statement_context.tags_override = 'controller=''users'',action=''show''';
SELECT ...;      -- tagged controller=users, action=show
COMMIT;          -- the override ends with the transaction
```

- **Syntax**: the [sqlcommenter](extractors.md#sqlcommenter) format,
  `key='value',key2='value2'`, with URL-encoded keys and values (`%20` or
  `+` for a space, `%27` for a quote, `%2C` for a comma). Whitespace around
  pairs is ignored; an empty or blank value means no override. The value is
  parsed once, when it is set, and **rejected** if it is malformed (an
  unquoted value, text that is not a pair, an unterminated quote), has an
  invalid `%`-escape, decodes to a NUL byte or to text invalid in the
  database encoding, or has a key longer than 63 bytes; the previous value
  then stays in effect:

  ```sql
  SET pg_stat_statement_context.tags_override = 'a=''%zz''';
  -- ERROR:  invalid value for parameter "pg_stat_statement_context.tags_override": "a='%zz'"
  -- DETAIL:  Pair 1 has an invalid %-escape.
  ```

- **Merging**: the override's tags are added to the tags of each statement
  (from its comments, and from `application_name` with
  [`appname`](extractors.md#appname) extractors). On a key conflict the
  **override wins**: override > comment > `application_name`. Within the
  override, the first occurrence of a key wins. The override does not stop
  the search for a [trailing footer](extractors.md#where-comments-are-found)
  when the statement has no comment of its own.
- **Pipeline**: the override's pairs go through the same
  [pipeline](extractors.md#the-tag-pipeline) as comment tags, except the
  per-extractor `keys` lists (there is no extractor): `rename`, the
  `tags` allowlist / `exclude_tags` denylist, `normalize`, truncation to
  `max_tag_value_len`, and the `max_tags` / `max_tagset_bytes` limits apply,
  with the same counters (`invalid_tags`, `dropped_tags`, ...). `rename`
  is per extractor, so the override uses the `rename` lists of the
  **comment `sqlcommenter` extractors**, in configuration order (the first
  rule whose source key matches); the `rename` lists of `marginalia`,
  `regex` and `appname` extractors are not used, and without a
  `sqlcommenter` extractor keys are not renamed.
- **When it is read**: when a statement starts executing, like
  `application_name` for `appname` extractors. A prepared statement
  (`PREPARE`/`EXECUTE`, or the extended protocol's Parse/Bind/Execute) uses
  the override in effect when it is executed, not when it was prepared. A
  `SET ... tags_override` statement itself is tagged with the previous
  value.
- **Nested statements** follow [`nested_tags`](#nested_tags): with
  `inherit` they get the tags of the top-level statement (override
  included, as it was when that statement started); with `scan` they read
  the override again when they start, so a function's `SET` clause or a
  `SET LOCAL` / `set_config()` inside a function applies to the statements
  after it; with `none` they get no tags. A `SET LOCAL` inside a function
  also lasts for the rest of the transaction, like any `SET LOCAL`.
- With `untagged = skip`, a statement whose only tags come from the override
  is tagged, and recorded.
- [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract)
  uses the session's current override.

An override can also be set per role or database (`ALTER ROLE ... SET`), in
a driver's connection options (e.g.
`options=-c pg_stat_statement_context.tags_override=job='nightly'`), or in
`postgresql.conf`. Values set per role, per database or in the file are
checked against the database encoding only when a statement uses them; a
pair invalid there is dropped and counted in `invalid_tags`, like a
comment's.

Like comments, the override is supplied by the client: any user can
attribute their statements to any tags, so tags are not a security
boundary.

### `cardinality_cap`

The maximum number of distinct values of each tag key; `0` (the default)
means no cap. Overridden per key by
[`cardinality_cap_overrides`](#cardinality_cap_overrides). It is a last line
of defense against a key whose values explode, from an unnormalized route,
a bug, or a client sending random values (see
[cardinality](extractors.md#allowlist-denylist-and-cardinality)):

```
pg_stat_statement_context.cardinality_cap = 100
```

- **Counted server-wide, per key.** Values are counted across all
  databases, users, queries and buckets. A key's first N distinct values
  (in the order statements bring them) are **admitted**; any other value is
  recorded as JSON `null`, so all its statements share one entry per query
  and other tags. Admitted values stay admitted: a value isn't forgotten
  when its entries are evicted, only by
  [`pg_stat_statement_context_reset()`](sql-interface.md#pg_stat_statement_context_reset)
  or a restart. Lowering the cap doesn't remove values already admitted.
- **Where it applies**: step 8 of the
  [tag pipeline](extractors.md#the-tag-pipeline), after `rename`,
  [`normalize`](#normalize) and truncation, to tags from every source
  (comments, `application_name`, [`tags_override`](#tags_override)), and
  only to tags that are then kept within `max_tags` and `max_tagset_bytes`.
  Values are compared byte for byte; the cap is per **final** (renamed) key.
- **`null` is unambiguous**: a client can only send strings (`'null'` and
  `''` stay strings), so `tags->'route' = 'null'` finds exactly the collapsed
  statements; `tags->>'route'` is SQL `NULL` for them.
- **Counters**: each collapsed value is counted in `_info().capped_tags`;
  [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract)
  shows what would collapse without admitting anything.
- **When**: a value is admitted when its tags are extracted, at
  `ExecutorStart`, so it takes its place even if the statement then fails.
  `ExecutorStart` also runs for statements that are never executed or
  recorded: a portal that is bound but never executed (extended protocol
  `Bind` without `Execute`) and a plain `EXPLAIN` (without `ANALYZE`). Their
  values use cap space too. Under concurrency a key
  can very rarely collapse one value too many while two sessions admit its
  last values at the same time; it never exceeds its cap. While a `_reset()`
  clears the whole table (needed about once every million resets, when
  its generation number wraps around), new values collapse to `null` for
  that moment.
- **Cost**: the check is a lock-free lookup in a shared hash table (a few
  atomic reads, plus a compare-and-swap for a new value), with no lock
  taken; see [benchmarks](benchmarks.md#cardinality-caps). With no cap
  configured, nothing is checked.

### `cardinality_cap_overrides`

Per-key caps that take precedence over
[`cardinality_cap`](#cardinality_cap): a comma-separated list of `key:N`
entries. `N = 0` exempts the key from the default cap. Empty by default.

```
pg_stat_statement_context.cardinality_cap = 100
pg_stat_statement_context.cardinality_cap_overrides = 'route:500, job:0'
```

Whitespace around entries, keys and numbers is ignored. The key is
everything before the entry's last `:` (so `a:b:3` caps the key `a:b`), at
most 63 bytes, without whitespace or `*`; it is matched exactly against the
final (renamed) key. `N` is 0 – 1000000. A malformed list (an entry without
`:N`, an empty entry, a key listed twice, more than 1024 entries) is
rejected and the previous value stays in effect. Overrides work without a
default cap (`cardinality_cap = 0`): then only the listed keys are capped.

### `cardinality_cap_slots`

The number of distinct (key, value) pairs the caps can track, server-wide,
in a shared table allocated at startup (8 bytes per slot, plus 16 bytes per
key slot, one key slot per 16 value slots and at least 64; about 144 kB at
the default). It is allocated even while no cap is set, so caps can be
turned on with a reload. This memory is not part of `_info().shmem_bytes`;
`_info().cap_shmem_bytes` reports its exact size (about 576 MiB at the
maximum of 2^26 slots).

Size it above the sum of the caps of the keys you expect, with headroom
(the table is an open-addressing hash table that slows down and fills
early when nearly full). When a new value finds no room (or a new key finds
no key slot), the value is recorded as `null`, as if over its cap, and
counted in both `_info().capped_tags` and `_info().cap_table_full`: the
caps fail closed. `pg_stat_statement_context_reset()` empties the table.

## Changing the configuration from SQL

The `sighup` settings, including `extractors`, `tags`, `exclude_tags` and
`normalize`, can
be changed without a restart:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'sqlcommenter, marginalia(position=prepend)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'application, controller, action, job';
SELECT pg_reload_conf();
```

`pg_reload_conf()` only sends the reload signal. Every session, including the
one that called it, applies the new values shortly afterwards, before it runs
its next command. Interactively that is immediate in practice, but a script
that changes the configuration and immediately tests it should pause first,
for example with `SELECT pg_sleep(0.5);`, and can check the value with `SHOW`.

The value is fully validated (including regex compilation) when it is set, so
a bad value fails immediately:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'nonsense';
-- ERROR:  invalid value for parameter "pg_stat_statement_context.extractors": "nonsense"
-- DETAIL:  Unknown extractor "nonsense".
```

If a bad value reaches `postgresql.conf` directly, the reload logs the error
and keeps the previous configuration. To go back to the defaults:

```sql
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
SELECT pg_reload_conf();
```

The `superuser` settings can also be changed for one session, for example to
record nested statements while investigating:

```sql
SET pg_stat_statement_context.track = 'all';
```

## Sizing `max_entries`

`max_entries` counts **(query × context) combinations**, independent of
`bucket_count`. Each entry holds its own ring of `bucket_count` counter slots,
so 5,000 recurring combinations need 5,000 entries whether they are active in
one bucket or in all of them. A combination keeps its entry while any of its
buckets is live; once all of its buckets have expired the entry is *dead* and
is the first to be reclaimed when space is needed.

Size it to at least the number of distinct (database × user × `queryid` ×
`toplevel` × tag set) combinations seen within one history window
(`bucket_count × bucket_interval`), plus headroom. For example, 400 query
fingerprints, each run from about 10 controller/action pairs by one
application user, need about 4,000 entries.

Shared memory is preallocated at startup. With the defaults an entry takes
about 0.9 kB (the tag set, `max_tagset_bytes`, dominates; each bucket adds
24 bytes), so 10,000 entries use about 8.4 MB.
`SELECT shmem_bytes FROM pg_stat_statement_context_info();` reports the exact
size.

## Time buckets

Each entry keeps `calls` and `total_exec_time` per bucket of
`bucket_interval`, for the last `bucket_count` buckets. Bucket boundaries fall
on wall-clock multiples of `bucket_interval`, and all backends agree on them.

- **Completions per interval.** A statement is attributed to the bucket in
  which it *finishes* (`ExecutorEnd`, or the end of a utility statement), with
  all of its execution time, even if it started in an earlier bucket (for
  example a long-running cursor). Buckets therefore show completions per
  interval, not work done per interval.
- **Rolling window.** Buckets older than `bucket_count` intervals are hidden
  from the views immediately, and their slots are reused lazily. No background
  worker is needed.
- **Clock changes.** The current bucket never moves backwards. If the clock
  steps back, new calls go into the newest bucket already seen; if it jumps
  forward by more than the history window, all old buckets expire at once.

The statistics live in shared memory only: they are lost on a server restart
or crash.

## Eviction

When a new combination arrives and the table already holds `max_entries`
entries, an eviction pass runs:

1. All **dead** entries (all buckets expired) are reclaimed.
2. If that frees fewer than 5% of `max_entries`, live entries are evicted
   until 5% is free: least recently written first, then lowest usage (a call
   count that decays on each pass, as in pgss).

Dead entries are not freed when they expire. They stay allocated (and are
counted in `entries`) until the table fills up and a pass reclaims them. On a
system whose tag combinations change over time, `entries` therefore normally
climbs to `max_entries` and stays there, and passes run regularly even when
no live history is lost.

`pg_stat_statement_context_info()` reports:

- `dealloc`: the number of eviction passes;
- `evicted_entries`: the entries those passes removed, **dead or live
  combined**;
- `entries`: allocated entries, including dead ones not yet reclaimed.

None of these counters separates the cleanup of expired entries from the
eviction of live ones. A rising `evicted_entries` is therefore not by itself a
sign of undersizing. For example, if 100 combinations expire, the next new
combination triggers a pass that adds 100 to `evicted_entries`, yet no
visible history is lost.

In v1, `_info()` cannot tell you for certain whether live entries are being
evicted. As a capacity-pressure indicator, count the live entries.
`pg_stat_statement_context_totals` has exactly one row per live entry,
visible to every role:

```sql
SELECT (SELECT count(*) FROM pg_stat_statement_context_totals) AS live_entries,
       max_entries, entries, dealloc, evicted_entries, oldest_bucket
  FROM pg_stat_statement_context_info();
```

- If `live_entries` stays well below `max_entries` (below about 95%), passes
  only reclaim expired entries and nothing visible is lost.
- If `live_entries` stays near `max_entries` while `dealloc` keeps rising, the
  table is under pressure. Passes *may* be evicting live entries, but a
  workload that keeps the table full of live entries while a steady 5% expire
  produces the same signals with no history lost. Treat it as a reason to
  raise `max_entries`, or to look for a tag with unexpectedly high cardinality
  (see [Cardinality](extractors.md#allowlist-denylist-and-cardinality)), not
  as proof of loss.
- Live eviction removes the least recently written entries first, so it
  mostly loses the history of rare combinations.

An eviction pass scans the whole table once (choosing the live victims by
partial selection rather than sorting every entry), which costs time while the
table is full. In the rare case that a pass can free nothing at all, the call is
dropped rather than failing the statement.
