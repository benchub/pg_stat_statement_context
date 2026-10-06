/*
 * stats_fn.c
 *		pg_stat_statement_context(showtags, merge_buckets): the stats SRF of
 *		DESIGN.md §7, read by the views pg_stat_statement_context (true,
 *		false) and pg_stat_statement_context_totals (true, true); and
 *		pg_stat_statement_context_last_bucket(showtags), read by the view of
 *		the same name: the same columns, for the last closed bucket only.
 *
 * Materialize mode. Reading is in two phases, so the table lock is held only
 * while raw bytes are copied:
 *	1. Under the shared lock (pssc_store_foreach(), which copies each entry
 *	   under its spinlock and first raises current_bucket to the clock), the
 *	   key fields, the encoding, the live slots and, if they will be shown,
 *	   the tag bytes of every entry with a live slot are copied out. Expired
 *	   slots (outside the readers' live window, §5.2) are skipped; nothing in
 *	   the store is written, so an expired slot keeps its contents until a
 *	   writer rolls it over or the entry is reclaimed. Dead entries yield
 *	   nothing.
 *	2. After the lock is released, tags are converted and built as jsonb and
 *	   the rows are added to the tuplestore.
 *
 * Rows: one per live slot (in bucket order) or, with merge_buckets, one per
 * entry holding the sums of its live slots (pssc_slot_merge()), with
 * bucket_start that of the oldest live slot. bucket_start = epoch +
 * bucket_id * bucket_interval. _last_bucket() returns, per entry, only the
 * slot of bucket scan_bucket - 1 (the bucket before the watermark observed
 * at the start of the scan, the same for every entry, so no write can land
 * in it any more), if that slot is still live; entries without one yield
 * nothing. Every row also carries the entry's monotonic counters
 * calls_total, exec_time_total and stats_since (§5.1), which therefore
 * repeat on each bucket row of an entry.
 *
 * Visibility (§6.11), mirroring pg_stat_statements: rows of the calling
 * user (GetUserId()) are always complete; for other roles' rows queryid and
 * tags are NULL unless the caller has the privileges of pg_read_all_stats
 * (has_privs_of_role(), as pgss on PG15+; pgss on PG14 used
 * is_member_of_role(), which also lets NOINHERIT members through). The
 * check is made here, so showtags = false cannot be used to get around it.
 * showtags = false makes tags NULL in every row (and skips copying and
 * building them).
 *
 * Tags are output with pssc_tags_jsonb_noerror() (src/tagout.c): converted
 * from the entry's encoding to the server's, escaped (\xHH, \\) for a
 * SQL_ASCII origin; a tag set from another encoding that cannot be
 * converted is escaped the same way instead of failing the whole read.
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "compat.h"
#include "counters.h"
#include "store.h"
#include "tagout.h"

#define STATS_COLS	11

/* What phase 1 copies out of one entry. */
typedef struct StatsEntry
{
	Oid			userid;
	Oid			dbid;
	int64		queryid;
	bool		toplevel;
	bool		visible;		/* caller may see queryid and tags */
	int			encoding;
	char	   *tags;			/* NULL: tags are not output */
	size_t		tags_len;
	int64		calls_total;
	double		exec_time_total;
	TimestampTz stats_since;
	int			nslots;			/* live slots, >= 1 */
	PsscSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} StatsEntry;

typedef struct StatsCollect
{
	Oid			caller;
	bool		see_all;
	bool		showtags;
	bool		last_only;		/* only the slot of scan_bucket - 1 */
	StatsEntry **entries;
	int			n;
	int			cap;
} StatsCollect;

/* Whether slot s of entry e is output. */
static inline bool
slot_wanted(const StatsCollect *c, const PsscStoreEntryView *e, const PsscSlot *s)
{
	if (c->last_only && s->bucket_id != e->scan_bucket - 1)
		return false;
	return pssc_bucket_is_live(s->bucket_id, e->current_bucket, e->bucket_count);
}

/* Visitor run under the shared lock: copies only, no conversion. */
static void
collect_entry(const PsscStoreEntryView *e, void *arg)
{
	StatsCollect *c = (StatsCollect *) arg;
	StatsEntry *se;
	int			nlive = 0;

	if (pssc_bucket_entry_is_dead(e->last_bucket, e->current_bucket, e->bucket_count))
		return;
	for (int i = 0; i < e->bucket_count; i++)
		nlive += slot_wanted(c, e, &e->slots[i]);
	if (nlive == 0)
		return;

	se = palloc(offsetof(StatsEntry, slots) + nlive * sizeof(PsscSlot));
	se->userid = e->key->userid;
	se->dbid = e->key->dbid;
	se->queryid = e->key->queryid;
	se->toplevel = e->key->toplevel;
	se->visible = c->see_all || e->key->userid == c->caller;
	se->encoding = e->encoding;
	se->calls_total = e->calls_total;
	se->exec_time_total = e->exec_time_total;
	se->stats_since = e->stats_since;
	se->tags = NULL;
	se->tags_len = 0;
	if (c->showtags && se->visible)
	{
		se->tags_len = e->key->tags_len;
		se->tags = palloc(se->tags_len + 1);
		memcpy(se->tags, e->key->tags, se->tags_len);
	}
	se->nslots = 0;
	for (int i = 0; i < e->bucket_count; i++)
		if (slot_wanted(c, e, &e->slots[i]))
			se->slots[se->nslots++] = e->slots[i];

	if (c->n == c->cap)
	{
		c->cap *= 2;
		c->entries = repalloc(c->entries, c->cap * sizeof(StatsEntry *));
	}
	c->entries[c->n++] = se;
}

