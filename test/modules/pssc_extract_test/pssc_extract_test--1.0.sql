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
-- QueryCancelPending, return REG_CANCEL), 'sleep' (wait up to 60 s in a
-- CHECK_FOR_INTERRUPTS loop, for a real statement_timeout), 'none' (off).
CREATE FUNCTION pssc_extract_test_regex_inject(phase text, idx int, action text,
                                               count int DEFAULT 1) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many times the injection fired since it was set.
CREATE FUNCTION pssc_extract_test_regex_injected() RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Backend-local regex runtime bookkeeping (pssc_regex_debug_stats).
CREATE FUNCTION pssc_extract_test_regex_stats(OUT compiles bigint, OUT frees bigint,
                                              OUT live int, OUT failed int)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- This backend's malloc'd bytes in use (NULL without glibc >= 2.33) and
-- bytes allocated by all its memory contexts.
CREATE FUNCTION pssc_extract_test_mem(OUT malloc_used bigint, OUT context_bytes bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
