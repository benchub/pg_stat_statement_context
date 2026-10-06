-- pg_stat_statement_context.normalize (DESIGN.md §6.11 step 6): per-key
-- regex-replace rules for tag values, seen through the debug function
-- pg_stat_statement_context_extract(). Rules run after rename and the
-- allowlist/denylist and before truncation; a rule is equivalent to
-- regexp_replace(value COLLATE "C", pattern, replacement, 'g'). Requires the
-- library in shared_preload_libraries and a UTF8 database. The sighup GUCs
-- are changed with ALTER SYSTEM + pg_reload_conf() through
-- test/sql/include/config.sql; everything is reset at the end.
CREATE EXTENSION pg_stat_statement_context;

-- ---- the GUC ----
SELECT name, setting, context, vartype, boot_val
  FROM pg_settings WHERE name = 'pg_stat_statement_context.normalize';
-- sighup, like the extractors: not settable in a session
SET pg_stat_statement_context.normalize = '';

-- The tags plus the diagnostics that are not false or 0.
CREATE FUNCTION ex(q text)
RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_object_agg(key, value)
    FROM jsonb_each(pg_stat_statement_context_extract(q))
   WHERE key = 'tags'
      OR (key IN ('oom', 'invalid_tags', 'dropped_tags', 'regex_compile_failures',
                  'normalized_tags', 'normalize_failures')
          AND value NOT IN ('false', '0'))
$$;

\set extractors 'sqlcommenter, marginalia'
\set tags 'action, controller, route, job'
\set exclude_tags ''
\set scan_window 2048
\set normalize 'route: ''/users/\\d+'' => ''/users/:id'''
\i test/sql/include/config.sql
SHOW pg_stat_statement_context.normalize;

-- Both new counters are part of the result.
SELECT pg_stat_statement_context_extract('SELECT 1 /*route:/users/42*/')
       - 'stmt_start' - 'stmt_end' AS full_result;

-- ---- one rule ----
SELECT q, ex(q) FROM (VALUES
  ('SELECT 1 /*route:/users/42,action:/users/42*/'),  -- the rule's key only
  ('SELECT 1 /*route:/users/1/users/22/x*/'),          -- every match
  ('SELECT 1 /*route:/posts/1*/'),                     -- no match: not counted
  ($q$SELECT 1 /*route='%2Fusers%2F7%3Fa%3D1'*/$q$),   -- after decoding
  ('SELECT 1 /*route:/users/1,route:/users/2*/'),      -- first occurrence wins
  ('SELECT 1; /*route:/users/9*/')                     -- footer
) v(q);
-- Different values collapse into one tag set.
SELECT pg_stat_statement_context_extract('SELECT 1 /*route:/users/1*/')->'tagset_bytes' =
       pg_stat_statement_context_extract('SELECT 1 /*route:/users/123456*/')->'tagset_bytes'
       AS same_size,
       ex('SELECT 1 /*route:/users/1*/')->'tags' =
       ex('SELECT 1 /*route:/users/123456*/')->'tags' AS same_tags;

-- ---- equivalence with regexp_replace(v COLLATE "C", p, r, 'g') ----
\set normalize 'route: ''x*'' => ''-'''
\i test/sql/include/config.sql
SELECT v, ex('SELECT 1 /*route:' || v || '*/')->'tags'->>'route' AS normalized,
       regexp_replace(v COLLATE "C", 'x*', '-', 'g') AS regexp_replace
  FROM (VALUES ('abc'), ('xx'), ('axxb'), ('x'), ('ax')) t(v);
\set normalize 'route: ''(\\w)(\\w)'' => ''\\2\\1'''
\i test/sql/include/config.sql
SELECT v, ex('SELECT 1 /*route:' || v || '*/')->'tags'->>'route' AS normalized,
       regexp_replace(v COLLATE "C", '(\w)(\w)', '\2\1', 'g') AS regexp_replace
  FROM (VALUES ('abcde'), ('a-bc-def'), ('é1ü2')) t(v);
\set normalize 'route: ''[a-z]+'' => ''<\\&>\\\\'''
\i test/sql/include/config.sql
SELECT v, ex('SELECT 1 /*route:' || v || '*/')->'tags'->>'route' AS normalized,
       regexp_replace(v COLLATE "C", '[a-z]+', '<\&>\\', 'g') AS regexp_replace
  FROM (VALUES ('ab/cd'), ('/1-')) t(v);

