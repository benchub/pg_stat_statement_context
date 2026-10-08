/* pssc_extract_test--1.0.sql -- TEST-ONLY, see test/t/005_extract.pl */

\echo Use "CREATE EXTENSION pssc_extract_test" to load this file. \quit

-- Run the tag-set pipeline (pssc_extract_tags) on query, a statement of
-- which is located like the parser reports it (stmt_location -1: the whole
-- string; stmt_len 0: to the end), with the owned start of DESIGN.md §6.5.
-- The query is raw bytes (bytea, so invalid encodings and NUL bytes can be
-- tested; the string is NUL-terminated after them) or text. bufsize is the
-- output buffer size (default PSSC_TAGSET_BYTES_MAX). Errors out if the
-- result breaks an invariant (layout, sorting, bounds, hash, buffer
-- overrun). tags are "key=value"; the counters are this call's deltas.
CREATE FUNCTION pssc_extract_test(query bytea,
                                  stmt_location int DEFAULT -1,
                                  stmt_len int DEFAULT 0,
                                  bufsize int DEFAULT 8192,
                                  OUT tags text[],
                                  OUT serialized bytea,
                                  OUT nbytes int,
                                  OUT hash bigint,
                                  OUT ntags int,
                                  OUT footer bool,
                                  OUT oom bool,
                                  OUT invalid_tags bigint,
                                  OUT dropped_tags bigint,
                                  OUT heuristic_scans bigint,
                                  OUT regex_compile_failures bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION pssc_extract_test(query text,
                                  stmt_location int DEFAULT -1,
                                  stmt_len int DEFAULT 0,
                                  bufsize int DEFAULT 8192,
                                  OUT tags text[],
                                  OUT serialized bytea,
                                  OUT nbytes int,
                                  OUT hash bigint,
                                  OUT ntags int,
                                  OUT footer bool,
                                  OUT oom bool,
                                  OUT invalid_tags bigint,
                                  OUT dropped_tags bigint,
                                  OUT heuristic_scans bigint,
                                  OUT regex_compile_failures bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_tagset_hash() of raw bytes.
CREATE FUNCTION pssc_extract_test_hash(data bytea) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Install (true) a fake regex extractor hook in this backend, or restore
-- (false) the real regex runtime (pssc_regex_extract): with the fake,
-- capture group i (key i of the extractor) is the i-th whitespace-separated
-- word of the comment body; missing words give no pair.
CREATE FUNCTION pssc_extract_test_fake_regex(enable bool) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Remove the regex hook in this backend: regex extractors produce nothing.
CREATE FUNCTION pssc_extract_test_no_regex() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Fault injection into the regex runtime of this backend
-- (pssc_regex_test_hook): before creating the memory context of a pattern
-- to compile (phase 'context'), before each pg_regcomp ('compile') or
-- pg_regexec ('exec') of extractor idx (-1: any) -- or the same for
-- normalize rule idx ('norm_context', 'norm_compile', 'norm_exec') --, the
-- next count times
-- (-1: always): 'espace' / 'etoobig' (as if the engine returned that code),
-- 'oom' (throw out of memory), 'error' (elog ERROR), 'cancel' (throw query
-- canceled), 'regcancel' (as the PG14/15 engine on a pending cancel: set
-- QueryCancelPending, return REG_CANCEL), 'sleep' (spin, using CPU like a
-- slow compile, up to 60 s in a CHECK_FOR_INTERRUPTS loop, for a real
-- statement_timeout or the compile time limit), 'regsleep' (the same as the
-- PG14/15 engine does it: poll for a pending cancel for up to 60 s, then
-- return REG_CANCEL), 'stall' / 'regstall' (the same, but sleeping: a
-- compile that is descheduled rather than slow), 'lateint' /
-- 'lateregint' (wait up to 60 s, without processing interrupts, for a
-- pending cancel such as the compile time limit's, then send this backend a
-- real SIGINT, then CHECK_FOR_INTERRUPTS / return REG_CANCEL), 'latewait'
-- (the same, but wait another 1 s instead of sending SIGINT, so that a
-- statement_timeout or transaction_timeout can fire), 'lateconflict' /
-- 'lateregconflict' (the same, but send this backend a real recovery
-- conflict signal (SIGUSR1) instead of SIGINT), 'expire' (only in a
-- phase run under the compile time limit: make the limit expire 300 ms
-- into the attempt and let the real engine run, so it is interrupted
-- mid-compile, after it has allocated), 'none' (off). Phase 'check' is the check
-- hooks' test compile of any pattern (idx -1).
CREATE FUNCTION pssc_extract_test_regex_inject(phase text, idx int, action text,
                                               count int DEFAULT 1) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Set this backend's regex compile time limit in ms (<= 0: none; the
-- default is PSSC_REGEX_COMPILE_LIMIT_MS); returns the previous one.
CREATE FUNCTION pssc_extract_test_regex_compile_limit(ms int) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Set a sighup parameter in this backend only (session source), running
-- its check hook here.
CREATE FUNCTION pssc_extract_test_set_local(name text, value text) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many times the injection fired since it was set.
CREATE FUNCTION pssc_extract_test_regex_injected() RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many times the injection's phase (and index) was reached since it was
-- set, fired or not: for a compile phase, the number of attempts.
CREATE FUNCTION pssc_extract_test_regex_attempts() RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Of those, how many engine calls were let run but did not complete (threw,
-- or returned an error such as REG_CANCEL): interrupted mid-compile.
CREATE FUNCTION pssc_extract_test_regex_interrupted() RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Wall-clock ms of the last engine call for the injection's phase (and idx)
-- that completed, measured in the backend around the engine alone; NULL if
-- none since the injection was set. With count 0 an injection fires nothing
-- but still counts attempts and times the engine.
CREATE FUNCTION pssc_extract_test_regex_engine_ms() RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The same engine call's CPU time, in ms (NULL as above).
CREATE FUNCTION pssc_extract_test_regex_engine_cpu_ms() RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How much CPU time the engine uses before the 'expire' injection makes the
-- compile time limit expire (default 300 ms), in this backend: a share of
-- the engine's work, unlike wall-clock time not stretched by a loaded host.
-- Returns the old value.
CREATE FUNCTION pssc_extract_test_regex_expire_ms(ms int) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- This backend's SIGPROF handler and CPU-time timer (ITIMER_PROF), which
-- the 'expire' injection borrows and must give back. 'install' sets a
-- sentinel handler that counts signals and a 1000 s timer (1000 s interval),
-- 'remove' the default handler and no timer, 'pending' makes the next give
-- back find an injection SIGPROF pending. Returns the state after the
-- action ('state': none), e.g.
-- 'handler=sentinel timer=armed interval=1000000 hits=0'.
CREATE FUNCTION pssc_extract_test_sigprof(action text) RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Fail the next count allocating steps of a compile attempt's error
-- handling after its limit fired (-1: all); returns how many failed since
-- the previous call.
CREATE FUNCTION pssc_extract_test_regex_catch_oom(count int) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Backend-local regex runtime bookkeeping (pssc_regex_debug_stats).
CREATE FUNCTION pssc_extract_test_regex_stats(OUT compiles bigint, OUT frees bigint,
                                              OUT live int, OUT failed int)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- This backend's malloc'd bytes in use (glibc >= 2.33 and macOS, NULL
-- elsewhere) and bytes allocated by all its memory contexts.
CREATE FUNCTION pssc_extract_test_mem(OUT malloc_used bigint, OUT context_bytes bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Hold a malloc'd block of the given size (0: none), freeing the previous
-- one: a known allocation for checking malloc_used above.
CREATE FUNCTION pssc_extract_test_malloc_hold(bytes bigint) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
