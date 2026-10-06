/*
 * pssc_store_test.c
 *		TEST-ONLY module that drives pg_stat_statement_context's shared store
 *		(src/store.h) from SQL for test/t/007_store.pl.
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It reaches the main library's exported
 * functions through load_external_function(), so pg_stat_statement_context
 * must be in shared_preload_libraries.
 */
#include "postgres.h"

#include <sys/stat.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "compat.h"
#include "store.h"

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"

static void *
main_sym(const char *name)
{
	return (void *) load_external_function(MAIN_LIB, name, true, NULL);
}

typedef bool (*build_key_fn) (PsscKey *, Oid, Oid, int64, bool, const char *,
							  size_t, uint32);
typedef PsscStoreResult (*record_fn) (const PsscKey *, double);
typedef PsscStoreResult (*record_at_fn) (const PsscKey *, int64, double);
typedef bool (*buckets_fn) (PsscStoreBuckets *);
typedef int64 (*int64_fn) (void);
typedef TimestampTz (*bucket_start_fn) (int64);
typedef void (*set_clock_fn) (PsscDebugClockMode, int64);
typedef void (*advance_clock_fn) (int64);
typedef bool (*is_live_fn) (int64, int64, int);
typedef uint32 (*key_hash_fn) (const PsscKey *);
typedef uint32 (*tagset_hash_fn) (const char *, size_t);
typedef void (*foreach_fn) (PsscStoreVisitor, void *);
typedef bool (*counters_fn) (PsscStoreCounters *);
typedef void (*void_fn) (void);
typedef void (*bool_fn) (bool);
typedef Size (*size_fn) (void);
typedef Size (*keysize_for_fn) (int);
typedef Size (*entrysize_for_fn) (Size, int);
typedef Size (*shmem_size_for_fn) (int, int, int);
typedef void (*add_stats_fn) (const PsscTagsetStats *);
typedef void (*count_missing_fn) (PsscTagsetStats *);

/*
 * Serializes a text[] of alternating keys and values as "k\0v\0..." into a
 * palloc'd buffer (with no length limit, so oversized sets can be tried).
 */
static char *
serialize_tags(ArrayType *arr, size_t *len)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	StringInfoData buf;

	deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT, &elems, &nulls, &n);
	if (n % 2 != 0)
		elog(ERROR, "tags must alternate keys and values");
	initStringInfo(&buf);
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			elog(ERROR, "tags must not contain NULL");
		appendBinaryStringInfo(&buf, VARDATA_ANY(elems[i]),
							   VARSIZE_ANY_EXHDR(elems[i]));
		appendStringInfoChar(&buf, '\0');
	}
	*len = buf.len;
	return buf.data;
}

/* Builds the key described by the common arguments; false if rejected. */
static bool
build_key(PsscKey *key, int64 queryid, ArrayType *tags_arr, bool toplevel,
		  bool hash_null, int64 tags_hash, Oid dbid, Oid userid)
{
	build_key_fn build = (build_key_fn) main_sym("pssc_store_build_key");
	tagset_hash_fn th = (tagset_hash_fn) main_sym("pssc_tagset_hash");
	size_t		len = 0;
	char	   *tags = tags_arr ? serialize_tags(tags_arr, &len) : pstrdup("");
	uint32		h = hash_null ? th(tags, len) : (uint32) tags_hash;

	return build(key, dbid, userid, queryid, toplevel, tags, len, h);
}

