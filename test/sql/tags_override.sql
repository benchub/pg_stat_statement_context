-- pg_stat_statement_context.tags_override (backlog item
-- 20261005-091225-30): a USERSET GUC holding sqlcommenter-style tags
-- (k='v',k2='v2', URL-encoded) merged with the comment tags; the override
-- wins every key conflict. Seen through pg_stat_statement_context_extract(),
-- which uses the session's current override. Each case prints 'ok' or what
-- it got instead. Requires the library in shared_preload_libraries and a
-- UTF8 database. The sighup GUCs are changed with ALTER SYSTEM +
-- pg_reload_conf() through test/sql/include/config.sql; everything is reset
-- at the end.
CREATE EXTENSION pg_stat_statement_context;

-- The tags plus the diagnostics that are not false or 0.
CREATE FUNCTION ex(q text)
RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_object_agg(key, value)
    FROM jsonb_each(pg_stat_statement_context_extract(q))
   WHERE key = 'tags'
      OR (key IN ('oom', 'footer', 'invalid_tags', 'dropped_tags',
                  'regex_compile_failures', 'normalized_tags',
                  'normalize_failures')
          AND value NOT IN ('false', '0'))
$$;
CREATE FUNCTION chk(q text, want jsonb)
RETURNS text LANGUAGE sql AS $$
  SELECT CASE WHEN ex(q) = want THEN 'ok' ELSE 'got ' || ex(q)::text END
$$;

\set extractors 'sqlcommenter, marginalia'
\set tags '*'
\set exclude_tags ''
\set scan_window 2048
\i test/sql/include/config.sql

-- ---- the check_hook ----
SHOW pg_stat_statement_context.tags_override;
SELECT context, vartype, boot_val FROM pg_settings
 WHERE name = 'pg_stat_statement_context.tags_override';
-- Malformed values are rejected at SET time; the value is unchanged.
SET pg_stat_statement_context.tags_override = 'controller=users';
SET pg_stat_statement_context.tags_override = 'a=''1'',junk';
SET pg_stat_statement_context.tags_override = 'a=''1';
SET pg_stat_statement_context.tags_override = 'a b=''1''';
SET pg_stat_statement_context.tags_override = 'a=''%zz''';
SET pg_stat_statement_context.tags_override = 'k%zz=''1''';
SET pg_stat_statement_context.tags_override = 'a=''%00''';
SET pg_stat_statement_context.tags_override = 'a=''%C3%28''';
SET pg_stat_statement_context.tags_override = 'kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk=''1''';
SET LOCAL pg_stat_statement_context.tags_override = 'x';
SHOW pg_stat_statement_context.tags_override;
SELECT chk('SELECT 1', '{"tags": {}}');
-- A 63-byte key, '+' as a space, an empty value, an empty override.
SET pg_stat_statement_context.tags_override = 'kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk=''1''';
SELECT chk('SELECT 1', '{"tags": {"kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk": "1"}}');
SET pg_stat_statement_context.tags_override = ' controller=''users'' , action=''sh%20ow+x'',e='''' ';
SHOW pg_stat_statement_context.tags_override;
SELECT chk('SELECT 1', '{"tags": {"action": "sh ow x", "controller": "users", "e": ""}}');
SET pg_stat_statement_context.tags_override = '  ';
SELECT chk('SELECT 1', '{"tags": {}}');

-- ---- merge with the comment tags ----
SET pg_stat_statement_context.tags_override = 'controller=''ovr'',job=''j''';
SELECT chk('SELECT 1 /*action=''a''*/',
           '{"tags": {"action": "a", "controller": "ovr", "job": "j"}}');
SELECT chk('SELECT 1 /*controller=''c'',action=''a''*/',
           '{"tags": {"action": "a", "controller": "ovr", "job": "j"}}');
SELECT chk('SELECT 1 /*controller:m*/',
           '{"tags": {"controller": "ovr", "job": "j"}}');
-- The trailing footer is still looked for (the last statement, 0..8, has
-- no comment of its own).
SELECT jsonb_object_agg(key, value) = '{"tags": {"action": "f", "controller": "ovr", "job": "j"}, "footer": true}' AS footer_ok
  FROM jsonb_each(pg_stat_statement_context_extract('SELECT 1; /*action:f*/', 0, 8))
 WHERE key IN ('tags', 'footer');

