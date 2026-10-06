/* pg_stat_statement_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statement_context" to load this file. \quit

-- SQL objects _info() and _reset() are added by later tasks; see
-- DESIGN.md §7.

-- Statistics per (userid, dbid, queryid, toplevel, tags) and time bucket
-- (DESIGN.md §7): one row per live bucket of each entry, or with
-- merge_buckets one row per entry summing its live buckets (bucket_start is
-- then the oldest of them). Expired buckets are hidden. For other roles'
-- rows queryid and tags are NULL unless the caller has the privileges of
-- pg_read_all_stats (checked in C, whatever showtags says); showtags =
-- false makes tags NULL in every row. Tags are converted from the encoding
-- of the database that recorded them; tags from a SQL_ASCII database, and
-- tag sets that cannot be converted to this database's encoding, are
-- escaped instead (bytes >= 0x80 as \xHH, '\' as \\).
CREATE FUNCTION pg_stat_statement_context(
    IN showtags boolean DEFAULT true,
    IN merge_buckets boolean DEFAULT false,
    OUT bucket_start timestamptz,
    OUT userid oid,
    OUT dbid oid,
    OUT queryid bigint,
    OUT toplevel bool,
    OUT tags jsonb,
    OUT calls bigint,
    OUT total_exec_time float8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_1_0'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statement_context AS
    SELECT * FROM pg_stat_statement_context(true, false);

-- One row per (userid, dbid, queryid, toplevel, tags) across live buckets;
-- bucket_start is the oldest contributing bucket.
CREATE VIEW pg_stat_statement_context_totals AS
    SELECT * FROM pg_stat_statement_context(true, true);

GRANT SELECT ON pg_stat_statement_context TO PUBLIC;
GRANT SELECT ON pg_stat_statement_context_totals TO PUBLIC;

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
