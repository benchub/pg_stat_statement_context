/* pg_stat_statement_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statement_context" to load this file. \quit

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

-- Current tags of each backend, a companion to pg_stat_activity (join on
-- pid): one row per backend whose last top-level statement had a frame,
-- with that statement's userid, dbid, queryid (NULL if 0) and tags; state
-- is 'active' while it runs and 'idle' after it ended. A top-level
-- statement without a frame (pg_stat_statement_context.enabled off, no
-- query ID) and backend exit remove the row. For other roles' rows
-- queryid, state and tags are NULL unless the caller has the privileges of
-- pg_read_all_stats (checked in C), as in the statistics views.
CREATE FUNCTION pg_stat_statement_context_activity(
    OUT pid integer,
    OUT userid oid,
    OUT dbid oid,
    OUT queryid bigint,
    OUT state text,
    OUT tags jsonb
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_activity_1_0'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statement_context_activity AS
    SELECT * FROM pg_stat_statement_context_activity();

GRANT SELECT ON pg_stat_statement_context_activity TO PUBLIC;

-- Debug function (DESIGN.md §7, §9): runs the tag-set pipeline of the
-- executor hooks on one statement of query (located like the parser does:
-- stmt_location -1 = the whole string, stmt_len 0 = to the end; both in
-- bytes) with the current configuration, and returns
--   {"tags": {...}, "ntags", "tagset_bytes", "footer", "heuristic", "oom",
--    "stmt_start", "stmt_end", "invalid_tags", "dropped_tags",
--    "heuristic_scans", "regex_compile_failures", "normalized_tags",
--    "normalize_failures", "capped_tags"}
-- A value over its key's cardinality cap shows as JSON null (capped_tags
-- counts them); the caps are only peeked at, no value is admitted.
-- In a SQL_ASCII database, non-ASCII bytes and '\' in tag keys and values
-- are escaped as \xHH and \\ (tagset_bytes counts the stored bytes).
-- Records nothing and works even when pg_stat_statement_context.enabled is
-- off. Superuser-only by default (it runs the regex engine on arbitrary
-- input and reveals the extractor configuration); GRANT EXECUTE to allow
-- others. PARALLEL RESTRICTED: parallel workers don't bound regex compiles.
CREATE FUNCTION pg_stat_statement_context_extract(query text,
                                                  stmt_location int DEFAULT -1,
                                                  stmt_len int DEFAULT 0)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_extract'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION pg_stat_statement_context_extract(text, int, int) FROM PUBLIC;

-- Store and diagnostic counters (DESIGN.md §7), one row: entries,
-- max_entries, eviction passes (dealloc) and the entries they removed
-- (evicted_entries), buckets (= bucket_count), oldest_bucket (start of the
-- oldest live bucket of any entry; NULL if none), the exact shared memory
-- size requested at startup (shmem_bytes) and for the separate cardinality
-- caps table (cap_shmem_bytes), the extraction counters, the
-- values collapsed to null by the cardinality caps (capped_tags; of which
-- cap_table_full because the tracking table was full), and the time of the
-- last reset (or of startup). Readable by everyone, like
-- pg_stat_statements_info.
CREATE FUNCTION pg_stat_statement_context_info(
    OUT entries bigint,
    OUT max_entries bigint,
    OUT dealloc bigint,
    OUT evicted_entries bigint,
    OUT buckets int,
    OUT oldest_bucket timestamptz,
    OUT shmem_bytes bigint,
    OUT cap_shmem_bytes bigint,
    OUT invalid_tags bigint,
    OUT dropped_tags bigint,
    OUT heuristic_scans bigint,
    OUT regex_compile_failures bigint,
    OUT utility_missing_queryid bigint,
    OUT capped_tags bigint,
    OUT cap_table_full bigint,
    OUT stats_reset timestamptz
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_info'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Removes every entry, zeroes every counter of _info(), sets stats_reset
-- and empties the cardinality caps' sets of admitted values. Superuser-only by default; GRANT EXECUTE to allow others.
CREATE FUNCTION pg_stat_statement_context_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_reset'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION pg_stat_statement_context_reset() FROM PUBLIC;
