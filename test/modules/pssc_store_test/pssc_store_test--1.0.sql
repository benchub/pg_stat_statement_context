/* pssc_store_test--1.0.sql -- TEST-ONLY, see test/t/007_store.pl */

\echo Use "CREATE EXTENSION pssc_store_test" to load this file. \quit

-- Build a key and call pssc_store_record(). tags alternates key, value and
-- is serialized as given ("k\0v\0..."); tags_hash defaults to
-- pssc_tagset_hash() of that, dbid/userid to the current database/user.
-- Returns the PsscStoreResult: updated, inserted, found_late, full,
-- unavailable; or 'rejected' if pssc_store_build_key() refused the key.
CREATE FUNCTION pssc_store_test_record(queryid bigint,
                                       tags text[] DEFAULT '{}',
                                       bucket_id bigint DEFAULT 0,
                                       elapsed float8 DEFAULT 1.0,
                                       toplevel bool DEFAULT true,
                                       tags_hash bigint DEFAULT NULL,
                                       dbid oid DEFAULT NULL,
                                       userid oid DEFAULT NULL)
RETURNS text AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT;

-- pssc_store_key_hash() of the key pssc_store_test_record() would build.
CREATE FUNCTION pssc_store_test_key_hash(queryid bigint,
                                         tags text[] DEFAULT '{}',
                                         toplevel bool DEFAULT true,
                                         tags_hash bigint DEFAULT NULL)
RETURNS bigint AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT;

-- One row per written slot of every entry (pssc_store_foreach()).
CREATE FUNCTION pssc_store_test_entries(OUT dbid oid, OUT userid oid,
                                        OUT queryid bigint, OUT toplevel bool,
                                        OUT tags text[], OUT tags_len int,
                                        OUT tags_hash bigint, OUT encoding int,
                                        OUT last_bucket bigint, OUT usage float8,
                                        OUT slot int, OUT bucket_id bigint,
                                        OUT calls bigint,
                                        OUT total_exec_time float8)
RETURNS SETOF record AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Shared header counters and sizes (pssc_store_get_counters()).
CREATE FUNCTION pssc_store_test_counters(OUT entries bigint, OUT max_entries bigint,
                                         OUT dealloc bigint, OUT evicted_entries bigint,
                                         OUT invalid_tags bigint, OUT dropped_tags bigint,
                                         OUT regex_compile_failures bigint,
                                         OUT heuristic_scans bigint,
                                         OUT utility_missing_queryid bigint,
                                         OUT dropped_records bigint,
                                         OUT stats_reset timestamptz,
                                         OUT shmem_bytes bigint, OUT keysize bigint,
                                         OUT entrysize bigint, OUT bucket_count int,
                                         OUT max_tagset_bytes int,
                                         OUT force_collisions bool,
                                         OUT hash_entries bigint)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION pssc_store_test_reset() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Size formulas for arbitrary settings, and for the current settings.
CREATE FUNCTION pssc_store_test_keysize_for(max_tagset_bytes int) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_store_test_entrysize_for(keysize bigint, bucket_count int) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_store_test_shmem_size_for(max_entries int, max_tagset_bytes int,
                                               bucket_count int) RETURNS numeric
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_store_test_shmem_size() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_store_debug_force_collisions().
CREATE FUNCTION pssc_store_test_force_collisions(on_ bool) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- pssc_store_add_tagset_stats() / pssc_store_count_utility_missing_queryid().
CREATE FUNCTION pssc_store_test_add_stats(invalid_tags bigint, dropped_tags bigint,
                                          heuristic_scans bigint,
                                          regex_compile_failures bigint) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION pssc_store_test_utility_missing_queryid() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- One-shot (this backend): the next pssc_store_record() switches forced
-- collisions to on_ after it has hashed its key and before it takes a lock
-- (pssc_store_set_record_test_hook()), i.e. as if another backend had done
-- it while this one stalled.
CREATE FUNCTION pssc_store_test_flip_collisions_in_next_record(on_ bool) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
