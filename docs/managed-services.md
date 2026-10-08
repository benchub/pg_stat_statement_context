# Managed services: privileges, parameter groups, troubleshooting

The rest of the documentation assumes a superuser who edits `postgresql.conf` or runs `ALTER SYSTEM`. On a managed PostgreSQL service (Amazon RDS, Google Cloud SQL, Azure Database for PostgreSQL, ...) that is usually not the case:

- The administrator role is **not a superuser**. On Amazon RDS, for example, it is a member of `rds_superuser`, which can create databases and roles and read monitoring data, but is not a PostgreSQL superuser.
- Server settings are changed through the provider's **parameter groups** (or "flags", "server parameters"), not in `postgresql.conf`, and the provider decides when it reloads or restarts the server.
- Server logs are not on a file system you can read; they are downloaded or streamed from the provider.

This page describes what PostgreSQL itself allows in that situation and how to diagnose the extension from SQL alone. Providers differ in which parameters they expose, in what privileges their administrator role has, and in whether they apply a change with a reload or a reboot; check your provider's documentation for those details. The extension must also be on the provider's allowlist (see [Deployment](limitations.md#deployment)).

Every statement about privileges on this page is checked by `test/t/037_managed_services.pl` against a role that stands in for a managed-service administrator: `NOSUPERUSER` with `CREATEDB`, `CREATEROLE` and membership in `pg_monitor`. That test also runs every example and checklist query below.

## Installing

1. In the parameter group, set `shared_preload_libraries` to include `pg_stat_statement_context`, **after** `pg_stat_statements` if you use it (see [Load order](../README.md#load-order)), for example `pg_stat_statements,pg_stat_statement_context`. This needs a reboot.
2. Check that `compute_query_id` is `auto` or `on` (see [Query IDs](../README.md#query-ids)).
3. In each database where you want to query the statistics, run `CREATE EXTENSION pg_stat_statement_context;` as the administrator role. The role that runs `CREATE EXTENSION` owns the extension's functions in that database, which matters for [the restricted functions](#functions-_reset-and-_extract).

## Settings

Every GUC has a context (the `context` column of `pg_settings`), which says how a change is applied and who may make it. What each context means is explained in [Configuration](configuration.md#guc-contexts); on a managed service it comes down to:

| Context | Set in the parameter group, then | Per session, role or database |
|---|---|---|
| `postmaster` | a **reboot** of the instance; until then `pg_settings.pending_restart` is true | no |
| `sighup` | a **reload** (the provider triggers it) | no: `SET`, `ALTER ROLE ... SET` and `ALTER DATABASE ... SET` fail with "cannot be changed now" |
| `superuser` | a **reload** | only for a superuser, or (PostgreSQL 15+) a role granted `SET` on the parameter |
| `user` | a **reload** | any role, with `SET`, `SET LOCAL`, connection options, or `ALTER ROLE`/`ALTER DATABASE ... SET` on a role or database it administers |

A non-superuser administrator cannot run `ALTER SYSTEM` (unless granted `ALTER SYSTEM` on the parameter, PostgreSQL 15+) <!-- check: alter-system-denied --> or call `pg_reload_conf()` <!-- check: reload-denied -->: on a managed service the parameter group is the only way to change `postmaster` and `sighup` settings. Neither kind can be set with `SET` or per role or database, by anyone <!-- check: sighup-not-settable -->.

The `superuser` settings can be set in the parameter group too. Setting them per session, role or database, which the rest of the documentation suggests for `tags`, `untagged`, `track` and others, fails for a non-superuser with "permission denied to set parameter", even for the owner of the database <!-- check: suset-denied -->. On PostgreSQL 15 and later a superuser can delegate one of them with `GRANT SET ON PARAMETER`; the grantee can then `SET` it and set it per role or database <!-- check: grant-set -->:

```sql
GRANT SET ON PARAMETER pg_stat_statement_context.untagged TO dba;
```

On a managed service only the provider can run that, if at all; ask your provider whether its administrator role holds or can obtain `SET` on custom parameters. The one `user` setting, [`tags_override`](configuration.md#tags_override), can be set by any role <!-- check: userset -->.

| GUC | Context | On a managed service |
|---|---|---|
| [`bucket_count`](configuration.md#bucket_count) | postmaster | parameter group, reboot |
| [`bucket_interval`](configuration.md#bucket_interval) | postmaster | parameter group, reboot |
| [`cardinality_cap`](configuration.md#cardinality_cap) | sighup | parameter group, reload |
| [`cardinality_cap_overrides`](configuration.md#cardinality_cap_overrides) | sighup | parameter group, reload |
| [`cardinality_cap_scope`](configuration.md#cardinality_cap_scope) | postmaster | parameter group, reboot |
| [`cardinality_cap_slots`](configuration.md#cardinality_cap_slots) | postmaster | parameter group, reboot |
| [`enabled`](configuration.md#enabled) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`exclude_tags`](configuration.md#exclude_tags) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`exemplar_keys`](configuration.md#exemplar_keys) | postmaster | parameter group, reboot |
| [`exemplar_memory`](configuration.md#exemplar_memory) | postmaster | parameter group, reboot |
| [`extractors`](configuration.md#extractors) | sighup | parameter group, reload |
| [`max_entries`](configuration.md#max_entries) | postmaster | parameter group, reboot |
| [`max_tag_value_len`](configuration.md#max_tag_value_len) | postmaster | parameter group, reboot |
| [`max_tags`](configuration.md#max_tags) | postmaster | parameter group, reboot |
| [`max_tagset_bytes`](configuration.md#max_tagset_bytes) | postmaster | parameter group, reboot |
| [`nested_tags`](configuration.md#nested_tags) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`normalize`](configuration.md#normalize) | sighup | parameter group, reload |
| [`reclaim_worker`](configuration.md#reclaim_worker) | postmaster | parameter group, reboot |
| [`reclaim_worker_interval`](configuration.md#reclaim_worker_interval) | sighup | parameter group, reload |
| [`save`](configuration.md#save) | sighup | parameter group, reload |
| [`scan_window`](configuration.md#scan_window) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`tags`](configuration.md#tags) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`tags_override`](configuration.md#tags_override) | user | any role: `SET`, `SET LOCAL`, connection options, per role or database |
| [`track`](configuration.md#track) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`track_utility`](configuration.md#track_utility) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |
| [`untagged`](configuration.md#untagged) | superuser | parameter group, reload; per session/role/database needs superuser or `GRANT SET` (PG 15+) |

Every name has the prefix `pg_stat_statement_context.`. Any role can read all of them in `pg_settings` and with `SHOW` <!-- check: settings-readable -->. After a parameter-group change, a `postmaster` setting shows its new value only after the reboot, and `pg_settings.pending_restart` is true for it once the server has reloaded the new value <!-- check: pending-restart -->.

## Values in a parameter group

String settings that use the [extractor DSL](extractors.md#the-extractor-dsl) or [`normalize`](configuration.md#normalize) rules contain quotes and backslashes, and each place you can write them adds its own quoting:

- **Parameter group** (and `SHOW`): the raw value, exactly as the extension reads it. Type it into the field as is, without surrounding quotes and without doubling anything. The DSL's own quoting stays: a quote *inside* a DSL string is written `''` in the raw value too.
- **SQL** (`ALTER SYSTEM SET`, `SET`): a string literal, so every `'` of the raw value is doubled and the whole is enclosed in `'...'`; backslashes stay single (with `standard_conforming_strings = on`, the default). Dollar quoting (`$$...$$`) takes the raw value unchanged.
- **`postgresql.conf`**: a quoted string in which every `'` is doubled and every `\` is doubled too.

After applying a parameter group, `SHOW` must print exactly the raw value. If it doesn't, the provider's console or API changed it (some treat commas or quotes in values specially); compare the two before suspecting the extension.

### A custom comment format

<!-- raw-example: svc-op -->
A house format `/* svc=billing op=charge */`, parsed by a [`regex`](extractors.md#regex) extractor, with an allowlist for its two keys. In the parameter group:

`pg_stat_statement_context.extractors`:

```text
regex(pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)
```

`pg_stat_statement_context.tags`:

```text
service, operation
```

The same from SQL:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  'regex(pattern=''svc=(\w+)\s+op=(\w+)'', keys=service|operation)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'service, operation';
SELECT pg_reload_conf();
```

and in `postgresql.conf`:

```ini
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=service|operation)'
pg_stat_statement_context.tags = 'service, operation'
```

Check it (once the reload has been applied):

```sql
SELECT pg_stat_statement_context_extract('SELECT 1 /* svc=billing op=charge */') -> 'tags';
-- {"service": "billing", "operation": "charge"}
```
<!-- /raw-example -->

### A quote inside a pattern

<!-- raw-example: quoted-pattern -->
A comment `/* app='billing' */`: the pattern contains quotes, which the DSL writes doubled. In the parameter group:

`pg_stat_statement_context.extractors`:

```text
regex(pattern='app=''(\w+)''', keys=application)
```

`pg_stat_statement_context.tags`:

```text
application
```

From SQL, dollar quoting avoids a second level of doubling:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  $$regex(pattern='app=''(\w+)''', keys=application)$$;
ALTER SYSTEM SET pg_stat_statement_context.tags = 'application';
SELECT pg_reload_conf();
```

In `postgresql.conf` every quote is doubled once more, and the backslash too:

```ini
pg_stat_statement_context.extractors = 'regex(pattern=''app=''''(\\w+)'''''', keys=application)'
pg_stat_statement_context.tags = 'application'
```

```sql
SELECT pg_stat_statement_context_extract($$SELECT 1 /* app='billing' */$$) -> 'tags';
-- {"application": "billing"}
```
<!-- /raw-example -->

### `normalize` rules

<!-- raw-example: normalize -->
Numeric IDs in a SQLCommenter `route` replaced by `:id`. In the parameter group:

`pg_stat_statement_context.extractors`:

```text
sqlcommenter
```

`pg_stat_statement_context.tags`:

```text
route
```

`pg_stat_statement_context.normalize`:

```text
route: '/users/\d+' => '/users/:id', route: '/posts/\d+' => '/posts/:id'
```

From SQL:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'sqlcommenter';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'route';
ALTER SYSTEM SET pg_stat_statement_context.normalize =
  $$route: '/users/\d+' => '/users/:id', route: '/posts/\d+' => '/posts/:id'$$;
SELECT pg_reload_conf();
```

In `postgresql.conf`:

```ini
pg_stat_statement_context.extractors = 'sqlcommenter'
pg_stat_statement_context.tags = 'route'
pg_stat_statement_context.normalize = 'route: ''/users/\\d+'' => ''/users/:id'', route: ''/posts/\\d+'' => ''/posts/:id'''
```

```sql
SELECT pg_stat_statement_context_extract($$SELECT 1 /*route='%2Fusers%2F42%2Fposts%2F7'*/$$) -> 'tags';
-- {"route": "/users/:id/posts/:id"}
```
<!-- /raw-example -->

## Privileges

### The views, `_info()` and `_counters()`

Any role can read the views (`pg_stat_statement_context`, `_totals`, `_last_bucket`, `_activity`) and call `pg_stat_statement_context_info()` and `pg_stat_statement_context_counters()` <!-- check: views-public -->. In the views, a role sees its own rows in full, but other roles' rows with `queryid` and `tags` (and `exemplars`) `NULL` <!-- check: own-rows -->. A role with the privileges of `pg_read_all_stats`, for example through `pg_monitor`, sees every row in full; an administrator role that is a member of `pg_monitor` therefore sees everything, while a new monitoring role sees only its own rows until it is granted one of them <!-- check: read-all-stats -->. See [Visibility and privacy](sql-interface.md#visibility-and-privacy).

```sql
GRANT pg_read_all_stats TO monitoring;   -- monitoring: an existing role
```

To run that grant, a non-superuser needs, on PostgreSQL 16 and later, `ADMIN OPTION` on `pg_read_all_stats` (being a member of it, or of `pg_monitor`, is not enough); on PostgreSQL 14 and 15, `CREATEROLE` is enough <!-- check: grant-read-all-stats -->. If your administrator role can't grant it, ask the provider how monitoring roles are set up there.

### Functions `_reset()` and `_extract()`

`pg_stat_statement_context_reset()` and `pg_stat_statement_context_extract()` have `EXECUTE` revoked from `PUBLIC`. They can be called by superusers, by their **owner**, which is the role that ran `CREATE EXTENSION` in that database (on a managed service, normally your administrator role) <!-- check: functions-owner -->, and by roles granted `EXECUTE`. Any other role gets "permission denied for function"; so does your administrator role in a database where someone else, such as the provider, created the extension, and it can't grant `EXECUTE` there either <!-- check: functions-revoked -->.

The owner can delegate them, in each database where the extension is installed <!-- check: grant-execute -->:

```sql
GRANT EXECUTE ON FUNCTION pg_stat_statement_context_reset() TO monitoring;
GRANT EXECUTE ON FUNCTION pg_stat_statement_context_extract(text, int, int) TO monitoring;
```

### Server settings and files

- `SHOW shared_preload_libraries` needs superuser or the privileges of `pg_read_all_settings` (included in `pg_monitor`) <!-- check: preload-visible -->.
- `pg_file_settings`, which shows errors in the configuration files, is readable only by superusers by default, even for members of `pg_monitor` <!-- check: file-settings-denied -->. The checklist below gives the alternatives.

## Troubleshooting

Each check below is SQL that the administrator role (a member of `pg_monitor`) can run, unless it says otherwise. Some causes appear only in the server log: download it from the provider (its console, CLI or log export) and search for `pg_stat_statement_context`.

### The views are empty

1. The library must be preloaded: `shared_preload_libraries` lists it, and its settings exist (26 rows). If it isn't, the views fail with `must be loaded via "shared_preload_libraries"`.

   ```sql
   SHOW shared_preload_libraries;
   SELECT count(*) FROM pg_settings WHERE name LIKE 'pg_stat_statement_context.%';
   ```

2. Recording must be on, statements need query IDs, and with the default `untagged = skip` only statements with at least one allowlisted tag are recorded.

   ```sql
   SHOW pg_stat_statement_context.enabled;
   SHOW pg_stat_statement_context.track;
   SHOW pg_stat_statement_context.untagged;
   SHOW pg_stat_statement_context.tags;
   SHOW compute_query_id;
   ```

3. Check what the configuration extracts from one of your real statements (as a role that may call [`_extract()`](#functions-_reset-and-_extract)). An empty `tags` means the comment format, its position or the allowlist doesn't match.

   ```sql
   SELECT pg_stat_statement_context_extract('SELECT 1 /*controller:users,action:show*/');
   ```

4. The counters show tags that were found but dropped (`invalid_tags`, `dropped_tags`, `capped_tags`), and extractors disabled by a failed regex compile (`regex_compile_failures`; the log has the details). `stats_reset` tells whether the store was reset recently.

   ```sql
   SELECT entries, invalid_tags, dropped_tags, capped_tags, regex_compile_failures, stats_reset
     FROM pg_stat_statement_context_counters();
   ```

5. Rows of other roles have `tags = NULL` without `pg_read_all_stats` (see [above](#the-views-_info-and-_counters)), so a query filtering on `tags` returns nothing for them. Rows also disappear when their buckets expire, after `bucket_count × bucket_interval`.

   ```sql
   SELECT count(*) AS rows, count(tags) AS rows_with_tags, count(DISTINCT userid) AS roles
     FROM pg_stat_statement_context_totals;
   SELECT oldest_bucket, current_bucket_start, buckets, bucket_seconds
     FROM pg_stat_statement_context_info();
   ```

### My utility statements are missing

Utility statements (DDL, `VACUUM`, ...) are recorded only with `track_utility = on`, and only when `pg_stat_statements` comes **before** `pg_stat_statement_context` in `shared_preload_libraries`. With the wrong order they are counted in `utility_missing_queryid` instead, and the server logs a `WARNING` at startup (see [Load order](../README.md#load-order)). On PostgreSQL 14 and 15 some utility statements have no query ID at all (see [Limitations](limitations.md#utility-statement-query-ids-on-postgresql-14-and-15)). `EXECUTE` and `PREPARE` are recorded as the statement they run, not as utilities.

```sql
SHOW pg_stat_statement_context.track_utility;
SHOW shared_preload_libraries;
SELECT utility_missing_queryid FROM pg_stat_statement_context_counters();
```

A growing `utility_missing_queryid` means the order is wrong: fix it in the parameter group and reboot.

### My settings change didn't apply

1. Look at the value in effect, where it comes from (`source`: `configuration file` for the parameter group, or `database`, `user`, `session`), and whether it waits for a reboot (`pending_restart`):

   ```sql
   SELECT name, setting, context, source, pending_restart
     FROM pg_settings
    WHERE name LIKE 'pg_stat_statement_context.%'
    ORDER BY name;
   ```

   - `pending_restart` is true: it is a `postmaster` setting; reboot the instance.
   - The value is the old one and `source` is `configuration file` or `default`: the reload hasn't happened yet, or the new value was **rejected**. A value that fails validation (a DSL syntax error, an invalid regex) is logged as `invalid value for parameter "pg_stat_statement_context...."` with a `DETAIL`, and the previous value stays in effect. Only the server log shows that error. To catch it before it reaches the parameter group, try the value with `ALTER SYSTEM` on a self-managed test server, which validates it immediately.
   - `SHOW` prints something other than what you typed: see [Values in a parameter group](#values-in-a-parameter-group).

2. A per-database or per-role setting overrides the parameter group for the `superuser` and `user` settings, and applies only to sessions started after it was set (pooled connections keep their old values):

   ```sql
   SELECT coalesce(d.datname, '(all)') AS database, coalesce(r.rolname, '(all)') AS role, s.setconfig
     FROM pg_db_role_setting s
     LEFT JOIN pg_database d ON d.oid = s.setdatabase
     LEFT JOIN pg_roles r ON r.oid = s.setrole
    WHERE array_to_string(s.setconfig, ',') LIKE '%pg_stat_statement_context.%';
   ```

3. Where you have a superuser (a self-managed test server), `pg_file_settings` shows the configuration files' values and their errors. It is not readable by the administrator role of a managed service.

   ```sql
   SELECT name, setting, applied, error FROM pg_file_settings
    WHERE name LIKE 'pg_stat_statement_context.%';
   ```

4. A regex extractor or `normalize` rule that was accepted can still be disabled in a backend whose compile of it fails at run time. `regex_compile_failures` counts these; the log names the pattern.

   ```sql
   SELECT regex_compile_failures FROM pg_stat_statement_context_counters();
   ```

### Statistics vanished after a restart

With [`save`](configuration.md#save) on, statistics survive a clean (smart or fast) shutdown. They are lost after a crash, an immediate shutdown, a failover (the new primary has only its own statistics, see [Replicas and failover](limitations.md#replicas-and-failover)), or when `bucket_interval`, `bucket_count` or the extension version changed. A `_reset()` also clears them. Compare the server's start time with `stats_reset`: if `stats_reset` is later, someone reset the statistics; if they are equal (to within seconds), the server started with an empty store.

```sql
SELECT pg_postmaster_start_time(), stats_reset, entries
  FROM pg_stat_statement_context_counters();
SHOW pg_stat_statement_context.save;
SELECT pg_is_in_recovery();
```

The reason is in the server log, around the startup:

- `pg_stat_statement_context: loaded N of M saved entries`: they were loaded (the `DETAIL` counts the ones dropped as expired, skipped or evicted);
- `pg_stat_statement_context: discarding saved statistics ...`: the file was not used, and the message says why;
- `pg_stat_statement_context: not saving statistics: the server did not shut down cleanly`, or no message at all: nothing was saved.

Whether the provider's maintenance, scaling or reboot operations shut the server down cleanly is up to the provider.