PG_FUNCTION_INFO_V1(pssc_store_test_record);
Datum
pssc_store_test_record(PG_FUNCTION_ARGS)
{
	record_fn record = (record_fn) main_sym("pssc_store_record");
	PsscKeyBuffer kb;
	PsscKey    *key = PSSC_KEY_FROM_BUFFER(&kb);
	PsscStoreResult r;
	const char *s;

	if (PG_ARGISNULL(0))
		elog(ERROR, "queryid must not be NULL");
	if (((size_fn) main_sym("pssc_store_keysize")) () == 0)
		PG_RETURN_TEXT_P(cstring_to_text("unavailable"));
	if (!build_key(key, PG_GETARG_INT64(0),
				   PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1),
				   PG_ARGISNULL(4) ? true : PG_GETARG_BOOL(4),
				   PG_ARGISNULL(5), PG_ARGISNULL(5) ? 0 : PG_GETARG_INT64(5),
				   PG_ARGISNULL(6) ? MyDatabaseId : PG_GETARG_OID(6),
				   PG_ARGISNULL(7) ? GetUserId() : PG_GETARG_OID(7)))
		PG_RETURN_TEXT_P(cstring_to_text("rejected"));

	if (PG_ARGISNULL(2))
		r = record(key, PG_ARGISNULL(3) ? 1.0 : PG_GETARG_FLOAT8(3));
	else
		r = ((record_at_fn) main_sym("pssc_store_record_at"))
			(key, PG_GETARG_INT64(2), PG_ARGISNULL(3) ? 1.0 : PG_GETARG_FLOAT8(3));
	switch (r)
	{
		case PSSC_STORE_UPDATED:
			s = "updated";
			break;
		case PSSC_STORE_INSERTED:
			s = "inserted";
			break;
		case PSSC_STORE_FOUND_LATE:
			s = "found_late";
			break;
		case PSSC_STORE_FULL:
			s = "full";
			break;
		case PSSC_STORE_UNAVAILABLE:
			s = "unavailable";
			break;
		default:
			elog(ERROR, "unexpected store result %d", (int) r);
	}
	PG_RETURN_TEXT_P(cstring_to_text(s));
}

PG_FUNCTION_INFO_V1(pssc_store_test_key_hash);
Datum
pssc_store_test_key_hash(PG_FUNCTION_ARGS)
{
	key_hash_fn kh = (key_hash_fn) main_sym("pssc_store_key_hash");
	PsscKeyBuffer kb;
	PsscKey    *key = PSSC_KEY_FROM_BUFFER(&kb);

	if (PG_ARGISNULL(0))
		elog(ERROR, "queryid must not be NULL");
	if (!build_key(key, PG_GETARG_INT64(0),
				   PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1),
				   PG_ARGISNULL(2) ? true : PG_GETARG_BOOL(2),
				   PG_ARGISNULL(3), PG_ARGISNULL(3) ? 0 : PG_GETARG_INT64(3),
				   MyDatabaseId, GetUserId()))
		elog(ERROR, "key rejected");
	PG_RETURN_INT64((int64) kh(key));
}

typedef struct EntriesState
{
	Tuplestorestate *ts;
	TupleDesc	desc;
	is_live_fn	is_live;
	is_live_fn	is_dead;
} EntriesState;