static int
slot_cmp(const void *a, const void *b)
{
	int64		x = ((const PsscSlot *) a)->bucket_id;
	int64		y = ((const PsscSlot *) b)->bucket_id;

	return (x > y) - (x < y);
}

static void
put_row(ReturnSetInfo *rsinfo, const StatsEntry *se, const PsscSlot *slot,
		Datum tags, bool tags_null)
{
	Datum		values[STATS_COLS];
	bool		nulls[STATS_COLS];
	int			i = 0;

	memset(nulls, 0, sizeof(nulls));
	values[i++] = TimestampTzGetDatum(pssc_store_bucket_start(slot->bucket_id));
	values[i++] = ObjectIdGetDatum(se->userid);
	values[i++] = ObjectIdGetDatum(se->dbid);
	if (se->visible)
		values[i++] = Int64GetDatum(se->queryid);
	else
		nulls[i++] = true;
	values[i++] = BoolGetDatum(se->toplevel);
	values[i] = tags;
	nulls[i++] = tags_null;
	values[i++] = Int64GetDatum(slot->calls);
	values[i++] = Float8GetDatum(slot->total_exec_time);
	values[i++] = Int64GetDatum(se->calls_total);
	values[i++] = Float8GetDatum(se->exec_time_total);
	values[i++] = TimestampTzGetDatum(se->stats_since);
	Assert(i == STATS_COLS);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

static void
stats_srf(FunctionCallInfo fcinfo, bool showtags, bool merge, bool last_only)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StatsCollect c;
	MemoryContext cxt;
	MemoryContext rowcxt;
	MemoryContext oldcxt;

	if (!pssc_store_available())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statement_context must be loaded via \"shared_preload_libraries\"")));

	pssc_init_materialized_srf(fcinfo, 0);
	if (rsinfo->setDesc->natts != STATS_COLS)
		elog(ERROR, "incorrect number of output arguments");

	/* catalog lookups before the lock, as pgss does */
	c.caller = GetUserId();
	c.see_all = has_privs_of_role(c.caller, ROLE_PG_READ_ALL_STATS);
	c.showtags = showtags;
	c.last_only = last_only;
	c.n = 0;
	c.cap = 64;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "pg_stat_statement_context read",
								ALLOCSET_DEFAULT_SIZES);
	rowcxt = AllocSetContextCreate(cxt, "pg_stat_statement_context row",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	c.entries = palloc(c.cap * sizeof(StatsEntry *));

	/* phase 1: copy under the shared lock */
	pssc_store_foreach(collect_entry, &c);

	/* phase 2: build the rows, lock released */
	for (int n = 0; n < c.n; n++)
	{
		StatsEntry *se = c.entries[n];
		Datum		tags = (Datum) 0;
		bool		tags_null = true;

		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(rowcxt);
		MemoryContextSwitchTo(rowcxt);
		if (se->tags != NULL)
		{
			tags = JsonbPGetDatum(pssc_tags_jsonb_noerror(se->tags, se->tags_len,
														  se->encoding, NULL));
			tags_null = false;
		}

		if (merge)
		{
			PsscSlot	sum;

			pssc_slot_init(&sum);
			for (int i = 0; i < se->nslots; i++)
				pssc_slot_merge(&sum, &se->slots[i]);
			put_row(rsinfo, se, &sum, tags, tags_null);
		}
		else
		{
			qsort(se->slots, se->nslots, sizeof(PsscSlot), slot_cmp);
			for (int i = 0; i < se->nslots; i++)
				put_row(rsinfo, se, &se->slots[i], tags, tags_null);
		}
		MemoryContextSwitchTo(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

PG_FUNCTION_INFO_V1(pg_stat_statement_context_1_0);

Datum
pg_stat_statement_context_1_0(PG_FUNCTION_ARGS)
{
	stats_srf(fcinfo, PG_GETARG_BOOL(0), PG_GETARG_BOOL(1), false);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pg_stat_statement_context_last_bucket_1_0);

Datum
pg_stat_statement_context_last_bucket_1_0(PG_FUNCTION_ARGS)
{
	stats_srf(fcinfo, PG_GETARG_BOOL(0), false, true);
	return (Datum) 0;
}
