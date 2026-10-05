-- Core GUCs (DESIGN.md §4.1). Requires the library in shared_preload_libraries.
-- Defaults are read from boot_val, and values are compared with what was set
-- before, so the test does not depend on the server's own configuration.
-- ALTER SYSTEM is used only with invalid values: it validates them (check
-- hooks, bounds) and fails before writing postgresql.auto.conf.

SELECT count(*) AS auto_conf_before
  FROM pg_file_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%'
   AND sourcefile LIKE '%postgresql.auto.conf' \gset

-- Every GUC, with description, type, default, context, unit and bounds.
\pset format unaligned
SELECT name, short_desc, vartype, boot_val, context, unit, min_val, max_val,
       enumvals
  FROM pg_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%'
 ORDER BY name COLLATE "C";
\pset format aligned

SELECT count(*) AS gucs,
       count(*) FILTER (WHERE short_desc IS NOT NULL AND short_desc <> '') AS described
  FROM pg_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%';

-- ---- superuser GUCs: a superuser can SET them ----
SET pg_stat_statement_context.enabled = off;
SHOW pg_stat_statement_context.enabled;
SET pg_stat_statement_context.track = 'all';
SHOW pg_stat_statement_context.track;
SET pg_stat_statement_context.track = 'none';
SHOW pg_stat_statement_context.track;
SET pg_stat_statement_context.track_utility = off;
SHOW pg_stat_statement_context.track_utility;
SET pg_stat_statement_context.nested_tags = 'scan';
SHOW pg_stat_statement_context.nested_tags;
SET pg_stat_statement_context.nested_tags = 'none';
SHOW pg_stat_statement_context.nested_tags;

-- Invalid values are rejected and the previous value stays.
SET pg_stat_statement_context.track = 'bogus';
SHOW pg_stat_statement_context.track;
SET pg_stat_statement_context.nested_tags = 'outer';
SHOW pg_stat_statement_context.nested_tags;
SET pg_stat_statement_context.enabled = 'maybe';
SHOW pg_stat_statement_context.enabled;
SET pg_stat_statement_context.track_utility = 2;
SHOW pg_stat_statement_context.track_utility;
RESET pg_stat_statement_context.enabled;
RESET pg_stat_statement_context.track;
RESET pg_stat_statement_context.track_utility;
RESET pg_stat_statement_context.nested_tags;

-- ---- a non-superuser cannot SET superuser GUCs ----
SELECT current_setting('pg_stat_statement_context.enabled') AS enabled_before,
       current_setting('pg_stat_statement_context.track') AS track_before,
       current_setting('pg_stat_statement_context.track_utility') AS track_utility_before,
       current_setting('pg_stat_statement_context.nested_tags') AS nested_tags_before
\gset
CREATE ROLE regress_pssc_nosuper;
SET ROLE regress_pssc_nosuper;
SET pg_stat_statement_context.enabled = off;
SET pg_stat_statement_context.track = 'all';
SET pg_stat_statement_context.track_utility = off;
SET pg_stat_statement_context.nested_tags = 'scan';
SELECT set_config('pg_stat_statement_context.enabled', 'off', true);
-- Reading them is allowed, and nothing changed.
SELECT current_setting('pg_stat_statement_context.enabled') = :'enabled_before' AS enabled_kept,
       current_setting('pg_stat_statement_context.track') = :'track_before' AS track_kept,
       current_setting('pg_stat_statement_context.track_utility') = :'track_utility_before' AS track_utility_kept,
       current_setting('pg_stat_statement_context.nested_tags') = :'nested_tags_before' AS nested_tags_kept;
RESET ROLE;
DROP ROLE regress_pssc_nosuper;

-- ---- sighup and postmaster GUCs cannot be SET in a session ----
SET pg_stat_statement_context.scan_window = '4kB';
SET pg_stat_statement_context.extractors = 'marginalia';
SET pg_stat_statement_context.tags = 'a';
SET pg_stat_statement_context.exclude_tags = 'a';
SET pg_stat_statement_context.untagged = 'record';
SET pg_stat_statement_context.max_entries = 1000;
SET pg_stat_statement_context.bucket_count = 6;
SET pg_stat_statement_context.bucket_interval = '60s';
SET pg_stat_statement_context.max_tags = 4;
SET pg_stat_statement_context.max_tag_value_len = 32;
SET pg_stat_statement_context.max_tagset_bytes = 256;

-- ---- invalid values are rejected by validation (nothing is written) ----
-- Bounds: one past each end (bounds of GUCs with units are tested in
-- test/t/003_guc.pl, since PG17+ adds the unit to the range message).
ALTER SYSTEM SET pg_stat_statement_context.max_entries = 99;
ALTER SYSTEM SET pg_stat_statement_context.max_entries = 1073741824;
ALTER SYSTEM SET pg_stat_statement_context.bucket_count = 0;
ALTER SYSTEM SET pg_stat_statement_context.bucket_count = 10001;
ALTER SYSTEM SET pg_stat_statement_context.max_tags = 0;
ALTER SYSTEM SET pg_stat_statement_context.max_tags = 65;
ALTER SYSTEM SET pg_stat_statement_context.max_tag_value_len = 0;
ALTER SYSTEM SET pg_stat_statement_context.max_tag_value_len = 4097;
ALTER SYSTEM SET pg_stat_statement_context.max_tagset_bytes = 127;
ALTER SYSTEM SET pg_stat_statement_context.max_tagset_bytes = 8193;
-- Units: bucket_interval is in seconds, scan_window in bytes.
ALTER SYSTEM SET pg_stat_statement_context.bucket_interval = '5 parsecs';
ALTER SYSTEM SET pg_stat_statement_context.scan_window = '2 lightyears';
-- Enums.
ALTER SYSTEM SET pg_stat_statement_context.untagged = 'maybe';
ALTER SYSTEM SET pg_stat_statement_context.track = 'everything';

-- tags / exclude_tags syntax.
ALTER SYSTEM SET pg_stat_statement_context.tags = 'action, contr oller';
ALTER SYSTEM SET pg_stat_statement_context.tags = E'action,\tjob\tid';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'action,,job';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'action, job,';
ALTER SYSTEM SET pg_stat_statement_context.tags = ', action';
ALTER SYSTEM SET pg_stat_statement_context.tags = ' , ';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'action, *';
ALTER SYSTEM SET pg_stat_statement_context.tags = '*, *';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'act*';
-- 64-byte key (the limit is 63).
SELECT 'action, ' || repeat('k', 64) AS long_key_list,
       repeat('x', 100) AS very_long_key \gset
ALTER SYSTEM SET pg_stat_statement_context.tags = :'long_key_list';
ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = 'trace parent';
ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = '*';
ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = 'request_id,,x';
ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = :'very_long_key';

-- None of the rejected ALTER SYSTEM commands wrote anything.
SELECT count(*) = :auto_conf_before AS auto_conf_unchanged
  FROM pg_file_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%'
   AND sourcefile LIKE '%postgresql.auto.conf';