static void
entries_visit(const PsscStoreEntryView *e, void *arg)
{
	EntriesState *st = (EntriesState *) arg;
	const PsscKey *k = e->key;
	Datum	   *elems;
	ArrayType  *tags;
	int			n = 0;
	size_t		off = 0;

	/* "k\0v\0..." back to text[] (an incomplete trailing item is kept too) */
	elems = palloc(sizeof(Datum) * (k->tags_len + 1));
	while (off < k->tags_len)
	{
		size_t		l = strnlen(k->tags + off, k->tags_len - off);

		elems[n++] = PointerGetDatum(cstring_to_text_with_len(k->tags + off, l));
		off += l + 1;
	}
	tags = construct_array(elems, n, TEXTOID, -1, false, TYPALIGN_INT);

	for (int i = 0; i < e->bucket_count; i++)
	{
		const PsscSlot *s = &e->slots[i];
		Datum		v[17];
		bool		nulls[17] = {0};

		if (s->bucket_id == PSSC_BUCKET_NONE)
			continue;
		v[0] = ObjectIdGetDatum(k->dbid);
		v[1] = ObjectIdGetDatum(k->userid);
		v[2] = Int64GetDatum(k->queryid);
		v[3] = BoolGetDatum(k->toplevel);
		v[4] = PointerGetDatum(tags);
		v[5] = Int32GetDatum(k->tags_len);
		v[6] = Int64GetDatum((int64) k->tags_hash);
		v[7] = Int32GetDatum(e->encoding);
		v[8] = Int64GetDatum(e->last_bucket);
		v[9] = Float8GetDatum(e->usage);
		v[10] = Int32GetDatum(i);
		v[11] = Int64GetDatum(s->bucket_id);
		v[12] = Int64GetDatum(s->calls);
		v[13] = Float8GetDatum(s->total_exec_time);
		v[14] = BoolGetDatum(st->is_live(s->bucket_id, e->current_bucket,
										 e->bucket_count));
		v[15] = BoolGetDatum(st->is_dead(e->last_bucket, e->current_bucket,
										 e->bucket_count));
		v[16] = Int64GetDatum(e->current_bucket);
		tuplestore_putvalues(st->ts, st->desc, v, nulls);
	}
}

PG_FUNCTION_INFO_V1(pssc_store_test_entries);
Datum
pssc_store_test_entries(PG_FUNCTION_ARGS)
{
	foreach_fn fe = (foreach_fn) main_sym("pssc_store_foreach");
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	EntriesState st;
	MemoryContext oldcxt;

	pssc_init_materialized_srf(fcinfo, 0);
	st.ts = rsinfo->setResult;
	st.desc = rsinfo->setDesc;
	st.is_live = (is_live_fn) main_sym("pssc_bucket_is_live");
	st.is_dead = (is_live_fn) main_sym("pssc_bucket_entry_is_dead");
	oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	fe(entries_visit, &st);
	MemoryContextSwitchTo(oldcxt);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pssc_store_test_counters);
Datum
pssc_store_test_counters(PG_FUNCTION_ARGS)
{
	counters_fn get = (counters_fn) main_sym("pssc_store_get_counters");
	PsscStoreCounters c;
	TupleDesc	desc;
	Datum		v[19];
	bool		nulls[19] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (!get(&c))
		elog(ERROR, "store is not set up");
	v[0] = Int64GetDatum(c.entries);
	v[1] = Int64GetDatum(c.max_entries);
	v[2] = Int64GetDatum(c.dealloc);
	v[3] = Int64GetDatum(c.evicted_entries);
	v[4] = Int64GetDatum(c.invalid_tags);
	v[5] = Int64GetDatum(c.dropped_tags);
	v[6] = Int64GetDatum(c.regex_compile_failures);
	v[7] = Int64GetDatum(c.heuristic_scans);
	v[8] = Int64GetDatum(c.utility_missing_queryid);
	v[9] = Int64GetDatum(c.dropped_records);
	v[10] = TimestampTzGetDatum(c.stats_reset);
	v[11] = Int64GetDatum((int64) c.shmem_bytes);
	v[12] = Int64GetDatum((int64) c.keysize);
	v[13] = Int64GetDatum((int64) c.entrysize);
	v[14] = Int32GetDatum(c.bucket_count);
	v[15] = Int32GetDatum(c.max_tagset_bytes);
	v[16] = BoolGetDatum(c.force_collisions);
	v[17] = Int64GetDatum(c.hash_entries);
	v[18] = Int64GetDatum(c.reclaimed_entries);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_store_test_reset);
Datum
pssc_store_test_reset(PG_FUNCTION_ARGS)
{
	((void_fn) main_sym("pssc_store_reset")) ();
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_keysize_for);
Datum
pssc_store_test_keysize_for(PG_FUNCTION_ARGS)
{
	keysize_for_fn f = (keysize_for_fn) main_sym("pssc_store_keysize_for");

	PG_RETURN_INT64((int64) f(PG_GETARG_INT32(0)));
}

