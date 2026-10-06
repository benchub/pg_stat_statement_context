/* pssc_guc_test--1.0.sql -- TEST-ONLY, see test/t/003_guc.pl */

\echo Use "CREATE EXTENSION pssc_guc_test" to load this file. \quit

-- Parsed form of pg_stat_statement_context.tags ('tags') or .exclude_tags
-- ('exclude_tags') in this backend.
CREATE FUNCTION pssc_guc_test_list(which text,
                                   OUT match_all bool,
                                   OUT nkeys int,
                                   OUT keys text[])
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_tag_list_find() on that list: 0-based position or -1.
CREATE FUNCTION pssc_guc_test_find(which text, key text) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Backend-local config generation.
CREATE FUNCTION pssc_guc_test_generation() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- C-side GUC variables as "name=value" (enums as their integer codes).
CREATE FUNCTION pssc_guc_test_vars() RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_tag_list_blob_size(nkeys, keybytes) as size_t; NULL when rejected or
-- when an argument exceeds SIZE_MAX. -1 means SIZE_MAX; other negatives error.
CREATE FUNCTION pssc_guc_test_blob_size(nkeys bigint, keybytes bigint) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- glibc version (major.minor) the module was compiled against; NULL if not glibc.
CREATE FUNCTION pssc_guc_test_glibc_version() RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- malloc'd bytes in use in this backend (glibc >= 2.33; NULL elsewhere).
CREATE FUNCTION pssc_guc_test_malloc_used() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Parsed pg_stat_statement_context.extractors in this backend, one canonical
-- line per extractor with every default filled in. With relocate, renders a
-- copy of the blob (checking it holds no pointers into itself) and checks
-- the original renders the same. Errors out if the blob is inconsistent.
CREATE FUNCTION pssc_guc_test_extractors(relocate bool DEFAULT true) RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Size in bytes of that blob.
CREATE FUNCTION pssc_guc_test_extractors_size() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Whether this shared_preload_libraries value lists pg_stat_statements after
-- pg_stat_statement_context (the _PG_init load-order WARNING, src/utility.c).
CREATE FUNCTION pssc_guc_test_load_order_wrong(spl text) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
