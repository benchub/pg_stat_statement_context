/*
 * info_fn.c
 *		pg_stat_statement_context_info(), pg_stat_statement_context_counters()
 *		and pg_stat_statement_context_reset() (DESIGN.md §7).
 *
 * _info() returns one row from a consistent snapshot of the shared header
 * (pssc_store_get_info(): one acquisition of the shared lock, so it is
 * wholly before or wholly after any reset):
 *	entries, max_entries	the table (§5.1)
 *	dealloc, reclaimed_entries, evicted_entries, dropped_records
 *					eviction passes, the dead entries they reclaimed, the
 *					live entries they evicted, and the records lost because
 *					a pass freed nothing (§5.3)
 *	buckets			bucket_count, the ring size (§4.1)
 *	bucket_seconds	bucket_interval, in seconds
 *	oldest_bucket	start of the oldest live slot of any entry, i.e. the
 *					oldest bucket_start the stats view can show; NULL if no
 *					slot is live (empty table, or every entry expired)
 *	current_bucket_start, last_closed_bucket_start
 *					start of the current_bucket watermark (§5.2) and of the
 *					bucket before it, the newest one no write can land in
 *					any more (what the _last_bucket view shows)
 *	shmem_bytes		exactly the size requested at startup (§5.1)
 *	cap_shmem_bytes	exactly the size requested for the separate cardinality
 *					caps table (cardinality_cap_slots; allocated even
 *					when caps are off)
 *	invalid_tags, dropped_tags, heuristic_scans, regex_compile_failures,
 *	utility_missing_queryid
 *	capped_tags, cap_table_full	values collapsed to null by the cardinality
 *					caps (§6.11 step 8), and of those the ones collapsed
 *					because the tracking table was full
 *	stats_reset, stats_reset_epoch	the latter in whole Unix epoch seconds
 *	exemplar_shmem_bytes, exemplar_value_bytes, exemplar_values_dropped
 *					(§6.13) the shared memory of the exemplar
 *					slots (included in shmem_bytes), the bytes a value may
 *					take, and the values dropped as longer than that
 * Finding oldest_bucket scans the whole table under the shared lock, as the
 * stats SRF does, in at most 3 passes (pssc_store_get_info()). Like every
 * reader, _info() first raises current_bucket to the clock.
 *
 * _counters() returns the same row without oldest_bucket, from a copy of the
 * shared header only (pssc_store_get_header()): O(1) whatever the size of
 * the table, for scrapers that call it often.
 *
 * The extraction counters are accumulated per backend and flushed into the
 * header at each ExecutorEnd and utility completion. _info() flushes the
 * calling backend's pending counters first, so a statement sees its own
 * extraction (e.g. a malformed tag in the comment of the very statement
 * that calls _info()); other backends' in-flight statements show up when
 * they end.
 *
 * _reset() (superuser by default: REVOKE ... FROM PUBLIC in the script)
 * first empties the cardinality caps' sets of admitted values (see
 * cardcap.h), so every key may take its cap of distinct values again;
 * doing it first keeps values admitted before the reset out of the emptied
 * store (except those of statements already in flight). It then removes
 * every entry and zeroes every counter under the exclusive lock, and sets
 * stats_reset. Concurrent writers wait for the lock and record after it;
 * that includes each statement's flush of its diagnostic counters (and its
 * utility_missing_queryid), which is added under the shared lock as a
 * whole, so a reset never splits one statement's counters. The calling
 * backend's pending extraction counters belong to the reset statement
 * itself, which started before the reset: they are discarded. Other
 * backends' pending counters (statements in flight during the reset) are
 * added after it, when those statements end. A backend whose regex
 * extractor failed to compile before the reset keeps it disabled until the
 * next configuration change and does not count the failure again, so after
 * a reset regex_compile_failures only counts new failures.
 *
 * All raise ERROR if the library was not preloaded, as the stats SRF does.
 * All are PARALLEL RESTRICTED: the pending counters they flush or discard
 * are those of the leader. The C symbols of _info() and _counters() carry
 * the SQL version whose columns they return, as the SRFs' do.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/timestamp.h"

#include "cardcap.h"
#include "compat.h"
#include "counters.h"
#include "executor.h"
#include "extract.h"
#include "store.h"

#define INFO_COLS	25
#define COUNTERS_COLS	(INFO_COLS - 1)

static void
require_preloaded(void)
{
	if (!pssc_store_available())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statement_context must be loaded via \"shared_preload_libraries\"")));
}

/*
 * The _info() row, or with_oldest false the _counters() row: the same
 * columns without oldest_bucket, and no scan of the table.
 */