PG_FUNCTION_INFO_V1(pssc_store_test_entrysize_for);
Datum
pssc_store_test_entrysize_for(PG_FUNCTION_ARGS)
{
	entrysize_for_fn f = (entrysize_for_fn) main_sym("pssc_store_entrysize_for");

	PG_RETURN_INT64((int64) f((Size) PG_GETARG_INT64(0), PG_GETARG_INT32(1)));
}

PG_FUNCTION_INFO_V1(pssc_store_test_shmem_size_for);
Datum
pssc_store_test_shmem_size_for(PG_FUNCTION_ARGS)
{
	shmem_size_for_fn f = (shmem_size_for_fn) main_sym("pssc_store_shmem_size_for");
	char		buf[32];

	snprintf(buf, sizeof(buf), "%llu",
			 (unsigned long long) f(PG_GETARG_INT32(0), PG_GETARG_INT32(1),
									PG_GETARG_INT32(2)));
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

PG_FUNCTION_INFO_V1(pssc_store_test_shmem_size);
Datum
pssc_store_test_shmem_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) ((size_fn) main_sym("pssc_store_shmem_size")) ());
}

PG_FUNCTION_INFO_V1(pssc_store_test_force_collisions);
Datum
pssc_store_test_force_collisions(PG_FUNCTION_ARGS)
{
	((bool_fn) main_sym("pssc_store_debug_force_collisions")) (PG_GETARG_BOOL(0));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_add_stats);
Datum
pssc_store_test_add_stats(PG_FUNCTION_ARGS)
{
	add_stats_fn f = (add_stats_fn) main_sym("pssc_store_add_tagset_stats");
	PsscTagsetStats s = {0};

	s.invalid_tags = (uint64) PG_GETARG_INT64(0);
	s.dropped_tags = (uint64) PG_GETARG_INT64(1);
	s.heuristic_scans = (uint64) PG_GETARG_INT64(2);
	s.regex_compile_failures = (uint64) PG_GETARG_INT64(3);
	f(&s);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_utility_missing_queryid);
Datum
pssc_store_test_utility_missing_queryid(PG_FUNCTION_ARGS)
{
	((count_missing_fn) main_sym("pssc_store_count_utility_missing_queryid")) (NULL);
	PG_RETURN_VOID();
}

typedef void (*set_hook_fn) (PsscStoreRecordTestHook, void *);

static bool flip_to;

/* One-shot: runs inside the next pssc_store_record(), after hashing. */
static void
flip_hook(void *arg)
{
	((set_hook_fn) main_sym("pssc_store_set_record_test_hook")) (NULL, NULL);
	((bool_fn) main_sym("pssc_store_debug_force_collisions")) (flip_to);
}

PG_FUNCTION_INFO_V1(pssc_store_test_flip_collisions_in_next_record);
Datum
pssc_store_test_flip_collisions_in_next_record(PG_FUNCTION_ARGS)
{
	flip_to = PG_GETARG_BOOL(0);
	((set_hook_fn) main_sym("pssc_store_set_record_test_hook")) (flip_hook, NULL);
	PG_RETURN_VOID();
}

static char stall_release_file[MAXPGPATH];

/*
 * One-shot: runs inside the next diagnostic-counter flush, under the store
 * lock, and sleeps (wait event PgSleep) until the release file exists or
 * two minutes have passed.
 */
static void
stall_flush_hook(void *arg)
{
	struct stat st;
	int			i;

	((set_hook_fn) main_sym("pssc_store_set_flush_test_hook")) (NULL, NULL);
	for (i = 0; i < 12000 && stat(stall_release_file, &st) != 0; i++)
		(void) WaitLatch(MyLatch, WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10L,
						 WAIT_EVENT_PG_SLEEP);
}

