/* sql/pg_stat_statement_context--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statement_context UPDATE TO '1.1'" to load this file. \quit

-- 1.1 (DESIGN.md §6.13): exemplars. The stats functions and their views
-- gain a last column, exemplars jsonb: the most recent value of each key of
-- pg_stat_statement_context.exemplar_keys seen in the entry's statements
-- ({} when none is stored), whether or not the key is a grouping tag. It is
-- NULL exactly when tags is: other roles' rows without the privileges of
-- pg_read_all_stats, and every row with showtags = false. Exemplars live
-- only in shared memory (not saved across restarts). _info() gains
-- exemplar_shmem_bytes (the exemplar slots' share of shmem_bytes),
-- exemplar_value_bytes (the most bytes a value may take; longer values are
-- dropped) and exemplar_values_dropped (values dropped as too long).
--
-- A function's result type cannot be changed in place: drop the views and
-- functions, then recreate them as in 1.0 with the new columns.

DROP VIEW pg_stat_statement_context_last_bucket;
DROP VIEW pg_stat_statement_context_totals;
DROP VIEW pg_stat_statement_context;
DROP FUNCTION pg_stat_statement_context_last_bucket(boolean);
DROP FUNCTION pg_stat_statement_context(boolean, boolean);
DROP FUNCTION pg_stat_statement_context_info();

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
    OUT total_exec_time float8,
    OUT calls_total bigint,
    OUT exec_time_total float8,
    OUT stats_since timestamptz,
    OUT exemplars jsonb
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_1_1'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statement_context AS
    SELECT * FROM pg_stat_statement_context(true, false);

CREATE VIEW pg_stat_statement_context_totals AS
    SELECT * FROM pg_stat_statement_context(true, true);

CREATE FUNCTION pg_stat_statement_context_last_bucket(
    IN showtags boolean DEFAULT true,
    OUT bucket_start timestamptz,
    OUT userid oid,
    OUT dbid oid,
    OUT queryid bigint,
    OUT toplevel bool,
    OUT tags jsonb,
    OUT calls bigint,
    OUT total_exec_time float8,
    OUT calls_total bigint,
    OUT exec_time_total float8,
    OUT stats_since timestamptz,
    OUT exemplars jsonb
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_last_bucket_1_1'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statement_context_last_bucket AS
    SELECT * FROM pg_stat_statement_context_last_bucket(true);

GRANT SELECT ON pg_stat_statement_context TO PUBLIC;
GRANT SELECT ON pg_stat_statement_context_totals TO PUBLIC;
GRANT SELECT ON pg_stat_statement_context_last_bucket TO PUBLIC;

CREATE FUNCTION pg_stat_statement_context_info(
    OUT entries bigint,
    OUT max_entries bigint,
    OUT dealloc bigint,
    OUT reclaimed_entries bigint,
    OUT evicted_entries bigint,
    OUT dropped_records bigint,
    OUT buckets int,
    OUT bucket_seconds int,
    OUT oldest_bucket timestamptz,
    OUT current_bucket_start timestamptz,
    OUT last_closed_bucket_start timestamptz,
    OUT shmem_bytes bigint,
    OUT cap_shmem_bytes bigint,
    OUT invalid_tags bigint,
    OUT dropped_tags bigint,
    OUT heuristic_scans bigint,
    OUT regex_compile_failures bigint,
    OUT utility_missing_queryid bigint,
    OUT capped_tags bigint,
    OUT cap_table_full bigint,
    OUT stats_reset timestamptz,
    OUT stats_reset_epoch bigint,
    OUT exemplar_shmem_bytes bigint,
    OUT exemplar_value_bytes int,
    OUT exemplar_values_dropped bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_stat_statement_context_info_1_1'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;
