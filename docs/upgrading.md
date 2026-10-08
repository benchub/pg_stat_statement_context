# Upgrading, downgrading and uninstalling

This page covers the lifecycle of an installation: upgrading the extension, moving to a new PostgreSQL major with `pg_upgrade`, going back to an older release, and removing the extension. It also says what happens to the saved statistics in each case.

`test/t/040_upgrade_uninstall.pl` checks the restart, discard and uninstall behavior described here on every supported major. The procedures were also run by hand in Docker on PostgreSQL 18, and `pg_upgrade` from 17 to 18 (backlog item 20261008-065635-9).

## Three version numbers

| What | Example | Where it comes from |
|---|---|---|
| Release | `1.0.0` | `CHANGELOG.md` and the package version (`postgresql-18-pg-stat-statement-context` `1.0.0-1.noble`, from `scripts/build-debs.sh`) |
| SQL version | `1.0` | `default_version` in `pg_stat_statement_context.control`. It is the release's `MAJOR.MINOR`, so a patch release keeps it. |
| Dump format | `1` | `PSSC_DUMP_FORMAT` in `src/store.c`. This internal number is bumped when the layout of the statistics file changes. |

The library is built with the SQL version of its own release (`PSSC_EXT_VERSION`, from the `.control` file). This is the version it reports and the version it writes into the statistics file.

## Identifying what is installed and loaded

```sql
-- SQL version installed in this database (run in each database)
SELECT extversion FROM pg_extension WHERE extname = 'pg_stat_statement_context';

-- SQL version of the installed .control file (what ALTER EXTENSION ... UPDATE would move to)
SELECT default_version, installed_version
  FROM pg_available_extensions WHERE name = 'pg_stat_statement_context';

-- PostgreSQL 18+: the library the server actually loaded, and its SQL version
SELECT module_name, version, file_name
  FROM pg_get_loaded_modules()
 WHERE file_name LIKE 'pg_stat_statement_context%';
```

On PostgreSQL 18, `pg_get_loaded_modules()` reads the library that is loaded in memory, so it reflects the files that were installed when the server was last started. Example output: `pg_stat_statement_context | 1.0 | pg_stat_statement_context.so`. It shows the SQL version, not the patch release. PostgreSQL 14–17 have no equivalent: `pg_available_extensions` reads the `.control` file on disk, which may be newer than the loaded library if the files were replaced without a restart. On every major, use the package manager for the exact release (for example `dpkg -s postgresql-17-pg-stat-statement-context`).

## Which changes need a restart

| Change | Restart? |
|---|---|
| Installing a new library (any upgrade or downgrade of the files) | **Yes.** The library is in `shared_preload_libraries`, so it is loaded when the server starts. Until the restart, the server keeps running the old one: after replacing the files on a running PostgreSQL 18 server (in Docker, on Linux), new connections still showed the old version in `pg_get_loaded_modules()`. |
| Adding the library to, or removing it from, `shared_preload_libraries` | **Yes.** |
| `ALTER EXTENSION ... UPDATE`, `CREATE EXTENSION`, `DROP EXTENSION` | No. These only change the SQL objects in one database. |
| A setting | It depends on the setting's context. `postmaster` settings (`max_entries`, `bucket_count`, ...) need a restart; see [GUC contexts](configuration.md#guc-contexts). |

## What happens to the saved statistics

