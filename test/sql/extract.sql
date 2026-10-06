-- pg_stat_statement_context_extract() (DESIGN.md §7, §9): the debug function
-- that runs the tag-set pipeline of the executor hooks (src/extract.c) on a
-- given query string with the current configuration, and the scanner and
-- extractor edge cases it exposes. Requires the library in
-- shared_preload_libraries and a UTF8 database (REGRESS_OPTS --encoding).
-- The sighup GUCs are changed with ALTER SYSTEM + pg_reload_conf() through
-- test/sql/include/config.sql; everything is reset at the end.
CREATE EXTENSION pg_stat_statement_context;
SHOW server_encoding;

\set extractors 'sqlcommenter, marginalia'
\set tags 'action, controller, job'
\set exclude_tags 'traceparent, tracestate, request_id'
\set scan_window 2048
\i test/sql/include/config.sql

-- ---- the function, its properties and its privileges ----
SELECT p.oid::regprocedure AS function, p.prorettype::regtype AS returns,
       pg_get_function_arguments(p.oid) AS arguments,
       p.provolatile, p.proparallel, p.proisstrict
  FROM pg_proc p
 WHERE p.proname = 'pg_stat_statement_context_extract';
SELECT has_function_privilege('public',
         'pg_stat_statement_context_extract(text, int, int)', 'EXECUTE')
       AS public_can_execute;

-- Every key of the result (DESIGN.md §7).
SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show,application:app*/');
SELECT jsonb_pretty(pg_stat_statement_context_extract(
         'SELECT 1 /*controller=''users'',action=''show''*/'));
-- STRICT.
SELECT pg_stat_statement_context_extract(NULL) IS NULL AS null_query;

-- The rest of the test reads a summary: the tags, plus the diagnostics that
-- are not false or 0.
CREATE FUNCTION ex(q text, loc int DEFAULT -1, len int DEFAULT 0)
RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_object_agg(key, value)
    FROM jsonb_each(pg_stat_statement_context_extract(q, loc, len))
   WHERE key = 'tags'
      OR (key IN ('footer', 'heuristic', 'oom', 'invalid_tags', 'dropped_tags',
                  'heuristic_scans', 'regex_compile_failures')
          AND value NOT IN ('false', '0'))
$$;

-- ---- no comment, empty input, comment placement ----
SELECT q, ex(q) FROM (VALUES
  (''),
  ('SELECT 1'),
  ('SELECT 1 /* just a remark */'),
  ('SELECT 1 /*controller:users*/'),
  ('SELECT 1 /*controller:users*/;'),
  (E'SELECT 1 /*controller:users*/ ;\n  ;  '),
  ('/*controller:users*/ SELECT 1'),            -- default position=append
  ('SELECT /*controller:users*/ 1'),
  ('SELECT 1 /*controller:users*/ -- trailing line comment'),
  ('SELECT 1 -- /*controller:users*/')
) v(q);

-- ---- nested comments ----
SELECT q, ex(q) FROM (VALUES
  ('SELECT 1 /* a /* b */ c */ /*controller:users*/'),
  -- the comment-like text is inside one nested comment, not a comment of its own
  ('SELECT 1 /* x /* y */ /*controller:fake*/ */'),
  ('SELECT 1 /*controller:users /* nested */ */'),
  ('SELECT 1 /*/**/controller:users*/')
) v(q);

-- ---- dollar quotes and $ in identifiers ----
SELECT q, ex(q) FROM (VALUES
  ('SELECT $$ /*controller:fake*/ $$'),
  ('SELECT $a$ $$ /*controller:fake*/ $a$ /*controller:real*/'),
  ('SELECT $a$ /*controller:fake*/ $b$ $a$ /*controller:real*/'),
  ('SELECT $_1$ /*controller:fake*/ $_1$ /*controller:real*/'),
  -- $ inside identifiers does not open a dollar quote
  ('SELECT foo$bar$ /*controller:real*/'),
  ('SELECT a$ AS x$y$ /*controller:real*/'),
  -- parameters
  ('SELECT $1 /*controller:real*/'),
  ('SELECT $1$2 /*controller:real*/'),
  -- an unterminated dollar quote swallows the rest
  ('SELECT $a$ /*controller:fake*/')
) v(q);

