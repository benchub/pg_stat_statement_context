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
                                  OUT heuristic_scans bigint)
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
                                  OUT heuristic_scans bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_tagset_hash() of raw bytes.
CREATE FUNCTION pssc_extract_test_hash(data bytea) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Install (true) or remove (false) a fake regex extractor hook in this
-- backend: capture group i (key i of the extractor) is the i-th
-- whitespace-separated word of the comment body; missing words give no pair.
CREATE FUNCTION pssc_extract_test_fake_regex(enable bool) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