PG_FUNCTION_INFO_V1(pssc_store_test_stall_next_flush);
Datum
pssc_store_test_stall_next_flush(PG_FUNCTION_ARGS)
{
	strlcpy(stall_release_file, text_to_cstring(PG_GETARG_TEXT_PP(0)), MAXPGPATH);
	((set_hook_fn) main_sym("pssc_store_set_flush_test_hook")) (stall_flush_hook, NULL);
	PG_RETURN_VOID();
}

/*
 * One-shot: runs inside the next _info() scan, under the shared store lock,
 * after the first entry has been judged, and sleeps (wait event PgSleep)
 * until the release file exists or two minutes have passed.
 */
static void
stall_info_scan_hook(void *arg)
{
	struct stat st;
	int			i;

	((set_hook_fn) main_sym("pssc_store_set_info_scan_test_hook")) (NULL, NULL);
	for (i = 0; i < 12000 && stat(stall_release_file, &st) != 0; i++)
		(void) WaitLatch(MyLatch, WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10L,
						 WAIT_EVENT_PG_SLEEP);
}

PG_FUNCTION_INFO_V1(pssc_store_test_stall_next_info_scan);
Datum
pssc_store_test_stall_next_info_scan(PG_FUNCTION_ARGS)
{
	strlcpy(stall_release_file, text_to_cstring(PG_GETARG_TEXT_PP(0)), MAXPGPATH);
	((set_hook_fn) main_sym("pssc_store_set_info_scan_test_hook")) (stall_info_scan_hook, NULL);
	PG_RETURN_VOID();
}

/* ------------------------------------------------- time buckets (§5.2) */

PG_FUNCTION_INFO_V1(pssc_store_test_buckets);
Datum
pssc_store_test_buckets(PG_FUNCTION_ARGS)
{
	PsscStoreBuckets b;
	TupleDesc	desc;
	Datum		v[10];
	bool		nulls[10] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (!((buckets_fn) main_sym("pssc_store_get_buckets")) (&b))
		elog(ERROR, "store is not set up");
	v[0] = TimestampTzGetDatum(b.epoch);
	v[1] = Int64GetDatum(b.interval_us);
	v[2] = Int32GetDatum(b.bucket_count);
	v[3] = Int64GetDatum(b.current_bucket);
	v[4] = Int64GetDatum(b.clock_bucket);
	v[5] = Int64GetDatum(b.reader_bucket);
	v[6] = TimestampTzGetDatum(b.now);
	v[7] = Int64GetDatum(b.advances);
	v[8] = CStringGetTextDatum(b.clock_mode == PSSC_CLOCK_PINNED ? "pinned" : "real");
	v[9] = Int64GetDatum(b.clock_value);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_store_test_clock_bucket);
Datum
pssc_store_test_clock_bucket(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(((int64_fn) main_sym("pssc_store_clock_bucket")) ());
}

PG_FUNCTION_INFO_V1(pssc_store_test_bucket_start);
Datum
pssc_store_test_bucket_start(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(((bucket_start_fn) main_sym("pssc_store_bucket_start"))
						  (PG_GETARG_INT64(0)));
}

PG_FUNCTION_INFO_V1(pssc_store_test_set_clock_offset);
Datum
pssc_store_test_set_clock_offset(PG_FUNCTION_ARGS)
{
	((set_clock_fn) main_sym("pssc_store_debug_set_clock"))
		(PSSC_CLOCK_REAL, PG_GETARG_INT64(0));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_pin_clock);
Datum
pssc_store_test_pin_clock(PG_FUNCTION_ARGS)
{
	((set_clock_fn) main_sym("pssc_store_debug_set_clock"))
		(PSSC_CLOCK_PINNED, (int64) PG_GETARG_TIMESTAMPTZ(0));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_advance_clock);
Datum
pssc_store_test_advance_clock(PG_FUNCTION_ARGS)
{
	((advance_clock_fn) main_sym("pssc_store_debug_advance_clock")) (PG_GETARG_INT64(0));
	PG_RETURN_VOID();
}

