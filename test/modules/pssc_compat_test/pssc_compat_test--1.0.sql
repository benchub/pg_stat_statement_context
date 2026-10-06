/* pssc_compat_test--1.0.sql -- TEST-ONLY, see test/t/002_compat.pl */

\echo Use "CREATE EXTENSION pssc_compat_test" to load this file. \quit

-- shmem request shim: bump a counter in shared memory under the named LWLock.
CREATE FUNCTION pssc_compat_test_shmem_bump() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- ExecutorRun / ProcessUtility shims.
CREATE FUNCTION pssc_compat_test_reset() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_compat_test_stats(OUT executor_runs bigint,
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

-- src/counters.h backend conversions to ms (executor totaltime, utility instr_time).
CREATE FUNCTION pssc_compat_test_counters_ms(sleep_ms int,
                                             OUT pre_total float8,
                                             OUT exec_ms float8,
                                             OUT exec_again float8,
                                             OUT exec_pgss float8,
                                             OUT util_ms float8,
                                             OUT util_pgss float8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