-- ---- SET LOCAL, RESET ----
SET pg_stat_statement_context.tags_override = 'a=''outer''';
BEGIN;
SET LOCAL pg_stat_statement_context.tags_override = 'a=''inner''';
SELECT chk('SELECT 1', '{"tags": {"a": "inner"}}');
COMMIT;
SELECT chk('SELECT 1', '{"tags": {"a": "outer"}}');
BEGIN;
SET pg_stat_statement_context.tags_override = 'a=''rolled''';
ROLLBACK;
SELECT chk('SELECT 1', '{"tags": {"a": "outer"}}');
RESET pg_stat_statement_context.tags_override;
SELECT chk('SELECT 1 /*a:c*/', '{"tags": {"a": "c"}}');

-- ---- any role can set it ----
CREATE ROLE pssc_override_user;
SET ROLE pssc_override_user;
SET pg_stat_statement_context.tags_override = 'who=''me''';
RESET ROLE;
SELECT chk('SELECT 1', '{"tags": {"who": "me"}}');
DROP ROLE pssc_override_user;

-- ---- the pipeline: rename (sqlcommenter extractors only), keys not
-- applied, allowlist, denylist, truncation, normalize ----
\set extractors 'sqlcommenter(keys=zzz, rename=route:endpoint), marginalia(rename=a:mg), appname(format=sqlcommenter, rename=b:app)'
\i test/sql/include/config.sql
SET pg_stat_statement_context.tags_override = 'route=''/r'',a=''1'',b=''2''';
SELECT chk('SELECT 1', '{"tags": {"a": "1", "b": "2", "endpoint": "/r"}}');
SELECT chk('SELECT 1 /*route=''/c'',k=''v''*/',
           '{"tags": {"a": "1", "b": "2", "endpoint": "/r"}}');
\set extractors 'sqlcommenter(rename=route:endpoint), appname(format=sqlcommenter)'
\i test/sql/include/config.sql
SELECT chk('SELECT 1 /*route=''/c'',k=''v''*/',
           '{"tags": {"a": "1", "b": "2", "endpoint": "/r", "k": "v"}}');
-- precedence: override > comment > application_name
SET application_name = 'a=''app'',c=''app'',d=''app''';
SELECT chk('SELECT 1 /*a=''cmt'',c=''cmt''*/',
           '{"tags": {"a": "1", "b": "2", "c": "cmt", "d": "app", "endpoint": "/r"}}');
RESET application_name;
\set extractors 'sqlcommenter, marginalia'
\set tags 'controller,action'
\i test/sql/include/config.sql
SET pg_stat_statement_context.tags_override = 'job=''j'',controller=''c'',action=''a''';
SELECT chk('SELECT 1', '{"tags": {"action": "a", "controller": "c"}}');
\set tags '*'
\set exclude_tags 'request_id'
\i test/sql/include/config.sql
SET pg_stat_statement_context.tags_override = 'request_id=''r1'',a=''1''';
SELECT chk('SELECT 1', '{"tags": {"a": "1"}}');
\set exclude_tags ''
\i test/sql/include/config.sql
-- truncated to max_tag_value_len (64) bytes, on a character boundary
SET pg_stat_statement_context.tags_override = 'a=''xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxyz'',b=''xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx%C3%A9''';
SELECT chk('SELECT 1', '{"tags": {"a": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "b": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}}');
\set normalize 'route: ''\\d+'' => '':id'''
\i test/sql/include/config.sql
SET pg_stat_statement_context.tags_override = 'route=''/users/42''';
SELECT chk('SELECT 1', '{"tags": {"route": "/users/:id"}, "normalized_tags": 1}');

-- ---- clean up ----
RESET pg_stat_statement_context.tags_override;
DROP FUNCTION chk(text, jsonb);
DROP FUNCTION ex(text);
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
ALTER SYSTEM RESET pg_stat_statement_context.exclude_tags;
ALTER SYSTEM RESET pg_stat_statement_context.scan_window;
ALTER SYSTEM RESET pg_stat_statement_context.normalize;
SELECT pg_reload_conf();
SELECT count(*) AS auto_conf_entries
  FROM pg_file_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%'
   AND sourcefile LIKE '%postgresql.auto.conf';
DROP EXTENSION pg_stat_statement_context;
