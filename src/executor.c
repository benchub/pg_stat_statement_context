/*
 * executor.c
 *		ExecutorStart/Run/Finish/End hooks (DESIGN.md §3.2, §3.3, §6.8).
 *
 * ExecutorStart chains first, then, if the extension is enabled, this is
 * not a parallel worker (the leader's time covers the workers, §6.8) and
 * queryId != 0, creates the executor frame (src/context.h), which resolves
 * the tags eagerly so nested statements can inherit them. Like pgss, it
 * then sets up queryDesc->totaltime (InstrAlloc(1, INSTRUMENT_ALL, false)
 * in es_query_cxt, the same on PG14-18) if nobody did and the statement is
 * tracked at the current nesting level. INSTRUMENT_ALL, not just a timer:
 * whoever allocates totaltime first decides what every hook gets, and
 * pgss reads buffer and WAL usage from it.
 *
 * ExecutorRun and ExecutorFinish make the frame active and count a
 * nesting level around the chained call (pssc_frame_enter/leave).
 *
 * ExecutorEnd refreshes the frame (user, nesting level, toplevel and
 * recordability as they are now, as pgss reads them) and records one call
 * and the executor time when, as in pgss_ExecutorEnd, totaltime is set and
 * the statement is tracked at the current level, and the untagged policy
 * allows its tag set (pssc_frame_refresh()). It then flushes this
 * backend's extraction counters into the shared header, and chains.
 *
 * Load order relative to pg_stat_statements: the recommended
 * shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'
 * makes these hooks the outermost (§3.2 "Load order"). The executor hooks
 * work in either order: pgss never changes plannedstmt->queryId in the
 * executor, both read the same queryDesc->totaltime (whichever hook runs
 * first allocates it with pgss's options; InstrEndLoop is idempotent), and
 * each counts its own nesting level. Only the utility hook (item -18)
 * depends on the order.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "compat.h"
#include "context.h"
#include "counters.h"
#include "executor.h"
#include "extract.h"
#include "guc.h"
#include "store.h"

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* pgss_enabled(level) for this extension's track setting. */
static bool
tracked_at_level(int level)
{
	return pssc_track == PSSC_TRACK_ALL ||
		(pssc_track == PSSC_TRACK_TOP && level == 0);
}

static void
pssc_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	PsscFrame  *frame;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (!pssc_enabled || IsParallelWorker() ||
		queryDesc->plannedstmt->queryId == 0)
		return;

	frame = pssc_frame_create(queryDesc);
	if (frame == NULL)
		return;

	if (queryDesc->totaltime == NULL && tracked_at_level(pssc_nesting_level))
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
		MemoryContextSwitchTo(oldcxt);
	}
}

static void
pssc_ExecutorRun(PSSC_EXECUTOR_RUN_PARAMS)
{
	PsscFrameSave save;

	pssc_frame_enter(&save, pssc_frame_lookup(queryDesc), true);
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
		else
			standard_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
	}
	PG_FINALLY();
	{
		pssc_frame_leave(&save);
	}
	PG_END_TRY();
}

static void
pssc_ExecutorFinish(QueryDesc *queryDesc)
{
	PsscFrameSave save;

	pssc_frame_enter(&save, pssc_frame_lookup(queryDesc), true);
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		pssc_frame_leave(&save);
	}
	PG_END_TRY();
}

static void
record_frame(QueryDesc *queryDesc, PsscFrame *frame)
{
	PsscKeyBuffer kb;
	PsscKey    *key = PSSC_KEY_FROM_BUFFER(&kb);

	if (!pssc_store_build_key(key, frame->dbid, frame->userid,
							  frame->queryId, frame->toplevel,
							  frame->tags, frame->tags_len,
							  frame->tags_hash))
		return;
	(void) pssc_store_record(key,
							 pssc_exec_ms_from_totaltime(queryDesc->totaltime));
}

static void
flush_extract_stats(void)
{
	PsscTagsetStats stats;

	memset(&stats, 0, sizeof(stats));
	pssc_extract_take_stats(&stats);
	if (stats.invalid_tags || stats.dropped_tags || stats.heuristic_scans ||
		stats.regex_compile_failures)
		pssc_store_add_tagset_stats(&stats);
}

static void
pssc_ExecutorEnd(QueryDesc *queryDesc)
{
	PsscFrame  *frame = pssc_frame_lookup(queryDesc);

	if (frame != NULL)
	{
		pssc_frame_refresh(frame);
		if (frame->recordable && queryDesc->totaltime != NULL)
			record_frame(queryDesc, frame);
	}
	flush_extract_stats();

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
pssc_executor_init(void)
{
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pssc_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pssc_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pssc_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pssc_ExecutorEnd;
}