The statistics live in shared memory. With [`save`](configuration.md#save) on (the default), a clean (smart or fast) shutdown writes them to `$PGDATA/pg_stat/pg_stat_statement_context.stat`, and the next start loads the file and then deletes it. The file is kept only when everything that gives its contents their meaning is unchanged. These checks are in `src/store.c` (`store_load()`), and the result of each start is logged:

| At the next start | The saved statistics are | LOG message |
|---|---|---|
| Nothing relevant changed, or only the library was replaced by one with the same SQL version (a patch release) | loaded | `loaded N of M saved entries` |
| The library has a different SQL version (`default_version`), newer or older | discarded | `discarding saved statistics ...: written by a different version` |
| The library writes a different dump format | discarded | the same, and the DETAIL gives both formats |
| A different PostgreSQL major version | discarded | the same |
| `bucket_interval` or `bucket_count` changed | discarded | `discarding saved statistics ...: bucket_interval or bucket_count changed` |
| `max_entries` or `max_tagset_bytes` is smaller | loaded, apart from the excess entries (evicted in [eviction](configuration.md#eviction) order) and the tag sets that no longer fit (skipped) | `loaded N of M saved entries` and a LOG line with the count |
| `save` is off | discarded | `discarding saved statistics ...: pg_stat_statement_context.save is off` |
| The previous shutdown was immediate, or the server crashed | none were saved | `not saving statistics: the server did not shut down cleanly` (at that shutdown) |
| The file is corrupt | discarded | `ignoring invalid data in file ...` |

Exemplars and the cardinality-cap tables are never saved. The discarded file is deleted, and the server always starts, with an empty store if needed.

In practice:

- **A patch release** (same SQL version, same dump format) keeps the statistics across the restart.
- **A minor or major release** changes the SQL version, so the statistics are discarded at the first start of the new library. This happens before you run `ALTER EXTENSION ... UPDATE`. The release notes say when a patch release changes the dump format.
- **`pg_upgrade` never carries the statistics over.** See [below](#major-postgresql-upgrade-with-pg_upgrade).

If you need the numbers, copy them out (for example `SELECT * FROM pg_stat_statement_context`) before the restart.

## Upgrading the extension

### A patch release (library only)

The SQL version doesn't change, so there is nothing to run in the databases.

1. Install the new package (or `make install`).
2. Restart the server.

The statistics are kept. Example log line: `loaded 2 of 2 saved entries from "pg_stat/pg_stat_statement_context.stat"`.

### A release with a new SQL version

1. Install the new package. It contains the new library, the new `.control` file and the upgrade scripts (`pg_stat_statement_context--1.0--1.1.sql`, ...).
2. Restart the server. The new library is loaded, and the saved statistics are discarded (see above). The databases still have the old SQL objects. They keep working, because each release keeps the C entry points the older SQL versions call (see [Version discipline](maintaining.md#5-version-discipline)).
3. In **every database** where the extension is created, run:

   ```sql
   ALTER EXTENSION pg_stat_statement_context UPDATE;
   ```

   This doesn't need a restart. To find the databases, connect to each one listed by `SELECT datname FROM pg_database WHERE datallowconn` and check `pg_extension`. If the extension is already at the target version, PostgreSQL prints `NOTICE:  version "1.0" of extension "pg_stat_statement_context" is already installed` and changes nothing.

Until step 3 is done in a database, the database has the old version's views and functions, without anything the new version adds.

## Downgrading

- **Library:** install the older package and restart. The saved statistics are discarded whenever the SQL version or dump format differs. Downgrading from a library with SQL version `1.1` to one with `1.0` logged `The file has format 1, PostgreSQL 18, extension version "1.1"; expected format 1, PostgreSQL 18, extension version "1.0".` The older library has only the entry points of its own and older SQL versions. A database that was updated to a newer SQL version must be brought back first (next item).
- **SQL objects:** this project ships no downgrade scripts, so `ALTER EXTENSION ... UPDATE TO` an older version fails (`ERROR:  extension "pg_stat_statement_context" has no update path from version ...`). Use `DROP EXTENSION pg_stat_statement_context;` and then `CREATE EXTENSION pg_stat_statement_context VERSION '1.0';` in each database. If your own views use the extension's views, `DROP EXTENSION` refuses until you drop them (or use `CASCADE`, which drops them too), so recreate them afterwards. Dropping and creating the extension doesn't touch the statistics in shared memory.

## Major PostgreSQL upgrade with pg_upgrade

`pg_upgrade` works with the extension installed. Tested from PostgreSQL 17 to 18 with the extension created in two databases, and a user view on `pg_stat_statement_context_totals` in each:

1. Install the extension's package (the same release) for the **new** major.
   - If it's missing and the new cluster preloads it, the new server can't start: `pg_upgrade --check` fails with `could not connect to target postmaster`, and its `pg_upgrade_server.log` has `FATAL:  could not access file "pg_stat_statement_context"`.
   - If it's missing and the new cluster doesn't preload it, the check fails at `Checking for presence of required libraries`, and `loadable_libraries.txt` lists `$libdir/pg_stat_statement_context` for each database.
2. Set `shared_preload_libraries` (with `pg_stat_statements` first) and the extension's settings in the new cluster's `postgresql.conf`. `pg_upgrade` doesn't copy the configuration files.
3. Run `pg_upgrade` as usual (`--check` first).
4. Start the new server. Each database has the extension at the same SQL version as before (`1.0`), and the user views still work. The `update_extensions.sql` script that `pg_upgrade` writes lists only extensions that ship a newer SQL version for the new major (in the test, `pg_stat_statements`), so this extension appears there only if its `.control` file for the new major has a newer `default_version`.

The statistics start empty on the new cluster:

- `pg_upgrade` doesn't copy `pg_stat/` from the old cluster. The old cluster's file stays in the old data directory until you delete the old cluster (`delete_old_cluster.sh`).
- The new cluster can contain a nearly empty statistics file that its server wrote while `pg_upgrade` started and stopped it. It is loaded (`loaded 0 of 0 saved entries`).
- Copying the old file across doesn't help. It is discarded because of the PostgreSQL major: `The file has format 1, PostgreSQL 17, extension version "1.0"; expected format 1, PostgreSQL 18, extension version "1.0".`

## Uninstalling

1. In **every database** where the extension is created, run:

   ```sql
   DROP EXTENSION pg_stat_statement_context;
   ```

   If your own views depend on the extension's views, drop them first or add `CASCADE`. Dropping the extension doesn't stop the collection: while the library is preloaded, statistics are recorded for every database, whether or not the extension exists in it.
2. Remove `pg_stat_statement_context` from `shared_preload_libraries`, and keep `pg_stat_statements` if you still use it. If it was set with `ALTER SYSTEM`, change it there, because `postgresql.auto.conf` overrides `postgresql.conf`. Remove the `pg_stat_statement_context.*` settings too.
3. Restart the server.
4. Delete `$PGDATA/pg_stat/pg_stat_statement_context.stat` if it exists. The last clean shutdown with the library loaded wrote it, and without the library nothing reads or removes it. If you add the library back later with the same version and settings, the file is loaded and those old statistics come back. Delete the file only while the library isn't loaded (after step 3, or with the server stopped): a running server with the library writes a new file at its next clean shutdown.
5. Remove the package (or the installed files: `pg_stat_statement_context.so` in `pg_config --pkglibdir`, and `pg_stat_statement_context.control` and `pg_stat_statement_context--*.sql` in `pg_config --sharedir`/extension).

If you do step 2 before step 1, the extension's views and functions fail with `pg_stat_statement_context must be loaded via "shared_preload_libraries"`. `DROP EXTENSION` still works without the library, so you can finish step 1 afterwards.
