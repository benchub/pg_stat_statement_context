-- pg_stat_statement_context smoke test for a provisioned server ("make
-- smoke"; docs/managed-services.md#smoke-test). Unlike "make installcheck",
-- it is safe on a production or managed server (Amazon RDS, ...): it needs
-- only psql, connects with the libpq environment (PGHOST, PGPORT, PGUSER,
-- PGDATABASE, ...), runs as any role, superuser or not, and changes no
-- setting. Its only writes are CREATE EXTENSION when the extension is
-- missing in the database (left installed; it fails if the role may not
-- create it) and the statistics of its own tagged statements: one entry per
-- comment format, tagged controller=pssc_smoke.
--   psql -X -q -v ON_ERROR_STOP=1 -f test/smoke/smoke.sql
-- It prints "ok:", "skipped:" (with the reason) and "info:" lines, and on
-- failure a "FAIL:" line, and then exits non-zero. Checks:
--   - shared_preload_libraries lists the library (if this role may read it);
--   - the extension exists in this database, or is created;
--   - pg_stat_statement_context_counters() and _info() work (they fail when
--     the library is not preloaded);
--   - _extract() finds the smoke statements' tags under the server's
--     current extractor configuration (if this role may call it);
--   - the tagged smoke statements, in SQLCommenter and marginalia format,
--     are recorded (each format _extract() recognizes; without _extract(),
--     at least one) and visible to this role in the views;
--   - pg_file_settings is unchanged (if this role may read it).
-- The statements carry controller and action tags, which the default
-- extractors (sqlcommenter, marginalia) and tags allowlist (action,
-- controller, job) keep; with the default untagged = skip, statements
-- without a kept tag are not recorded.
\set ON_ERROR_STOP 1
\set QUIET 1
\set fail 'DO $pssc_smoke$ BEGIN RAISE EXCEPTION ''pg_stat_statement_context smoke test failed''; END $pssc_smoke$;'
\set stmt_sc 'SELECT ''pssc_smoke'' AS pssc_smoke /*controller=''pssc_smoke'',action=''sqlcommenter''*/'
\set stmt_mg 'SELECT ''pssc_smoke'' AS pssc_smoke /*controller:pssc_smoke,action:marginalia*/'

SELECT current_setting('server_version') AS smoke_version,
       current_database() AS smoke_db, current_user AS smoke_user,
       (SELECT CASE WHEN rolsuper THEN 'superuser' ELSE 'not superuser' END
          FROM pg_roles WHERE rolname = current_user) AS smoke_super,
       has_function_privilege('pg_show_all_file_settings()', 'EXECUTE')
         AS file_settings_readable \gset
\echo 'info: PostgreSQL' :smoke_version, 'database' :smoke_db, 'role' :smoke_user (:smoke_super)

\if :file_settings_readable
SELECT md5(coalesce(string_agg(concat_ws('|', sourcefile, sourceline, seqno,
         name, setting, applied, error), E'\n' ORDER BY seqno), ''))
         AS file_settings_before
  FROM pg_file_settings \gset
\endif

-- The library is preloaded. shared_preload_libraries is hidden from roles
-- without pg_read_all_settings (pg_settings then has no row for it).
SELECT EXISTS (SELECT 1 FROM pg_settings
                WHERE name = 'shared_preload_libraries') AS spl_readable,
       EXISTS (SELECT 1 FROM pg_settings s,
                      unnest(string_to_array(s.setting, ',')) AS l(lib)
                WHERE s.name = 'shared_preload_libraries'
                  AND regexp_replace(btrim(l.lib, ' "'),
                        '^.*/|\.(so|dylib|dll)$', '', 'g')
                      = 'pg_stat_statement_context') AS spl_has \gset
\if :spl_readable
\if :spl_has
\echo 'ok: shared_preload_libraries lists pg_stat_statement_context'
\else
\echo 'FAIL: shared_preload_libraries does not list pg_stat_statement_context: add it (after pg_stat_statements) and restart the server'
:fail
\endif
\else
\echo 'skipped: shared_preload_libraries not readable by this role (needs pg_read_all_settings): the functions below check the preload'
\endif

-- The extension.
SELECT EXISTS (SELECT 1 FROM pg_extension
                WHERE extname = 'pg_stat_statement_context') AS ext_exists,
       EXISTS (SELECT 1 FROM pg_available_extensions
                WHERE name = 'pg_stat_statement_context') AS ext_available \gset
\if :ext_exists
\elif :ext_available
\set ON_ERROR_STOP 0
CREATE EXTENSION pg_stat_statement_context;
\set ON_ERROR_STOP 1
\if :ERROR
\echo 'FAIL: the extension is missing in database' :smoke_db 'and this role cannot CREATE EXTENSION pg_stat_statement_context (error above): have the administrator run it'
:fail
\endif
\echo 'ok: created extension pg_stat_statement_context in database' :smoke_db '(left installed)'
\else
\echo 'FAIL: pg_stat_statement_context is not installed on the server (no pg_stat_statement_context.control)'
:fail
\endif
SELECT n.nspname AS s, e.extversion AS ext_version
  FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
 WHERE e.extname = 'pg_stat_statement_context' \gset
\echo 'ok: extension pg_stat_statement_context' :ext_version 'in schema' :s

-- The settings that decide what is recorded, for the messages below.
SELECT coalesce(current_setting('pg_stat_statement_context.enabled', true), '(unset)') AS g_enabled,
       coalesce(current_setting('pg_stat_statement_context.track', true), '(unset)') AS g_track,
       coalesce(current_setting('pg_stat_statement_context.untagged', true), '(unset)') AS g_untagged,
       coalesce(current_setting('pg_stat_statement_context.tags', true), '(unset)') AS g_tags,
       coalesce(current_setting('pg_stat_statement_context.extractors', true), '(unset)') AS g_extractors,
       coalesce(current_setting('compute_query_id', true), '(unset)') AS g_qid \gset
\echo 'info: enabled =' :g_enabled, 'track =' :g_track, 'untagged =' :g_untagged, 'compute_query_id =' :g_qid
\echo 'info: tags =' :'g_tags', 'extractors =' :'g_extractors'

-- The counters functions (both fail if the library is not preloaded).
\set ON_ERROR_STOP 0
SELECT entries AS c_entries, max_entries AS c_max_entries, buckets AS c_buckets,
       bucket_seconds AS c_bucket_seconds,
       max_entries > 0 AND buckets > 0 AND bucket_seconds > 0
         AND entries >= 0 AND stats_reset IS NOT NULL AS c_ok
  FROM :"s".pg_stat_statement_context_counters() \gset
\set ON_ERROR_STOP 1
\if :ERROR
\echo 'FAIL: pg_stat_statement_context_counters() failed (error above)'
:fail
\endif
\if :c_ok
\echo 'ok: pg_stat_statement_context_counters():' :c_entries 'of' :c_max_entries 'entries,' :c_buckets 'buckets of' :c_bucket_seconds 's'
\else
\echo 'FAIL: pg_stat_statement_context_counters() returned implausible values'
:fail
\endif
\set ON_ERROR_STOP 0
SELECT buckets = :c_buckets AND bucket_seconds = :c_bucket_seconds
         AND max_entries = :c_max_entries AND stats_reset IS NOT NULL AS i_ok,
       coalesce(oldest_bucket::text, 'none') AS i_oldest
  FROM :"s".pg_stat_statement_context_info() \gset
\set ON_ERROR_STOP 1
\if :ERROR
\echo 'FAIL: pg_stat_statement_context_info() failed (error above)'
:fail
\endif
\if :i_ok
\echo 'ok: pg_stat_statement_context_info() agrees with _counters(); oldest bucket:' :i_oldest
\else
\echo 'FAIL: pg_stat_statement_context_info() disagrees with _counters()'
:fail
\endif

-- What the current configuration extracts from the smoke statements.
SELECT has_function_privilege(
         format('%I.pg_stat_statement_context_extract(text, integer, integer)', :'s'),
         'EXECUTE') AS can_extract \gset
\if :can_extract
SELECT coalesce(:"s".pg_stat_statement_context_extract(:'stmt_sc') -> 'tags'
         ->> 'controller' = 'pssc_smoke', false) AS x_sc,
       coalesce(:"s".pg_stat_statement_context_extract(:'stmt_mg') -> 'tags'
         ->> 'controller' = 'pssc_smoke', false) AS x_mg \gset
\if :x_sc
\echo 'ok: _extract() finds the tags of the SQLCommenter smoke statement'
\else
\echo 'info: _extract() finds no controller tag in the SQLCommenter smoke statement under the current configuration'
\endif
\if :x_mg
\echo 'ok: _extract() finds the tags of the marginalia smoke statement'
\else
\echo 'info: _extract() finds no controller tag in the marginalia smoke statement under the current configuration'
\endif
SELECT :'x_sc'::boolean OR :'x_mg'::boolean AS x_any \gset
\if :x_any
\else
\echo 'FAIL: _extract() finds the smoke tags in neither format: the extractors or the tags allowlist (which must keep controller) do not match the smoke statements'
:fail
\endif
\else
\echo 'skipped: _extract() not executable by this role (GRANT EXECUTE to check what the configuration extracts)'
\endif

-- Record the smoke statements, three times in each format.
\set smoke_calls 'SELECT coalesce(sum(calls_total) FILTER (WHERE tags->>''action'' = ''sqlcommenter''), 0) AS calls_sc, coalesce(sum(calls_total) FILTER (WHERE tags->>''action'' = ''marginalia''), 0) AS calls_mg FROM ' :"s" '.pg_stat_statement_context(true, true) WHERE userid = (SELECT oid FROM pg_roles WHERE rolname = current_user) AND dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) AND tags->>''controller'' = ''pssc_smoke'''
:smoke_calls \gset before_
:stmt_sc \gset smoke_
:stmt_sc \gset smoke_
:stmt_sc \gset smoke_
:stmt_mg \gset smoke_
:stmt_mg \gset smoke_
:stmt_mg \gset smoke_
:smoke_calls \gset after_
SELECT :after_calls_sc - :before_calls_sc >= 3 AS rec_sc,
       :after_calls_mg - :before_calls_mg >= 3 AS rec_mg \gset
\if :rec_sc
\echo 'ok: SQLCommenter smoke statements recorded'
\endif
\if :rec_mg
\echo 'ok: marginalia smoke statements recorded'
\endif
\if :can_extract
SELECT (:'x_sc'::boolean AND NOT :'rec_sc'::boolean)
         OR (:'x_mg'::boolean AND NOT :'rec_mg'::boolean) AS rec_missing \gset
\else
SELECT NOT (:'rec_sc'::boolean OR :'rec_mg'::boolean) AS rec_missing \gset
\endif
\if :rec_missing
\echo 'FAIL: tagged smoke statements not recorded (calls before/after: SQLCommenter' :before_calls_sc/:after_calls_sc, 'marginalia' :before_calls_mg/:after_calls_mg'): check enabled, track, untagged, tags and extractors above, and capped_tags and dropped_tags in _counters()'
:fail
\endif

-- Visible to this role, with queryid and tags.
SELECT count(*) FILTER (WHERE queryid IS NOT NULL AND calls > 0) > 0 AS vis_ok
  FROM :"s".pg_stat_statement_context
 WHERE userid = (SELECT oid FROM pg_roles WHERE rolname = current_user)
   AND dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
   AND tags->>'controller' = 'pssc_smoke' \gset
SELECT count(*) FILTER (WHERE queryid IS NOT NULL AND calls_total > 0) > 0 AS tot_ok
  FROM :"s".pg_stat_statement_context_totals
 WHERE userid = (SELECT oid FROM pg_roles WHERE rolname = current_user)
   AND dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
   AND tags->>'controller' = 'pssc_smoke' \gset
SELECT count(*) AS lb_rows FROM :"s".pg_stat_statement_context_last_bucket \gset
SELECT count(*) AS act_rows FROM :"s".pg_stat_statement_context_activity \gset
\if :vis_ok
\else
\echo 'FAIL: the smoke statements are not visible with their queryid in the view pg_stat_statement_context'
:fail
\endif
\if :tot_ok
\else
\echo 'FAIL: the smoke statements are not visible with their queryid in the view pg_stat_statement_context_totals'
:fail
\endif
\echo 'ok: smoke statements visible in pg_stat_statement_context and _totals; _last_bucket and _activity readable'

\if :file_settings_readable
SELECT md5(coalesce(string_agg(concat_ws('|', sourcefile, sourceline, seqno,
         name, setting, applied, error), E'\n' ORDER BY seqno), ''))
         = :'file_settings_before' AS file_settings_same
  FROM pg_file_settings \gset
\if :file_settings_same
\echo 'ok: pg_file_settings unchanged'
\else
\echo 'FAIL: pg_file_settings changed during the smoke test'
:fail
\endif
\else
\echo 'skipped: pg_file_settings not readable by this role (the smoke test changes no setting)'
\endif

\echo 'smoke test passed'
