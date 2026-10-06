/* pg_stat_statement_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statement_context" to load this file. \quit

-- SQL objects (stats SRF, views, _info(), _reset()) are added by later
-- tasks; see DESIGN.md §7.

-- Debug function (DESIGN.md §7, §9): runs the tag-set pipeline of the
-- executor hooks on one statement of query (located like the parser does:
-- stmt_location -1 = the whole string, stmt_len 0 = to the end; both in
-- bytes) with the current configuration, and returns
--   {"tags": {...}, "ntags", "tagset_bytes", "footer", "heuristic", "oom",
--    "stmt_start", "stmt_end", "invalid_tags", "dropped_tags",
--    "heuristic_scans", "regex_compile_failures"}
-- In a SQL_ASCII database, non-ASCII bytes and '\' in tag keys and values
-- are escaped as \xHH and \\ (tagset_bytes counts the stored bytes).
-- Records nothing and works even when pg_stat_statement_context.enabled is
-- off. Superuser-only by default (it runs the regex engine on arbitrary
-- input and reveals the extractor configuration); GRANT EXECUTE to allow
-- others.
CREATE FUNCTION pg_stat_statement_context_extract(query text,
                                                  stmt_location int DEFAULT -1,
                                                  stmt_len int DEFAULT 0)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_extract'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

REVOKE ALL ON FUNCTION pg_stat_statement_context_extract(text, int, int) FROM PUBLIC;
