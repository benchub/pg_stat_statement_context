# Configuration

All configuration uses GUCs (server settings). There is no separate configuration file. Settings can be put in `postgresql.conf` or set with `ALTER SYSTEM` (on a managed service, in the provider's parameter groups: see [Managed services](managed-services.md)). What else is possible, and when a change takes effect, depends on each GUC's [context](#guc-contexts).

All of them are visible in `pg_settings`:

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
| [`bucket_count`](#bucket_count) | integer | `12` | 1 – 10000 | postmaster |
| [`bucket_interval`](#bucket_interval) | integer (seconds) | `300` (`5min`) | 1s – 1 day | postmaster |
| [`cardinality_cap`](#cardinality_cap) | integer | `0` (off) | 0 – 1000000 | sighup |
| [`cardinality_cap_overrides`](#cardinality_cap_overrides) | string | `''` | `key:N` list | sighup |
| [`cardinality_cap_scope`](#cardinality_cap_scope) | enum | `role` | `role`, `database`, `server` | postmaster |
| [`cardinality_cap_slots`](#cardinality_cap_slots) | integer | `16384` | 256 – 67108864 | postmaster |
| [`enabled`](#enabled) | bool | `on` | | superuser |
| [`exclude_tags`](#exclude_tags) | string | `'traceparent, tracestate, request_id'` | key list | superuser |
| [`exemplar_keys`](#exemplar_keys) | string | `''` (off) | key list, at most 8 | postmaster |
| [`exemplar_memory`](#exemplar_memory) | integer (kB) | `2048` (`2MB`) | 0 – 2147483647 kB | postmaster |
| [`extractors`](#extractors) | string | `'sqlcommenter, marginalia'` | extractor DSL | sighup |
| [`max_entries`](#max_entries) | integer | `10000` | 100 – 1073741823 | postmaster |
| [`max_tag_value_len`](#max_tag_value_len) | integer (bytes) | `64` | 1 – 4096 | postmaster |
| [`max_tags`](#max_tags) | integer | `8` | 1 – 64 | postmaster |
| [`max_tagset_bytes`](#max_tagset_bytes) | integer (bytes) | `512` | 128 – 8192 | postmaster |
| [`nested_tags`](#nested_tags) | enum | `inherit` | `inherit`, `scan`, `none` | superuser |
| [`normalize`](#normalize) | string | `''` | rule list | sighup |
| [`reclaim_worker`](#reclaim_worker) | bool | `off` | | postmaster |
| [`reclaim_worker_interval`](#reclaim_worker_interval) | integer (ms) | `10000` (`10s`) | 100 ms – 1 day | sighup |
| [`save`](#save) | bool | `on` | | sighup |
| [`scan_window`](#scan_window) | integer (bytes) | `2048` (`2kB`) | 64 B – 1 MB | superuser |
| [`tags`](#tags) | string | `'action, controller, job'` | key list or `'*'` | superuser |
| [`tags_override`](#tags_override) | string | `''` | `key='value'` pairs | user |
| [`track`](#track) | enum | `top` | `none`, `top`, `all` | superuser |
| [`track_utility`](#track_utility) | bool | `on` | | superuser |
| [`untagged`](#untagged) | enum | `skip` | `skip`, `record` | superuser |

Every name has the prefix `pg_stat_statement_context.`.

### GUC contexts

The context (the last column above, and `pg_settings.context`) says how a change is applied and who may make it:

- **postmaster**: these size shared memory or fix the bucket layout. Set in `postgresql.conf` or with `ALTER SYSTEM`; a change needs a server restart.
- **sighup**: server-wide. Set in `postgresql.conf` or with `ALTER SYSTEM`, and applied on reload (`SELECT pg_reload_conf();` or `pg_ctl reload`). They can't be set per role, per database or per session: PostgreSQL rejects `SET`, `ALTER ROLE ... SET` and `ALTER DATABASE ... SET` with "cannot be changed now".
- **superuser**: set like `sighup` settings, and also per session (`SET`), per role or per database (`ALTER ROLE/DATABASE ... SET`), but only by a superuser or (PostgreSQL 15+) a role granted `SET` on the parameter. This is why `tags`, `exclude_tags`, `untagged` and `scan_window` can differ between databases and roles (see [Changing the configuration from SQL](#changing-the-configuration-from-sql)).
- **user**: any user can change it, with `SET`, `SET LOCAL`, a function's `SET` clause or connection options, as well as per role or per database.

What each context means on a managed service, where the administrator is not a superuser, is in [Managed services](managed-services.md#settings).

### `bucket_count`

The number of time buckets kept for each entry. History length is `bucket_count × bucket_interval` (1 hour by default). See [Time buckets](#time-buckets).

### `bucket_interval`

The width of each time bucket. It accepts time units, for example `'1min'` or `'1h'`; a bare number is in seconds. It is fixed at startup so that all backends agree on bucket boundaries.

### `cardinality_cap`

The maximum number of distinct values of each tag key; `0` (the default) means no cap. Overridden per key by [`cardinality_cap_overrides`](#cardinality_cap_overrides). It is a last line of defense against a key whose values explode, from an unnormalized route, a bug, or a client sending random values (see [cardinality](extractors.md#allowlist-denylist-and-cardinality)):

```
pg_stat_statement_context.cardinality_cap = 100
```

- **Counted per role and database, per key** (by default; see [`cardinality_cap_scope`](#cardinality_cap_scope)). Values are counted across all queries and buckets, separately for each (role, database), as pg_stat_statements keys its entries. A key's first N distinct values (in the order statements bring them) are **admitted**; any other value is recorded as JSON `null`, so all its statements share one entry per query and other tags. Admitted values stay admitted: a value isn't forgotten when its entries are evicted, only by [`pg_stat_statement_context_reset()`](sql-interface.md#pg_stat_statement_context_reset) or a restart. Lowering the cap, or setting it to `0` and back, doesn't remove values already admitted.
- **Where it applies**: step 8 of the [tag pipeline](extractors.md#the-tag-pipeline), after `rename`, [`normalize`](#normalize) and truncation, to tags from every source (comments, `application_name`, [`tags_override`](#tags_override)), and only to tags that are then kept within `max_tags` and `max_tagset_bytes`. Values are compared byte for byte; the cap is per **final** (renamed) key.
- **`null` is unambiguous**: a client can only send strings (`'null'` and `''` stay strings), so `tags->'route' = 'null'` finds exactly the collapsed statements; `tags->>'route'` is SQL `NULL` for them.
- **Counters**: each collapsed value is counted in `pg_stat_statement_context_info().capped_tags`; [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract) shows what would collapse without admitting anything.
- **When**: a value is admitted when its tags are extracted, at `ExecutorStart`, so it takes its place even if the statement then fails. `ExecutorStart` also runs for statements that are never executed or recorded: a portal that is bound but never executed (extended protocol `Bind` without `Execute`) and a plain `EXPLAIN` (without `ANALYZE`). Their values use cap space too. Under concurrency a key can very rarely collapse one value too many while two sessions admit its last values at the same time; it never exceeds its cap. While a `pg_stat_statement_context_reset()` clears the whole table (needed about once every million resets, when its generation number wraps around), new values collapse to `null` for that moment.
- **Values, not combinations**: each key is capped on its own, so with k kept keys capped at N values, one query of one role can still have N^k entries without a single cap event: (N + 1)^k with `null`, and (N + 2)^k − 1 when keys are optional (absent is one more state). These bounds assume the caps have been enforced at N since the last `pg_stat_statement_context_reset()`, or since a restart that loaded no saved statistics ([`save`](#save)). Values admitted under a higher cap, or before the cap was set to `0` and back, still count. So do the values of live entries collected while a key was uncapped or restored at startup, until those entries expire or are evicted. Size `max_entries` for the combinations you observe; see [Caps bound values, not combinations](extractors.md#caps-bound-values-not-combinations) and [Watching for cardinality pressure](extractors.md#watching-for-cardinality-pressure).
- **Cost**: the check is a lock-free lookup in a shared hash table (a few atomic reads, plus a compare-and-swap for a new value), with no lock taken; see [benchmarks](benchmarks.md#cardinality-caps). With no cap configured, nothing is checked.

### `cardinality_cap_overrides`

Per-key caps that take precedence over [`cardinality_cap`](#cardinality_cap): a comma-separated list of `key:N` entries. `N = 0` exempts the key from the default cap. Empty by default.

```
pg_stat_statement_context.cardinality_cap = 100
pg_stat_statement_context.cardinality_cap_overrides = 'route:500, job:0'
```

Whitespace around entries, keys and numbers is ignored. The key is everything before the entry's last `:` (so `a:b:3` caps the key `a:b`), at most 63 bytes, without whitespace or `*`; it is matched exactly against the final (renamed) key. `N` is 0 – 1000000. A malformed list (an entry without `:N`, an empty entry, a key listed twice, more than 1024 entries) is rejected and the previous value stays in effect. Overrides work without a default cap (`cardinality_cap = 0`): then only the listed keys are capped.

### `cardinality_cap_scope`

What the [cardinality caps](#cardinality_cap) count distinct values per. Takes effect at server start.

- **`role`** (the default): per (role, database), the key of pg_stat_statements entries. Each role has its own cap for each key in each database, and its own set of admitted values. The role is the one the entry is recorded under (see below).
- **`database`**: per database, shared by all roles in it.
- **`server`**: one cap per key for the whole server.

A key's cap is the same number in every scope (`cardinality_cap = 100` under `role` allows 100 values per role and database).

`database` and `server` share caps between roles, which has two security consequences:

- **Membership oracle.** Once a key is at its cap, a value that another role (or, under `server`, another database) already sent stays a string, while a new one becomes `null`. Any role that can read its own rows can therefore send a candidate value and learn whether someone else sent it. Under `server` a role can also probe a value without filling the key's cap: slot positions follow from the value alone, so it can fill the slots after the candidate's with values of its own and see whether one more value still finds room.
- **Poisoning.** Any role can use up a key's cap, so every other role's new values become `null` until a `pg_stat_statement_context_reset()`.

Use them only when all the roles sharing a scope trust each other; keep `role` on multi-tenant and managed services (see [Cap scope and trust](extractors.md#cap-scope-and-trust)).

Under `role` (and between databases under `database`), the caps and admitted values of other scopes don't affect a role's values, and the positions of values in the shared table are keyed with a random secret drawn at startup and by every `pg_stat_statement_context_reset()`, so a role can't aim its own values at another scope's. What remains shared is the table's size: all scopes share the one table sized by [`cardinality_cap_slots`](#cardinality_cap_slots), and narrower scopes admit more distinct (key, value) pairs in total and use more key slots. When the table is (nearly) full, new values of every scope become `null`, which any role can observe; like `pg_stat_statements.max`, it's a shared limit, so size it for all roles and databases.

Tags are capped when they are extracted, for the current user. When they end up recorded or shown under another role, the caps are applied again for that role first, so every row obeys the caps of the role it belongs to:

- a cursor opened under one role and run to completion under another (`SET ROLE` in between), or opened by a `SECURITY DEFINER` function and fetched or closed by its caller, is recorded under the role that finishes it (as in pg_stat_statements), with that role's caps;
- statements that inherit the caller's tags ([`nested_tags = inherit`](#nested_tags)) inside a `SECURITY DEFINER` function are recorded under the definer, with the definer's caps;
- a tagged `SET ROLE` (with [`track_utility`](#track_utility)) is recorded under the new role, with its caps, and so is the [activity view](sql-interface.md#pg_stat_statement_context_activity) row of a portal run after a `SET ROLE`.

Applying the caps again admits the values kept as strings in that role's scope and counts collapses in `capped_tags` again. It uses the cap settings in effect at that moment. This costs nothing under `database` and `server` or while caps are off, and little memory otherwise (developer reference: DESIGN.md §6.1 "Identity").

### `cardinality_cap_slots`

The number of distinct (key, value) pairs the caps can track, server-wide, in a shared table allocated at startup (8 bytes per slot, plus 16 bytes per key slot, one key slot per 16 value slots and at least 64; 147,488 bytes at the default; see [Shared memory sizing](#shared-memory-sizing)). It is allocated even while no cap is set, so caps can be turned on with a reload. This memory is not part of `pg_stat_statement_context_info().shmem_bytes`; `pg_stat_statement_context_info().cap_shmem_bytes` reports its exact size (about 576 MiB at the maximum of 2^26 slots).

Size it above the sum of the caps of the keys you expect, with headroom (the table is an open-addressing hash table that slows down and fills early when nearly full). Under the default [`cardinality_cap_scope = role`](#cardinality_cap_scope), each (role, database) that sends a key takes up to that key's cap of slots and one key slot of its own, so on a server with many roles or databases, size it for the sum over all of them. When a new value finds no room (or a new key finds no key slot), the value is recorded as `null`, as if over its cap, and counted in both `pg_stat_statement_context_info().capped_tags` and `pg_stat_statement_context_info().cap_table_full`: the caps fail closed. `pg_stat_statement_context_reset()` empties the table.

### `enabled`

Master switch. When `off`, the hooks do nothing: no tags are extracted and nothing is recorded. Existing statistics are kept, and `pg_stat_statement_context_extract()` still works.

### `exclude_tags`

The denylist: tag keys to discard. It applies **only when `tags = '*'`**. Same syntax as `tags`, without `'*'`. The default lists well-known high-cardinality keys. Like `tags`, it can be set per database, per role or per session by a superuser.

### `exemplar_keys`

Keys whose most recent value is stored with each entry as an **exemplar**, shown in the `exemplars` column of the views, e.g. `'traceparent'`: from an expensive aggregate you can jump to one real trace without grouping by the trace id. A comma-separated list of at most 8 keys (at most 63 bytes each, no whitespace or `*`, case-sensitive; duplicates are ignored). Empty (the default) turns exemplars off and uses no memory.

A key is matched after [`rename`](extractors.md#the-tag-pipeline), before [`tags`](#tags) and [`exclude_tags`](#exclude_tags), and its value is captured whether or not the key is then kept as a tag. So the usual setup is a key that is in both `exclude_tags` (not grouped by) and `exemplar_keys`:

```
pg_stat_statement_context.exclude_tags = 'traceparent, tracestate, request_id'
pg_stat_statement_context.exemplar_keys = 'traceparent'
```

The value stored is the raw, validated value: [`normalize`](#normalize), [`max_tag_value_len`](#max_tag_value_len) and the cardinality caps do not apply to it. An extractor's own `keys` list does. A statement with no value for a key leaves the entry's previous value in place, so `exemplars` shows the latest value seen, not necessarily one from the latest call. Nested statements with `nested_tags = inherit` use the outer statement's exemplars. Exemplars are visible to the same roles as `tags`, are not saved across restarts, and are removed by `pg_stat_statement_context_reset()`.

### `exemplar_memory`

Shared memory for the exemplar values of all entries, allocated at startup when `exemplar_keys` is not empty. It is split evenly: each entry gets `exemplar_memory / max_entries` bytes (rounded down to a multiple of 8), each key an equal share of that, and each value that share minus 2 bytes, at most 256 bytes. `pg_stat_statement_context_info().exemplar_value_bytes` shows the result and `pg_stat_statement_context_info().exemplar_shmem_bytes` the memory used (included in `shmem_bytes`, never more than `exemplar_memory`; see [Shared memory sizing](#shared-memory-sizing)).

A value longer than `exemplar_value_bytes` is **dropped**, not truncated (a truncated trace id identifies nothing): the entry keeps its previous value and `pg_stat_statement_context_info().exemplar_values_dropped` is incremented. At the defaults (10000 entries, 2 MB) one key gets 206 bytes and two keys 102 bytes each, enough for a 55-byte `traceparent`.

### `extractors`

The extractor list, which says which comment formats are parsed and where in the statement to look, and whether tags are also derived from `application_name` ([`appname`](extractors.md#appname)). See [the extractor DSL](extractors.md#the-extractor-dsl). A malformed value is rejected when it is set or reloaded, and the previous value stays in effect.

### `max_entries`

The maximum number of entries. An entry is one {database, user, `queryid`, `toplevel`, tag set}. See [Sizing](#sizing-max_entries) and [Eviction](#eviction).

### `max_tag_value_len`

The maximum length of a tag value, in bytes. Longer values are truncated on a character boundary. Tag *keys* are always limited to 63 bytes; a longer key is dropped.

### `max_tags`

The maximum number of tags stored per entry. It also limits the number of capture groups in a `regex` extractor's pattern. Tags beyond the limit are dropped (in priority order, see [the tag pipeline](extractors.md#the-tag-pipeline)) and counted in `pg_stat_statement_context_info().dropped_tags`.

### `max_tagset_bytes`

A hard cap on the size of an entry's serialized tag set, which is part of the hash key. The size of a tag set is the sum of `length(key) + 1 + length(value) + 1` bytes over its tags (a value [capped](#cardinality_cap) to `null` counts as 2 bytes). Tags are kept greedily in priority order (`tags` list order, or sorted key order with `tags = '*'`). A tag that doesn't fit is dropped and counted in `pg_stat_statement_context_info().dropped_tags`, and smaller lower-priority tags may still be kept. This setting is the main factor in the size of an entry.

Size it comfortably above the largest tag set you expect: the sum over the allowlisted keys of `length(key) + max_tag_value_len + 2`. The limit is applied after the extractor chain has picked its winner. A tag set that doesn't fit is therefore not replaced by another extractor's tags, and the statement can end up untagged (see [chain semantics](extractors.md#the-extractor-dsl)).

### `nested_tags`

Which tags nested statements get:

- `inherit` (default): the tags of the statement that is running them (the "active" statement, ultimately the top-level one). A query inside a trigger is then attributed to the controller whose `UPDATE` fired the trigger.
- `scan`: tags from the nested statement's own source text (for example a comment inside the function body), plus [`appname`](extractors.md#appname) tags from `application_name` and the [`tags_override`](#tags_override) tags as they are when the nested statement starts (so a function's `SET pg_stat_statement_context.tags_override = ...` clause applies to the statements it runs).
- `none`: no tags. With `untagged = skip`, nested statements are then not recorded at all.

This matters only for nested statements that are recorded, i.e. with `track = all`. Statements run while a parent statement is being *planned* (such as constant-folded function calls) have no active statement, so they always get only their own tags.

### `normalize`

Regex-replace rules that rewrite tag values, so that values with variable parts (IDs, UUIDs, dates) share one entry. Empty by default (no rules). A comma-separated list of rules, each

```
key: 'pattern' => 'replacement'
```

- `key` is the **final** tag key: rules run after `rename` and after the `tags` allowlist / `exclude_tags` denylist, so they see renamed keys and only tags that are kept (a rule for a key that isn't kept does nothing). It may be written bare or in single quotes; it follows the rules of `tags` keys.
- `pattern` and `replacement` are always in single quotes; write a quote as `''`. Commas, `:` and `=>` inside quotes need no escaping.
- Each rule replaces **every** match, exactly like `regexp_replace(value COLLATE "C", 'pattern', 'replacement', 'g')`: the same engine (PostgreSQL advanced regular expressions) and flags as the [`regex` extractor](extractors.md#regex), and no back-references in the pattern. Flags can be embedded, e.g. `'(?i)^job-'`. In the replacement, `\1` to `\9` insert a capture group, `\&` the whole match and `\\` a backslash; any other escape is rejected.
- Several rules for the same key run in order, each on the previous one's output. A rule may produce an empty value; the tag is kept.
- The result is then truncated to `max_tag_value_len` bytes, so a rule sees the whole value (even one longer than `max_tag_value_len`), and a value is truncated only once, after all rules.
- Limits: at most 32 rules; a pattern is 1 to 1024 bytes and a replacement at most 1024 bytes. A rule's output is cut at `max(value length, max_tag_value_len)` bytes, which also stops the matching there.

For example, in `postgresql.conf`, where quotes are doubled and backslashes must be doubled too (the file's own string escaping turns `\\` into `\`):

```
pg_stat_statement_context.normalize = 'route: ''/users/\\d+'' => ''/users/:id'', route: ''/posts/\\d+'' => ''/posts/:id'''
```

or from SQL:

```sql
ALTER SYSTEM SET pg_stat_statement_context.normalize =
  $$route: '/\d+' => '/:id', job: '^(\w+)Job$' => '\1'$$;
SELECT pg_reload_conf();
```

Rules are compiled and fully validated when the value is set or reloaded; an invalid rule (syntax, bad pattern, back-reference, reference to a missing capture group, unknown escape, over a limit, a pattern that takes longer than 100 ms to compile) rejects the whole value and the previous one stays in effect. Test rules with [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract), which shows the normalized tags and counts the changed values in `normalized_tags`.

Rules run on every tagged statement, under the same safety limits as regex extractors: each backend compiles a rule once (at first use after a configuration change), with the same [100 ms compile time limit](extractors.md#regex), and the engine's own complexity limits and query cancellation apply. If a rule fails at run time (the regex engine runs out of memory or reports an error), the tag is **dropped** rather than stored unnormalized, which could create many entries; this is counted in the debug function's `normalize_failures`. If a rule fails to compile in a backend at run time (including a compile stopped at the time limit), it is disabled there until the next `extractors` or `normalize` change (counted in `pg_stat_statement_context_info().regex_compile_failures`), and tags of its key are dropped. The user's statement does not fail because of a rule (an unexpected internal error in the regex engine is not hidden).

### `reclaim_worker`

Starts a background worker that frees dead entries (all buckets expired) without waiting for the table to fill up; see [Eviction](#eviction). Off by default, in which case no worker is registered at all. The worker needs no database connection, so it does not appear in `pg_stat_activity`; its process title is `pg_stat_statement_context reclaim worker`, and it uses one of `max_worker_processes`. It is not needed for correct results: expired buckets are never shown either way.

### `reclaim_worker_interval`

How often the reclaim worker wakes up. It accepts time units, for example `'1s'` or `'1min'`; a bare number is in milliseconds. A wake-up that finds the current bucket unchanged since the last one does nothing, so dead entries are freed at most `reclaim_worker_interval` after their last bucket expires, and the worker takes the table's exclusive lock at most once per `bucket_interval`. Ignored when `reclaim_worker` is off.

### `save`

Keeps the statistics across clean restarts, like `pg_stat_statements.save`. After a smart or fast shutdown the postmaster writes the store to `$PGDATA/pg_stat/pg_stat_statement_context.stat`; the next start loads it and removes the file. Entries, their buckets, the counters, `stats_reset` and the bucket times are kept; buckets that expired while the server was down are dropped. Exemplars are not saved.

The file is not used (each case is logged, and the server starts with an empty store) when it was written by another file format, PostgreSQL major version or extension version; when `bucket_interval` or `bucket_count` changed; when it is corrupt; or when `save` is off at startup. A smaller `max_entries` evicts the excess in [eviction](#eviction) order, and tag sets that no longer fit `max_tagset_bytes` are skipped. Nothing is saved after an immediate shutdown or a crash, and the file is removed at every start, so statistics from before a crash are never loaded.

### `scan_window`

How many bytes at the start or end of a long statement are searched for comments. Statements no longer than `scan_window` are always lexed exactly. It also bounds the total comment bytes examined per statement. See [Where comments are found](extractors.md#where-comments-are-found). It accepts byte units, e.g. `'4kB'`. A superuser can set it per database, per role or per session.

**Cost on long statements.** With the default `position = append`, a long statement costs one scan of its last `scan_window` bytes, whatever its length; if the client sends it without a trailing `;`, PostgreSQL reports no statement length, so a `strlen` over the whole text comes first. `position = any`, and a `scan_window` that covers the whole statement, lex every byte, so their cost grows with the statement's length. Keep `append` (or `prepend`) and the default window for workloads with long statements such as large `IN` lists, unless comments can sit in the middle of statements. There is no separate byte budget. The measured costs on a 59 kB statement and more tuning advice are in [Long statements: scan costs and tuning](benchmarks.md#long-statements-scan-costs-and-tuning).

### `tags`

The allowlist: a comma-separated list of tag keys to keep. It is applied **after** `rename`, so it must name the renamed key. Keys are case-sensitive, at most 63 bytes, and may not contain whitespace or `*`; at most 1024 entries. Tags whose key isn't listed are discarded. The list order is the tag priority used by `max_tags` and `max_tagset_bytes`. An empty list keeps no tags at all.

`'*'` (alone) keeps every tag except those in `exclude_tags`. That is not recommended; see [Cardinality](extractors.md#allowlist-denylist-and-cardinality).

A superuser can set it per database, per role or per session (`ALTER DATABASE ... SET`, `ALTER ROLE ... SET`, `SET`), so each database can keep its own keys. A function's `SET` clause applies it to the statements the function runs, when they get their tags from their own text ([`nested_tags = scan`](#nested_tags)); inherited tags were already filtered by the setting in effect when the outer statement started.

### `tags_override`

Tags for the current session or transaction, for clients that cannot add comments (some drivers and ORMs) and for prepared statements, whose text is fixed when they are prepared. Empty by default (no tags). Any user can set it:

```sql
BEGIN;
SET LOCAL pg_stat_statement_context.tags_override = 'controller=''users'',action=''show''';
SELECT ...;      -- tagged controller=users, action=show
COMMIT;          -- the override ends with the transaction
```

- **Syntax**: the [sqlcommenter](extractors.md#sqlcommenter) format, `key='value',key2='value2'`, with URL-encoded keys and values (`%20` or `+` for a space, `%27` for a quote, `%2C` for a comma). Whitespace around pairs is ignored; an empty or blank value means no override. The value is parsed once, when it is set, and **rejected** if it is malformed (an unquoted value, text that is not a pair, an unterminated quote), has an invalid `%`-escape, decodes to a NUL byte or to text invalid in the database encoding, or has a key longer than 63 bytes; the previous value then stays in effect:

  ```sql
  SET pg_stat_statement_context.tags_override = 'a=''%zz''';
  -- ERROR:  invalid value for parameter "pg_stat_statement_context.tags_override": "a='%zz'"
  -- DETAIL:  Pair 1 has an invalid %-escape.
  ```

- **Merging**: the override's tags are added to the tags of each statement (from its comments, and from `application_name` with [`appname`](extractors.md#appname) extractors). On a key conflict the **override wins**: override > comment > `application_name`. Within the override, the first occurrence of a key wins. The override does not stop the search for a [trailing footer](extractors.md#where-comments-are-found) when the statement has no comment of its own.
- **Pipeline**: the override's pairs go through the same [pipeline](extractors.md#the-tag-pipeline) as comment tags, except the per-extractor `keys` lists (there is no extractor): `rename`, the `tags` allowlist / `exclude_tags` denylist, `normalize`, truncation to `max_tag_value_len`, and the `max_tags` / `max_tagset_bytes` limits apply, with the same counters (`invalid_tags`, `dropped_tags`, ...). `rename` is per extractor, so the override uses the `rename` lists of the **comment `sqlcommenter` extractors**, in configuration order (the first rule whose source key matches); the `rename` lists of `marginalia`, `regex` and `appname` extractors are not used, and without a `sqlcommenter` extractor keys are not renamed.
- **When it is read**: when a statement starts executing, like `application_name` for `appname` extractors. A prepared statement (`PREPARE`/`EXECUTE`, or the extended protocol's Parse/Bind/Execute) uses the override in effect when it is executed, not when it was prepared. A `SET ... tags_override` statement itself is tagged with the previous value.
- **Nested statements** follow [`nested_tags`](#nested_tags): with `inherit` they get the tags of the top-level statement (override included, as it was when that statement started); with `scan` they read the override again when they start, so a function's `SET` clause or a `SET LOCAL` / `set_config()` inside a function applies to the statements after it; with `none` they get no tags. A `SET LOCAL` inside a function also lasts for the rest of the transaction, like any `SET LOCAL`.
- With `untagged = skip`, a statement whose only tags come from the override is tagged, and recorded.
- [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract) uses the session's current override.

An override can also be set per role or database (`ALTER ROLE ... SET`), in a driver's connection options (e.g. `options=-c pg_stat_statement_context.tags_override=job='nightly'`), or in `postgresql.conf`. Values set per role, per database or in the file are checked against the database encoding only when a statement uses them; a pair invalid there is dropped and counted in `invalid_tags`, like a comment's.

Like comments, the override is supplied by the client: any user can attribute their statements to any tags, so tags are not a security boundary.

### `track`

Which statements are recorded, as in `pg_stat_statements.track`:

- `none`: none.
- `top` (default): only top-level statements, i.e. the ones the client sent. A plan run by SQL `EXECUTE` counts as top-level.
- `all`: also nested statements, such as queries inside PL/pgSQL functions, procedures, `DO` blocks and triggers. Their rows have `toplevel = false`. See [Nested statements](sql-interface.md#nested-statements-toplevel-and-inclusive-costs) before summing them.

### `track_utility`

Whether utility statements (DDL, `VACUUM`, `CALL`, `DO`, `COPY`, ...) are recorded. `EXECUTE` and `PREPARE` are never recorded as utilities, because the executor records the plan they run. `DEALLOCATE` is recorded only on PostgreSQL 17 and later, matching `pg_stat_statements`.

### `untagged`

What to do with statements that end up with no tags:

- `skip` (default): don't record them, so untagged traffic doesn't use entries.
- `record`: record them with an empty tag set (`tags = {}`).

A superuser can set it per database, per role or per session, e.g. `ALTER DATABASE canvas SET pg_stat_statement_context.untagged = 'record'` to record the untagged statements of one database only.

## Changing the configuration from SQL

The `sighup` and `superuser` settings, including `extractors`, `tags`, `exclude_tags` and `normalize`, can be changed server-wide without a restart:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'sqlcommenter, marginalia(position=prepend)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'application, controller, action, job';
SELECT pg_reload_conf();
```

`pg_reload_conf()` only sends the reload signal. Every session, including the one that called it, applies the new values shortly afterwards, before it runs its next command. Interactively that is immediate in practice, but a script that changes the configuration and immediately tests it should pause first, for example with `SELECT pg_sleep(0.5);`, and can check the value with `SHOW`.

The value is fully validated (including regex compilation) when it is set, so a bad value fails immediately:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'nonsense';
-- ERROR:  invalid value for parameter "pg_stat_statement_context.extractors": "nonsense"
-- DETAIL:  Unknown extractor "nonsense".
```

If a bad value reaches `postgresql.conf` directly, the reload logs the error and keeps the previous configuration. To go back to the defaults:

```sql
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
SELECT pg_reload_conf();
```

The `superuser` settings can also be changed for one session, for example to record nested statements while investigating:

```sql
SET pg_stat_statement_context.track = 'all';
```

or per database or per role. The new value applies to sessions that start afterwards:

```sql
ALTER DATABASE canvas SET pg_stat_statement_context.untagged = 'record';
ALTER DATABASE billing SET pg_stat_statement_context.tags = 'controller, action, tenant';
ALTER ROLE batch SET pg_stat_statement_context.tags = 'job';
```

Entries are keyed by database and role, so different values don't mix in one entry. On PostgreSQL 15 and later, a superuser can let another role set one of them with `GRANT SET ON PARAMETER pg_stat_statement_context.tags TO ...`.

## Sizing `max_entries`

`max_entries` counts **(query × context) combinations**, independent of `bucket_count`. Each entry holds its own ring of `bucket_count` counter slots, so 5,000 recurring combinations need 5,000 entries whether they are active in one bucket or in all of them. A combination keeps its entry while any of its buckets is live; once all of its buckets have expired the entry is *dead* and is the first to be reclaimed when space is needed.

Size it to at least the number of distinct (database × user × `queryid` × `toplevel` × tag set) combinations seen within one history window (`bucket_count × bucket_interval`), plus headroom. For example, 400 query fingerprints, each run from about 10 controller/action pairs by one application user, need about 4,000 entries. [Cardinality caps](#cardinality_cap) don't bound this number: they limit each key's values, not the combinations of keys (see [Caps bound values, not combinations](extractors.md#caps-bound-values-not-combinations)).

Every entry is preallocated in shared memory at startup. With the defaults an entry takes 872 bytes (the tag set, `max_tagset_bytes`, dominates; each bucket adds 24 bytes, and the entry's own counters 48 bytes), plus about 54 bytes of hash-table and eviction overhead, so 10,000 entries use about 8.8 MiB; see [Shared memory sizing](#shared-memory-sizing).

## Shared memory sizing

All of the extension's shared memory is requested once, at server start, in three parts. It comes on top of `shared_buffers` and PostgreSQL's own structures (on PostgreSQL 15 and later, `SHOW shared_memory_size` shows the server's total, extension included), and changing any of the settings below needs a restart.

| Part | Reported by | Settings that size it |
|---|---|---|
| Statistics store: the entries, their hash table and the eviction array, including the exemplars | `pg_stat_statement_context_info().shmem_bytes` (exact) | `max_entries`, `max_tagset_bytes`, `bucket_count`, `exemplar_keys`, `exemplar_memory` |
| Cardinality-cap table | `pg_stat_statement_context_info().cap_shmem_bytes` (exact) | `cardinality_cap_slots` |
| Activity slots ([`pg_stat_statement_context_activity`](sql-interface.md#pg_stat_statement_context_activity)) | `pg_shmem_allocations` (below) | `MaxBackends`, `max_tagset_bytes` |

`pg_stat_statement_context_info().exemplar_shmem_bytes` is part of `shmem_bytes`, not an addition to it. The activity slots are not in `pg_stat_statement_context_info()`; a superuser (or a member of `pg_read_all_stats`) can read their exact size, and that of the other named parts, with:

```sql
SELECT name, size FROM pg_shmem_allocations WHERE name LIKE 'pg_stat_statement_context%';
```

The total is `shmem_bytes + cap_shmem_bytes + activity`. (The hash table's elements are allocated anonymously, so `pg_shmem_allocations` alone does not add up to `shmem_bytes`.)

### Formula

In bytes, on 64-bit platforms, where `align8(x)` rounds `x` up to a multiple of 8 and `pow2(x)` is the smallest power of two ≥ `x`:

```
entry      = align8(24 + max_tagset_bytes) + 48 + 24 × bucket_count + exemplar
element    = entry + 16                         (dynahash element header)
group      = floor(A / element), A the smallest power of two ≥ 256 with floor(A / element) ≥ 32
buckets    = pow2(max_entries)
segments   = max(1, buckets / 256)
store      = align8(240 + 24 × max_entries)     (header and eviction array)
           + 848                                (dynahash header)
           + 8 × max(256, segments)             (directory)
           + 2048 × segments                    (bucket array, 8 bytes per bucket)
           + ceil(max_entries / group) × group × element
cap        = 32 + 8 × (cardinality_cap_slots + 2 × max(64, floor(cardinality_cap_slots / 16)))
activity   = 16 + MaxBackends × align8(36 + max_tagset_bytes)
```

- **Exemplars** (0 when `exemplar_keys` is empty): `share = floor(exemplar_memory × 1024 / max_entries)` rounded down to a multiple of 8, `value = min(floor(share / nkeys) − 2, 256)` for `nkeys` exemplar keys, and `exemplar = align8(nkeys × (2 + value))` (0 when `value` would be below 1). `pg_stat_statement_context_info().exemplar_value_bytes` is `value` and `pg_stat_statement_context_info().exemplar_shmem_bytes` is `max_entries × exemplar`.
- **MaxBackends** is `max_connections + autovacuum_max_workers + max_worker_processes + max_wal_senders + 1` on PostgreSQL 14–16, `+ 2` on 17 (the slot sync worker), and on 18 `autovacuum_worker_slots` (default 16) replaces `autovacuum_max_workers`: 122, 123 and 136 at the defaults.
- **Rounding.** `cap` and `activity` are exact, and so is `exemplar_shmem_bytes`. The 240- and 848-byte headers can change by a few bytes between versions, so `store` is exact to within 256 bytes. `test/t/034_shmem_sizing.pl` checks all of this against the server.
- **Rule of thumb.** Each entry costs about `align8(24 + max_tagset_bytes) + 88 + 24 × bucket_count + exemplar` bytes, plus 8 per hash bucket (`pow2(max_entries)` of them), plus about 3 kB. So `max_tagset_bytes` dominates at the default 12 buckets, and `bucket_count` dominates for long histories (288 buckets take 6,912 bytes per entry).

`scripts/shmem-sizing.pl name=value ...` (e.g. `scripts/shmem-sizing.pl max_entries=50000 bucket_count=288 max_connections=500 pg_version=17`) evaluates the formula for other settings; it computes MaxBackends from `max_connections`, `autovacuum_workers`, `max_worker_processes`, `max_wal_senders` and `pg_version` (default 18), or takes `max_backends=N` directly.

### Totals for representative settings

Generated by `scripts/shmem-sizing.pl` (MaxBackends 136, the PostgreSQL 18 default at `max_connections = 100`; activity is 7,176 bytes less on 17 and 7,728 bytes less on 14–16):

<!-- shmem-sizing-table:begin (scripts/shmem-sizing.pl --update) -->
| Settings (others at their defaults) | Store (`shmem_bytes`) | of which exemplars | Cap table (`cap_shmem_bytes`) | Activity | Total |
|---|---:|---:|---:|---:|---:|
| Defaults | 9,261,312 (8.8 MiB) | 0 | 147,488 (144 KiB) | 75,088 (73 KiB) | 9,483,888 (9.0 MiB) |
| 24 h of history: `bucket_count = 288` | 75,719,568 (72.2 MiB) | 0 | 147,488 (144 KiB) | 75,088 (73 KiB) | 75,942,144 (72.4 MiB) |
| `max_entries = 50000` | 46,130,976 (44.0 MiB) | 0 | 147,488 (144 KiB) | 75,088 (73 KiB) | 46,353,552 (44.2 MiB) |
| Exemplars on: `exemplar_keys = 'traceparent'` | 11,367,088 (10.8 MiB) | 2,080,000 (2.0 MiB) | 147,488 (144 KiB) | 75,088 (73 KiB) | 11,589,664 (11.1 MiB) |
| `max_connections = 5000` | 9,261,312 (8.8 MiB) | 0 | 147,488 (144 KiB) | 2,779,888 (2.7 MiB) | 12,188,688 (11.6 MiB) |
| Small instance: `max_entries = 2000`, `max_tagset_bytes = 256` | 1,356,800 (1.3 MiB) | 0 | 147,488 (144 KiB) | 40,272 (39 KiB) | 1,544,560 (1.5 MiB) |
<!-- shmem-sizing-table:end -->

### Limits for small instances

The memory is reserved even when the table is empty. As a guide, keep the extension's total under about 2–5% of the instance's RAM, and well below `shared_buffers`:

- **About 1–2 GB of RAM** (e.g. the smallest managed-database classes): the defaults (about 9 MiB) are fine. Don't combine a long history with many entries: `bucket_count = 288` at 10,000 entries is 72 MiB. Lower `max_entries` (2,000 entries at `max_tagset_bytes = 256` is 1.5 MiB in all) or `max_tagset_bytes` first.
- **4–8 GB:** up to about 100–200 MiB, e.g. 24 h of history at 10,000 entries, or 50,000 entries at 12 buckets.
- **`cardinality_cap_slots`** costs 9 bytes per slot (576 MiB at the maximum of 2^26 slots); raise it only for caps that need it.
- **Exemplars** add about `exemplar_memory` (default 2 MiB) to `shmem_bytes`.
- **Many connections** cost `align8(36 + max_tagset_bytes)` bytes per backend slot (552 at the default), e.g. 2.7 MiB at `max_connections = 5000`.

## Time buckets

Each entry keeps `calls` and `total_exec_time` per bucket of `bucket_interval`, for the last `bucket_count` buckets. Bucket boundaries fall on wall-clock multiples of `bucket_interval`, and all backends agree on them.

- **Completions per interval.** A statement is attributed to the bucket in which it *finishes* (`ExecutorEnd`, or the end of a utility statement), with all of its execution time, even if it started in an earlier bucket (for example a long-running cursor). Buckets therefore show completions per interval, not work done per interval.
- **Rolling window.** Buckets older than `bucket_count` intervals are hidden from the views immediately, and their slots are reused lazily. No background worker is needed.
- **Clock changes.** The current bucket never moves backwards. If the clock steps back, new calls go into the newest bucket already seen; if it jumps forward by more than the history window, all old buckets expire at once.

The statistics live in shared memory. With [`save`](#save) on they survive a clean restart (unless `bucket_interval` or `bucket_count` changed); they are lost after a crash or an immediate shutdown.

## Eviction

When a new combination arrives and the table already holds `max_entries` entries, an eviction pass runs:

1. All **dead** entries (all buckets expired) are reclaimed.
2. If that frees fewer than 5% of `max_entries`, live entries are evicted until 5% is free: least recently written first, then lowest usage (a call count that decays on each pass, as in `pg_stat_statements`).

Dead entries are not freed when they expire. They stay allocated (and are counted in `entries`) until the table fills up and a pass reclaims them. On a system whose tag combinations change over time, `entries` therefore normally climbs to `max_entries` and stays there, and passes run regularly even when no live history is lost.

With [`reclaim_worker`](#reclaim_worker) on, a background worker frees dead entries shortly after they expire instead, even with no query traffic: it reclaims every dead entry (counted in `reclaimed_entries`), never evicts a live one, does not decay usage and is not an eviction pass (`dealloc` does not change). `entries` then tracks the combinations seen within the last `bucket_count × bucket_interval`, and passes run only when that exceeds `max_entries`.

`pg_stat_statement_context_info()` reports what the passes did, with each outcome counted separately:

- `dealloc`: the number of eviction passes;
- `reclaimed_entries`: **dead** entries the passes (or the reclaim worker) reclaimed. This is normal housekeeping: their buckets had all expired, so no visible history was lost;
- `evicted_entries`: **live** entries the passes evicted because reclaiming dead ones did not free 5%. Their history disappears from the views early: the sign that `max_entries` is too small;
- `dropped_records`: calls that were not recorded at all, because a pass freed nothing (no entry was dead and the pass could not allocate the memory it needs to pick live victims), or the shared hash table refused the insert;
- `entries`: allocated entries, including dead ones not yet reclaimed.

How to read them:

- **`reclaimed_entries` grows, `evicted_entries` stays at 0:** the table is sized correctly. Combinations come and go, and the expired ones are recycled.
- **`evicted_entries` grows:** live history is being lost. Raise `max_entries` (see [above](#max_entries)), or look for a tag with unexpectedly high cardinality (see [Cardinality](extractors.md#allowlist-denylist-and-cardinality)) and [cap](#cardinality_cap) or exclude it; a cap doesn't stop combinations of capped keys from filling the table (see [Watching for cardinality pressure](extractors.md#watching-for-cardinality-pressure)). Compare its rate with that of `reclaimed_entries`: the larger its share, the more undersized the table.
- **`dropped_records` is ever non-zero:** calls were lost outright. This only happens when the table is full of live entries *and* a pass could free nothing (in practice, the backend ran out of memory); treat it as severe undersizing, or memory pressure, and act on it.

```sql
SELECT max_entries, entries, dealloc, reclaimed_entries, evicted_entries,
       dropped_records, oldest_bucket
  FROM pg_stat_statement_context_info();
```

Live eviction removes the least recently written entries first, so it mostly loses the history of rare combinations.

An eviction pass scans the whole table once (choosing the live victims by partial selection rather than sorting every entry), which costs time while the table is full. In the rare case that a pass can free nothing at all, the call is dropped (and counted in `dropped_records`) rather than failing the statement.