-- ---- string literals: '', E'', U&'', quoted identifiers ----
SELECT q, ex(q) FROM (VALUES
  ($q$SELECT ' /*controller:fake*/ ' /*controller:real*/$q$),
  ($q$SELECT 'it''s /*controller:fake*/' /*controller:real*/$q$),
  -- E'': backslash escapes the quote
  ($q$SELECT E'it\'s /*controller:fake*/' /*controller:real*/$q$),
  ($q$SELECT e'\\' /*controller:real*/$q$),
  ($q$SELECT E'\\\' /*controller:fake*/' /*controller:real*/$q$),
  -- U&'': backslash is a Unicode escape, never a quote escape
  ($q$SELECT U&'d\0061t\+000061' /*controller:real*/$q$),
  ($q$SELECT U&'\' /*controller:real*/$q$),
  ($q$SELECT u&'\' UESCAPE '!' /*controller:real*/$q$),
  -- quoted identifiers
  ($q$SELECT 1 AS "/*controller:fake*/" /*controller:real*/$q$),
  ($q$SELECT 1 AS "a""b /*controller:fake*/" /*controller:real*/$q$),
  ($q$SELECT 1 AS U&"\0061 /*controller:fake*/" /*controller:real*/$q$),
  -- unterminated literals swallow the rest
  ($q$SELECT ' /*controller:fake*/$q$),
  ($q$SELECT " /*controller:fake*/$q$)
) v(q);

-- ---- standard_conforming_strings ----
-- on: a backslash in a plain '' string is literal, so 'it\' ends the string,
-- the comment is real and the trailing ' starts an unterminated string.
SET standard_conforming_strings = on;
SELECT ex($q$SELECT 'it\' /*controller:outside*/$q$) AS scs_on_1,
       ex($q$SELECT 'it\'s /*controller:fake*/' /*controller:real*/$q$) AS scs_on_2;
-- off: the backslash escapes the quote, as in E''.
SET standard_conforming_strings = off;
SELECT ex($q$SELECT 'it\' /*controller:outside*/$q$) AS scs_off_1,
       ex($q$SELECT 'it\'s /*controller:fake*/' /*controller:real*/$q$) AS scs_off_2;
RESET standard_conforming_strings;

-- ---- unterminated comments ----
SELECT q, ex(q) FROM (VALUES
  ('SELECT 1 /*controller:users'),
  ('SELECT 1 /*controller:users*'),
  ('SELECT 1 /* /*controller:users*/'),
  ('SELECT 1 /*controller:users*/ /* unterminated'),
  ('SELECT 1 /*controller:users*/ /* /* x */')
) v(q);

-- ---- multibyte text ----
SELECT q, ex(q) FROM (VALUES
  ('SELECT ''日本語 /*controller:fake*/'' /*controller:日本語,action:show*/'),
  ('SELECT 1 /*controller:Ünïcödé*/'),
  ('SELECT 1 /*controller=''caf%C3%A9'',action=''ü''*/'),
  ('SELECT ''😀'' /* 😀 */ /*controller:😀*/')
) v(q);

-- ---- multi-statement strings: statement ranges and trailing footers ----
-- Ranges are given as the parser reports them: stmt_location in bytes
-- (PG14-17: just after the previous ';'; PG18: the first token, so leading
-- comments lie before it), stmt_len in bytes, 0 = to the end of the string,
-- stmt_location -1 = the whole string. stmt_start/stmt_end show the range
-- the statement owns (DESIGN.md §6.5).
\set q 'SELECT 1 /*controller:a*/; SELECT 2 /*controller:b*/'
SELECT ex(:'q') AS whole,
       ex(:'q', 0, 25) AS first,
       ex(:'q', 26, 0) AS second_pg14,
       ex(:'q', 27, 0) AS second_pg18;
SELECT r->'stmt_start' AS start, r->'stmt_end' AS "end", r->'tags' AS tags
  FROM (VALUES (-1, 0), (0, 25), (26, 0), (27, 0), (27, 25), (27, 20)) v(loc, len),
       LATERAL pg_stat_statement_context_extract(:'q', loc, len) r;

-- Leading comments of a later statement belong to it, wherever the parser
-- starts its range (prepend extractor).
\set extractors 'marginalia(position=prepend)'
\i test/sql/include/config.sql
\set q 'SELECT 1; /*controller:lead*/ SELECT 2'
SELECT ex(:'q', 0, 8) AS first,
       ex(:'q', 9, 0) AS second_pg14,
       ex(:'q', 30, 0) AS second_pg18,
       (pg_stat_statement_context_extract(:'q', 30, 0))->'stmt_start' AS owned_start;
-- A ';' inside a string or comment is not a statement boundary.
\set q 'SELECT '';'' /* ; */; /*controller:lead*/ SELECT 2'
SELECT ex(:'q', 19, 0) AS second_pg14, ex(:'q', 40, 0) AS second_pg18;
\set extractors 'sqlcommenter, marginalia'
\i test/sql/include/config.sql

-- Trailing footer: used only by the last statement, and only when its own
-- range has no tags.
\set q 'SELECT 1; SELECT 2; /*controller:foot*/'
SELECT ex(:'q', 0, 8) AS first, ex(:'q', 9, 9) AS last,
       ex(:'q', 10, 8) AS last_pg18, ex(:'q') AS whole;
\set q 'SELECT 1; SELECT 2 /*controller:own*/; /*controller:foot*/'
SELECT ex(:'q', 9, 28) AS own_wins;
\set q 'SELECT 1; SELECT 2; /*controller:foot*/ SELECT 3'
SELECT ex(:'q', 9, 9) AS not_last;
\set q 'SELECT 1; SELECT 2; /*controller:foot*/ ; /*action:x*/ ;'
SELECT ex(:'q', 9, 9) AS footer_run;
\set q 'SELECT 1; SELECT 2; -- x\n/*controller:foot*/'
SELECT ex(:'q', 9, 9) AS footer_after_line_comment;

-- Out-of-range statement locations are rejected.
SELECT pg_stat_statement_context_extract('SELECT 1', -2, 0);
SELECT pg_stat_statement_context_extract('SELECT 1', 9, 0);
SELECT pg_stat_statement_context_extract('SELECT 1', 0, 9);
SELECT pg_stat_statement_context_extract('SELECT 1', 7, 2);
SELECT pg_stat_statement_context_extract('SELECT 1', 0, -1);
-- At the end of the string: an empty statement.
SELECT ex('SELECT 1', 8, 0) AS at_end, ex('SELECT 1', -1, 5) AS loc_unknown;

-- ---- long statements (DESIGN.md §6.2) ----
-- Within scan_window the statement is lexed exactly from the front. Beyond
-- it, append uses the inexact tail path (heuristic): the lexer state at the
-- window start is unknown, so a line comment (or string) that begins before
-- the window can fool it. Prepend stays exact.
SELECT ex('SELECT ' || repeat('1,', 3000) || '1 /*controller:long*/') AS long_append,
       ex('SELECT ' || repeat('1,', 3000) || '1 /*controller:long*/;  ') AS long_append_semi;
SELECT ex('SELECT 1 -- ' || repeat('x', 100) || ' /*controller:fake*/') AS short_line_comment,
       ex('SELECT 1 -- ' || repeat('x', 3000) || ' /*controller:fake*/') AS long_line_comment,
       ex('SELECT 1 -- ' || repeat('x', 3000) || E'\n /*controller:real*/') AS long_after_newline,
       ex(E'SELECT 1 -- x\n' || repeat('x', 3000) || ' -- /*controller:fake*/') AS long_dashes_in_window;
\set extractors 'marginalia(position=prepend)'
\i test/sql/include/config.sql
SELECT ex('/*controller:head*/ SELECT ' || repeat('1,', 3000) || '1') AS long_prepend;
\set extractors 'sqlcommenter, marginalia'
\i test/sql/include/config.sql
\set scan_window 64
\i test/sql/include/config.sql
-- a comment crossing the window start yields nothing
SELECT ex('SELECT 1 /*controller:' || repeat('x', 70) || '*/') AS crosses_window,
       ex('SELECT ' || repeat(' ', 70) || '1 /*controller:x*/') AS just_past_window,
       ex(repeat(' ', 30) || 'SELECT 1 /*controller:x*/') AS within_window;
\set scan_window 2048
\i test/sql/include/config.sql

-- ---- sqlcommenter ----
SELECT q, ex(q) FROM (VALUES
  ($q$SELECT 1 /*controller='users',action='show'*/$q$),
  ($q$SELECT 1 /*action='a%20b',controller='c+d',job='e%2Bf'*/$q$),
  ($q$SELECT 1 /*controller='it\'s',action='back\\slash'*/$q$),
  ($q$SELECT 1 /*controller='a,b',action='c'*/$q$),
  ($q$SELECT 1 /* controller = 'users' , action = 'show' */$q$),
  ($q$SELECT 1 /*controller='bad%zz',action='show'*/$q$),
  ($q$SELECT 1 /*controller=users*/$q$),
  ($q$SELECT 1 /*controller='users',garbage,action='show'*/$q$),
  ($q$SELECT 1 /*con%74roller='users'*/$q$)
) v(q);
\set extractors 'sqlcommenter(url_decode=off)'
\i test/sql/include/config.sql
SELECT ex($q$SELECT 1 /*controller='a%20b+c',action='%00'*/$q$) AS url_decode_off;

-- ---- marginalia ----
\set extractors 'marginalia'
\i test/sql/include/config.sql
SELECT q, ex(q) FROM (VALUES
  ('SELECT 1 /*application:Foo,controller:users,action:show*/'),
  ('SELECT 1 /*controller:users,action:show,line:app/models/u.rb:12*/'),
  ('SELECT 1 /*controller:users,action:show*/ /*with annotation*/'),
  ('SELECT 1 /* controller : users , action : show */'),
  ('SELECT 1 /*controller:users,,action:show,*/'),
  ('SELECT 1 /*controller:users,free text,action:show*/'),
  ('SELECT 1 /*controller=''users''*/')
) v(q);
\set extractors 'marginalia(kv_sep=''='', pair_sep='' '')'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*controller=users action=show*/') AS custom_separators;

-- ---- regex ----
\set extractors 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=controller|action)'
\i test/sql/include/config.sql
SELECT q, ex(q) FROM (VALUES
  ('SELECT 1 /* svc=billing op=charge */'),
  -- comments only, never the query text; position=any by default
  ('SELECT ''svc=fake op=fake'' /* svc=billing op=charge */'),
  ('SELECT ''svc=fake op=fake'''),
  ('/* svc=billing op=charge */ SELECT 1 /* other */'),
  ('SELECT 1 -- svc=billing op=charge'),
  ('SELECT 1 /* svc=billing op=charge */ /* svc=x op=y */'),
  ('SELECT 1 /* svc=日本 op=語 */'),
  ('SELECT 1 /* svc=billing */')
) v(q);
-- optional groups, every match
\set extractors 'regex(pattern=''(\\w+)=(\\w+)?'', keys=controller|action)'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /* a=b c=d */') AS first_match_wins,
       ex('SELECT 1 /* a= */') AS unmatched_optional_group;

-- ---- the extractor chain ----
-- The first extractor that produces wins; merge=on extractors still run;
-- the first occurrence of a key wins.
\set extractors 'sqlcommenter, marginalia'
\i test/sql/include/config.sql
SELECT ex($q$SELECT 1 /*controller:m*/ /*controller='s'*/$q$) AS sqlcommenter_first,
       ex($q$SELECT 1 /*controller:m,action:m*/$q$) AS falls_through;
\set extractors 'sqlcommenter, marginalia(merge=on)'
\i test/sql/include/config.sql
SELECT ex($q$SELECT 1 /*controller='s'*/ /*controller:m,action:m*/$q$) AS merged;
\set extractors 'marginalia(position=prepend), marginalia(position=append, merge=on)'
\i test/sql/include/config.sql
SELECT ex('/*controller:head*/ SELECT 1 /*controller:tail,action:tail*/') AS prepend_then_append;

-- ---- allowlist, per-extractor keys, rename, denylist ----
\set extractors 'marginalia(keys=controller|route, rename=route:action)'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*controller:c,route:r,action:a,job:j*/') AS keys_then_rename;
\set extractors 'sqlcommenter(rename=route:endpoint), marginalia(rename=controller:endpoint)'
\set tags 'endpoint, action, job'
\i test/sql/include/config.sql
SELECT ex($q$SELECT 1 /*route='/users',action='show'*/$q$) AS sqlcommenter_renamed,
       ex($q$SELECT 1 /*controller:users,action:show*/$q$) AS marginalia_renamed,
       ex($q$SELECT 1 /*route:users*/$q$) AS not_renamed;
\set extractors 'marginalia'
\set tags '*'
\set exclude_tags 'traceparent, request_id'
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*controller:c,traceparent:00-abc,request_id:42,zeta:z,alpha:a*/') AS denylist;
\set exclude_tags ''
\i test/sql/include/config.sql
SELECT ex('SELECT 1 /*controller:c,traceparent:00-abc*/') AS empty_denylist;
-- keys longer than 63 bytes are dropped and counted
SELECT ex('SELECT 1 /*' || repeat('k', 63) || ':ok,' || repeat('k', 64) || ':long*/') AS key_lengths;

-- ---- %00 and invalid encodings ----
\set extractors 'sqlcommenter'
\i test/sql/include/config.sql
SELECT q, ex(q) FROM (VALUES
  ($q$SELECT 1 /*controller='a%00b',action='ok'*/$q$),
  ($q$SELECT 1 /*con%00troller='x',action='ok'*/$q$),
  ($q$SELECT 1 /*controller='%FF',action='ok'*/$q$),
  ($q$SELECT 1 /*controller='%C3',action='ok'*/$q$),
  ($q$SELECT 1 /*controller='%C3%A4',action='ok'*/$q$),
  ($q$SELECT 1 /*controller='%ED%A0%80',action='ok'*/$q$),
  ($q$SELECT 1 /*%FF='x',action='ok'*/$q$)
) v(q);

-- ---- limits: truncation on character boundaries, max_tags, max_tagset_bytes ----
-- (max_tag_value_len = 64, max_tags = 8 and max_tagset_bytes = 512 are
-- postmaster settings at their defaults.)
\set extractors 'marginalia'
\i test/sql/include/config.sql
SELECT t.key, octet_length(t.value) AS bytes, char_length(t.value) AS chars,
       t.value
  FROM ex('SELECT 1 /*a:' || repeat('x', 70) ||
          ',b:' || repeat('é', 40) ||
          ',c:' || repeat('日', 30) ||
          ',d:' || repeat('😀', 20) ||
          ',e:x' || repeat('😀', 20) ||
          ',f:' || repeat('y', 64) || '*/') r,
       jsonb_each_text(r->'tags') t
 ORDER BY t.key;
-- ten tags: the last two in sorted-key order do not fit max_tags
SELECT ex('SELECT 1 /*k0:0,k1:1,k2:2,k3:3,k4:4,k5:5,k6:6,k7:7,k8:8,k9:9*/') AS max_tags;
-- eight 64-byte values do not fit 512 bytes: greedy fill keeps what fits
SELECT jsonb_object_keys(r->'tags') AS kept, r->'dropped_tags' AS dropped,
       r->'tagset_bytes' AS bytes
  FROM pg_stat_statement_context_extract(
         'SELECT 1 /*' ||
         (SELECT string_agg('k' || i || ':' || repeat('v', 64), ',')
            FROM generate_series(1, 8) i) || ',z:small*/') r;

-- ---- DSL and regex errors ----
-- sighup settings cannot be SET in a session.
SET pg_stat_statement_context.extractors = 'marginalia';
-- ALTER SYSTEM validates the DSL and regexes (check hook) and writes nothing.
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'sqlcomenter';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'marginalia(position=middle)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'marginalia(keys=a|)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'regex(pattern=''(a'', keys=k)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'regex(pattern=''(a)\1'', keys=k)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'regex(pattern=''(?:(a))\1'', keys=k)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'regex(pattern=''(a)(b)'', keys=k)';
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'regex(pattern=''(a)(b)(c)(d)(e)(f)(g)(h)(i)'', keys=a|b|c|d|e|f|g|h|i)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'action, *';
-- The configuration in effect is unchanged.
SELECT current_setting('pg_stat_statement_context.extractors') AS extractors,
       ex('SELECT 1 /*controller:still*/') AS still_marginalia;

-- ---- works while the extension is disabled ----
SET pg_stat_statement_context.enabled = off;
SELECT ex('SELECT 1 /*controller:off*/') AS enabled_off;
RESET pg_stat_statement_context.enabled;

-- ---- privileges ----
CREATE ROLE regress_pssc_extract;
SET ROLE regress_pssc_extract;
SELECT pg_stat_statement_context_extract('SELECT 1 /*controller:x*/');
RESET ROLE;
GRANT EXECUTE ON FUNCTION pg_stat_statement_context_extract(text, int, int)
  TO regress_pssc_extract;
SET ROLE regress_pssc_extract;
SELECT pg_stat_statement_context_extract('SELECT 1 /*controller:x*/')->'tags' AS granted;
RESET ROLE;
REVOKE EXECUTE ON FUNCTION pg_stat_statement_context_extract(text, int, int)
  FROM regress_pssc_extract;
DROP ROLE regress_pssc_extract;

-- ---- clean up ----
DROP FUNCTION ex(text, int, int);
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
ALTER SYSTEM RESET pg_stat_statement_context.exclude_tags;
ALTER SYSTEM RESET pg_stat_statement_context.scan_window;
SELECT pg_reload_conf();
SELECT count(*) AS auto_conf_entries
  FROM pg_file_settings
 WHERE name LIKE 'pg\_stat\_statement\_context.%'
   AND sourcefile LIKE '%postgresql.auto.conf';
DROP EXTENSION pg_stat_statement_context;