-- ---- several rules: in order, each on the output of the previous one ----
\set normalize 'route: ''\\d+'' => ''N'', route: ''N/N'' => ''pair'', controller: ''^(\\w+)s$'' => ''\\1'', job: ''(?i)^JOB-'' => '''''
\i test/sql/include/config.sql
SHOW pg_stat_statement_context.normalize;
SELECT ex('SELECT 1 /*route:/1/22/x,controller:users,job:Job-7,action:12*/') AS chained;
-- Quotes in patterns and replacements, separators inside quotes, quoted keys.
\set normalize ' ''route'' : ''it''''s,[|]=>:'' => ''x,y'' , job:''^'' => ''''''q'' '
\i test/sql/include/config.sql
SHOW pg_stat_statement_context.normalize;
SELECT ex($q$SELECT 1 /*route='it%27s%2C%7C%3D%3E%3A',job='j'*/$q$) AS quoting;
-- Non-ASCII patterns.
\set normalize 'route: ''[éü]'' => ''?'''
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*route:Zürich-café*/') AS multibyte;

-- ---- position in the pipeline ----
-- After rename: rules see the renamed key.
\set extractors 'marginalia(rename=path:route|route:old_route)'
\set tags '*'
\set normalize 'route: ''\\d+'' => ''N'', path: ''\\d+'' => ''P'', old_route: ''\\d+'' => ''O'''
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*path:/a/1,route:/b/2*/') AS after_rename;
-- After the allowlist and the denylist: dropped tags are not normalized.
\set extractors 'sqlcommenter, marginalia'
\set tags 'action'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*route:/b/2,action:a*/') AS allowlist;
\set tags '*'
\set exclude_tags 'route'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*route:/b/2,action:a*/') AS denylist;
\set exclude_tags ''
-- Before truncation (max_tag_value_len = 64): a rule sees the whole value,
-- and its output is truncated on a character boundary.
\set normalize 'route: ''^a{70}Z$'' => ''short'', job: ''^.*$'' => ''\\&\\&'', action: ''x'' => ''é'''
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*route:' || repeat('a', 70) || 'Z*/')->'tags' AS sees_whole_value,
       length(ex('SELECT 1 /*job:' || repeat('j', 40) || '*/')->'tags'->>'job') AS grown_truncated,
       ex('SELECT 1 /*action:' || repeat('x', 40) || '*/')->'tags'->>'action' =
         repeat('é', 32) AS multibyte_truncated;
-- A rule may empty a value; the tag is kept.
\set normalize 'route: ''^.*$'' => '''''
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*route:/users/1*/') AS emptied;

-- ---- invalid rules are rejected (check hook), nothing is written ----
SELECT count(*) AS before FROM pg_file_settings
 WHERE name = 'pg_stat_statement_context.normalize'
   AND sourcefile LIKE '%postgresql.auto.conf' \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: /x/ => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''x''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''x'' => y';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''x => ''y';
ALTER SYSTEM SET pg_stat_statement_context.normalize = ': ''x'' => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'ro*: ''x'' => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'ro ute: ''x'' => ''y''';
SELECT repeat('k', 64) || ': ''x'' => ''y''' AS v \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: '''' => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''(a'' => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''(a)\1'' => ''y''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''(a)'' => ''\2''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''a'' => ''\q''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''a'' => ''b\''';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''a'' => ''b'',';
ALTER SYSTEM SET pg_stat_statement_context.normalize = 'route: ''a'' => ''b'' job: ''c'' => ''d''';
SELECT 'route: ''' || repeat('a', 1025) || ''' => ''b''' AS v \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
SELECT 'route: ''a'' => ''' || repeat('b', 1025) || '''' AS v \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
SELECT string_agg('k' || i || ': ''a'' => ''b''', ', ') AS v
  FROM generate_series(1, 33) i \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
SELECT count(*) = :before AS nothing_written FROM pg_file_settings
 WHERE name = 'pg_stat_statement_context.normalize'
   AND sourcefile LIKE '%postgresql.auto.conf';
-- The limits themselves are accepted.
SELECT string_agg('k' || i || ': ''a'' => ''b''', ', ') AS v
  FROM generate_series(1, 32) i \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
SELECT repeat('k', 63) || ': ''' || repeat('a', 1024) || ''' => '''
       || repeat('b', 1024) || '''' AS v \gset
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'v';
-- Empty and blank settings have no rules.
ALTER SYSTEM SET pg_stat_statement_context.normalize = '  ';

-- ---- clean up ----
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
