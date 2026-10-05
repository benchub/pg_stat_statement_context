/* pssc_compat_test--1.0.sql -- TEST-ONLY, see test/t/002_compat.pl */

\echo Use "CREATE EXTENSION pssc_compat_test" to load this file. \quit

-- shmem request shim: bump a counter in shared memory under the named LWLock.
CREATE FUNCTION pssc_compat_test_shmem_bump() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- ExecutorRun / ExecutorEnd (rows source) / ProcessUtility shims.
CREATE FUNCTION pssc_compat_test_reset() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_compat_test_stats(OUT executor_runs bigint,
                                       OUT last_rows bigint,
                                       OUT utility_calls bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- GUC extra allocator shim: value seen by the assign hook (via extra).
CREATE FUNCTION pssc_compat_test_guc_extra() RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Regex allocator shim.
CREATE FUNCTION pssc_compat_test_regex(pattern text, subject text,
                                       OUT matched bool,
                                       OUT capture text,
                                       OUT compiled_in_context bool,
                                       OUT freed bool)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Materialized SRF shim.
CREATE FUNCTION pssc_compat_test_srf(n int, use_expected_desc bool DEFAULT false,
                                     OUT i int, OUT label text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_compat_test_srf_scalar(n int, bless bool DEFAULT false)
RETURNS SETOF int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- Calls the SRF with no ReturnSetInfo, which the shim must reject.
CREATE FUNCTION pssc_compat_test_srf_direct() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Buffer/WAL/I-O timing/JIT availability macros.
CREATE FUNCTION pssc_compat_test_counter_fields() RETURNS text[]
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_compat_test_blk_time_accessors(OUT nslots int,
                                                    OUT shared_read int,
                                                    OUT shared_write int,
                                                    OUT local_read int,
                                                    OUT local_write int,
                                                    OUT temp_read int,
                                                    OUT temp_write int)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_compat_test_usage_delta(query text,
                                             OUT shared_blks bigint,
                                             OUT wal_records bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