static Datum
info_row(FunctionCallInfo fcinfo, bool with_oldest)
{
	TupleDesc	tupdesc;
	Datum		values[INFO_COLS];
	bool		nulls[INFO_COLS];
	PsscStoreCounters c;
	int64		oldest = PSSC_BUCKET_NONE;
	int			ncols = with_oldest ? INFO_COLS : COUNTERS_COLS;
	bool		ok;
	int			i = 0;

	require_preloaded();
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (tupdesc->natts != ncols)
		elog(ERROR, "incorrect number of output arguments");

	pssc_flush_extract_stats();
	if (with_oldest)
		ok = pssc_store_get_info(&c, &oldest);
	else
		ok = pssc_store_get_header(&c);
	if (!ok)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");

	memset(nulls, 0, sizeof(nulls));
	values[i++] = Int64GetDatum(c.entries);
	values[i++] = Int64GetDatum(c.max_entries);
	values[i++] = Int64GetDatum(c.dealloc);
	values[i++] = Int64GetDatum(c.reclaimed_entries);
	values[i++] = Int64GetDatum(c.evicted_entries);
	values[i++] = Int64GetDatum(c.dropped_records);
	values[i++] = Int32GetDatum(c.bucket_count);
	values[i++] = Int32GetDatum((int32) (c.interval_us / USECS_PER_SEC));
	if (with_oldest)
	{
		if (oldest == PSSC_BUCKET_NONE)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(pssc_store_bucket_start(oldest));
	}
	values[i++] = TimestampTzGetDatum(pssc_store_bucket_start(c.current_bucket));
	values[i++] = TimestampTzGetDatum(pssc_store_bucket_start(c.current_bucket - 1));
	values[i++] = Int64GetDatum((int64) c.shmem_bytes);
	values[i++] = Int64GetDatum((int64) pssc_cap_shmem_bytes());
	values[i++] = Int64GetDatum(c.invalid_tags);
	values[i++] = Int64GetDatum(c.dropped_tags);
	values[i++] = Int64GetDatum(c.heuristic_scans);
	values[i++] = Int64GetDatum(c.regex_compile_failures);
	values[i++] = Int64GetDatum(c.utility_missing_queryid);
	values[i++] = Int64GetDatum(c.capped_tags);
	values[i++] = Int64GetDatum(c.cap_table_full);
	values[i++] = TimestampTzGetDatum(c.stats_reset);
	values[i++] = Int64GetDatum(pssc_bucket_floor_div(c.stats_reset, USECS_PER_SEC) +
								(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	values[i++] = Int64GetDatum((int64) c.exemplar_shmem_bytes);
	values[i++] = Int32GetDatum(c.exemplar_value_len);
	values[i++] = Int64GetDatum(c.exemplar_values_dropped);
	Assert(i == ncols);

	tupdesc = BlessTupleDesc(tupdesc);
	return HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));
}

PG_FUNCTION_INFO_V1(pg_stat_statement_context_info_1_0);

Datum
pg_stat_statement_context_info_1_0(PG_FUNCTION_ARGS)
{
	return info_row(fcinfo, true);
}

PG_FUNCTION_INFO_V1(pg_stat_statement_context_counters_1_0);

Datum
pg_stat_statement_context_counters_1_0(PG_FUNCTION_ARGS)
{
	return info_row(fcinfo, false);
}

PG_FUNCTION_INFO_V1(pg_stat_statement_context_reset);

Datum
pg_stat_statement_context_reset(PG_FUNCTION_ARGS)
{
	PsscTagsetStats discard;

	require_preloaded();
	memset(&discard, 0, sizeof(discard));
	pssc_extract_take_stats(&discard);
	pssc_cap_reset();
	pssc_store_reset();
	PG_RETURN_VOID();
}