/* current_bucket last seen by this backend, for the never-decreases check */
static int64 seen_current_bucket = PG_INT64_MIN;

PG_FUNCTION_INFO_V1(pssc_store_test_check_invariants);
Datum
pssc_store_test_check_invariants(PG_FUNCTION_ARGS)
{
	PsscStoreBuckets b;
	int64		n;

	n = ((int64_fn) main_sym("pssc_store_check_invariants")) ();
	if (!((buckets_fn) main_sym("pssc_store_get_buckets")) (&b))
		elog(ERROR, "store is not set up");
	if (b.current_bucket < seen_current_bucket)
		elog(ERROR, "current_bucket went backwards: " INT64_FORMAT " after " INT64_FORMAT,
			 b.current_bucket, seen_current_bucket);
	seen_current_bucket = b.current_bucket;
	PG_RETURN_INT64(n);
}

static int64 stall_advance_us;
static int64 stall_queryid;
static bool stall_other;

/*
 * One-shot: runs inside the next pssc_store_record(), after it computed its
 * bucket id and before it locks: another "backend" moves the clock and
 * records stall_queryid (unless NULL), which advances current_bucket. The
 * stalled record then continues with its stale id.
 */
static void
stall_hook(void *arg)
{
	PsscKeyBuffer kb;
	PsscKey    *key = PSSC_KEY_FROM_BUFFER(&kb);

	((set_hook_fn) main_sym("pssc_store_set_record_test_hook")) (NULL, NULL);
	((advance_clock_fn) main_sym("pssc_store_debug_advance_clock")) (stall_advance_us);
	if (!stall_other)
		return;
	if (!build_key(key, stall_queryid, NULL, true, true, 0, MyDatabaseId, GetUserId()))
		elog(ERROR, "key rejected");
	((record_fn) main_sym("pssc_store_record")) (key, 1.0);
}

PG_FUNCTION_INFO_V1(pssc_store_test_stall_next_record);
Datum
pssc_store_test_stall_next_record(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		elog(ERROR, "advance_us must not be NULL");
	stall_advance_us = PG_GETARG_INT64(0);
	stall_other = !PG_ARGISNULL(1);
	stall_queryid = stall_other ? PG_GETARG_INT64(1) : 0;
	((set_hook_fn) main_sym("pssc_store_set_record_test_hook")) (stall_hook, NULL);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_store_test_fail_next_eviction_alloc);
Datum
pssc_store_test_fail_next_eviction_alloc(PG_FUNCTION_ARGS)
{
	((void_fn) main_sym("pssc_store_debug_fail_next_eviction_alloc")) ();
	PG_RETURN_VOID();
}

/* ------------------------------------------- cardinality caps (cardcap.h) */

typedef void (*set_cap_hook_fn) (void (*) (void *), void *);

PG_FUNCTION_INFO_V1(pssc_store_test_cap_near_wrap);
Datum
pssc_store_test_cap_near_wrap(PG_FUNCTION_ARGS)
{
	((void_fn) main_sym("pssc_cap_test_near_wrap")) ();
	PG_RETURN_VOID();
}

static char cap_release_file[MAXPGPATH];

static void
stall_cap_clear_hook(void *arg)
{
	struct stat st;

	for (int i = 0; i < 12000 && stat(cap_release_file, &st) != 0; i++)
	{
		(void) WaitLatch(MyLatch, WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10L,
						 WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
	}
}

PG_FUNCTION_INFO_V1(pssc_store_test_stall_next_cap_clear);
Datum
pssc_store_test_stall_next_cap_clear(PG_FUNCTION_ARGS)
{
	strlcpy(cap_release_file, text_to_cstring(PG_GETARG_TEXT_PP(0)), MAXPGPATH);
	((set_cap_hook_fn) main_sym("pssc_cap_set_reset_test_hook")) (stall_cap_clear_hook, NULL);
	PG_RETURN_VOID();
}
