/*
 * utility.c
 *		ProcessUtility hook (DESIGN.md §3.2, §6.6, §6.7, §6.9).
 *
 * EXECUTE and PREPARE are passed through: no frame, no nesting level, never
 * recorded. The executor records the plan EXECUTE runs, at the caller's
 * level (so it is top level when EXECUTE is), with the tags of the prepared
 * text (§6.3); this matches pgss, which neither tracks them nor bumps its
 * nesting level for them. They only advance the PG18 statement-boundary
 * cache (pssc_context_note_stmt_boundary()), so a long one does not push
 * the next statement's leading comment out of scan_window.
 *
 * Every other utility, before chaining:
 *	- decides whether it is tracked, as pgss does at that point: enabled,
 *	  track_utility, track allows the current nesting level, and it is a
 *	  utility pgss records (DEALLOCATE only on PG17+,
 *	  PSSC_PGSS_RECORDS_DEALLOCATE);
 *	- snapshots queryId, statement bounds and tags into a utility frame on
 *	  this function's stack (pssc_utility_frame_init(); if enabled), which
 *	  never points into pstmt or the query string;
 *	- makes that frame active around the chained call, even when it is not
 *	  tracked, so CALL/DO children inherit its tags (§6.7);
 *	- counts a nesting level around the chained call exactly when pgss
 *	  does, so a child's toplevel matches pgss's key: on PG17+ always; on
 *	  PG14-16 only when pgss tracks the utility under pgss's own settings
 *	  (pg_stat_statements.track_utility and .track, read by name; this
 *	  extension's track_utility and track when pgss is not loaded),
 *	  PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES;
 *	  frame and level are restored in PG_FINALLY.
 * Whether the utility is recorded is decided by this extension's settings
 * alone.
 * The elapsed time is measured around the chained call, as pgss measures
 * total_exec_time. Afterwards pstmt is never read again: a COMMIT or
 * ROLLBACK inside the utility (CALL, DO, transaction statements) may have
 * freed it. A tracked utility is refreshed (pssc_frame_refresh(): user,
 * level, toplevel and recordability as pgss reads them then) and recorded
 * from the snapshot, unless its queryId is 0: pg_stat_statements loaded
 * after this extension runs its hook outside ours and zeroes
 * pstmt->queryId first, so such a utility is counted in
 * utility_missing_queryid instead (whatever its tags). pstmt->queryId is
 * never modified here, since pgss inside this hook relies on it.
 * Statements that fail are not counted (§6.9).
 */
#include "postgres.h"

#include "access/parallel.h"
#include "nodes/parsenodes.h"
#include "portability/instr_time.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "compat.h"
#include "context.h"
#include "executor.h"
#include "guc.h"
#include "store.h"
#include "utility.h"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static bool
is_execute_or_prepare(Node *parsetree)
{
	return IsA(parsetree, ExecuteStmt) || IsA(parsetree, PrepareStmt);
}

/* pgss's PGSS_HANDLED_UTILITY on this server version. */
static bool
pgss_records_utility(Node *parsetree)
{
	if (is_execute_or_prepare(parsetree))
		return false;
	if (!PSSC_PGSS_RECORDS_DEALLOCATE && IsA(parsetree, DeallocateStmt))
		return false;
	return true;
}

/* pgss_enabled(level) for this extension's track setting. */
static bool
tracked_at_level(int level)
{
	return pssc_track == PSSC_TRACK_ALL ||
		(pssc_track == PSSC_TRACK_TOP && level == 0);
}

/*
 * The value of another extension's GUC, or NULL when it is not defined or
 * only a placeholder (its library is not loaded).
 */
static const char *
loaded_guc(const char *name)
{
	if (GetConfigOptionFlags(name, true) & GUC_CUSTOM_PLACEHOLDER)
		return NULL;
	return GetConfigOption(name, true, false);
}

/*
 * pgss's track_utility and track, falling back to this extension's when
 * pgss is not loaded. Read per utility: GetConfigOption() is a lookup by
 * name, cheap next to a utility statement.
 */
static void
pgss_utility_settings(bool *track_utility, int *track)
{
	const char *v;

	*track_utility = pssc_track_utility;
	*track = pssc_track;

	v = loaded_guc("pg_stat_statements.track_utility");
	if (v != NULL)
		(void) parse_bool(v, track_utility);

	v = loaded_guc("pg_stat_statements.track");
	if (v != NULL)
	{
		if (strcmp(v, "all") == 0)
			*track = PSSC_TRACK_ALL;
		else if (strcmp(v, "top") == 0)
			*track = PSSC_TRACK_TOP;
		else if (strcmp(v, "none") == 0)
			*track = PSSC_TRACK_NONE;
	}
}

/*
 * Whether pgss counts this utility (not EXECUTE/PREPARE) as a nesting level
 * at the given level: always on PG17+; on PG14-16 only when it tracks it
 * (pgss_track_utility && pgss_enabled(level) && PGSS_HANDLED_UTILITY).
 */
static bool
pgss_nests_utility(Node *parsetree, int level)
{
	bool		track_utility;
	int			track;

	if (!PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES)
		return true;
	pgss_utility_settings(&track_utility, &track);
	return track_utility && !IsParallelWorker() &&
		(track == PSSC_TRACK_ALL || (track == PSSC_TRACK_TOP && level == 0)) &&
		pgss_records_utility(parsetree);
}

static void
chain(PSSC_PROCESS_UTILITY_PARAMS)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
	else
		standard_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
}

static void
record_frame(const PsscFrame *frame, double elapsed_ms)
{
	PsscKeyBuffer kb;
	PsscKey    *key = PSSC_KEY_FROM_BUFFER(&kb);

	if (!pssc_store_build_key(key, frame->dbid, frame->userid,
							  frame->queryId, frame->toplevel,
							  frame->tags, frame->tags_len,
							  frame->tags_hash))
		return;
	(void) pssc_store_record(key, elapsed_ms);
}

static void
pssc_ProcessUtility(PSSC_PROCESS_UTILITY_PARAMS)
{
	Node	   *parsetree = pstmt->utilityStmt;
	PsscUtilityFrame uf;
	PsscFrame  *frame = NULL;
	PsscFrameSave save;
	bool		tracked;
	bool		nest;
	instr_time	start;
	instr_time	duration;

	if (is_execute_or_prepare(parsetree))
	{
		pssc_context_note_stmt_boundary(queryString, pstmt->stmt_location,
										pstmt->stmt_len);
		chain(PSSC_PROCESS_UTILITY_ARGS);
		return;
	}

	tracked = pssc_enabled && pssc_track_utility &&
		tracked_at_level(pssc_nesting_level) &&
		pgss_records_utility(parsetree);
	nest = pgss_nests_utility(parsetree, pssc_nesting_level);

	if (pssc_enabled && !IsParallelWorker())
	{
		pssc_utility_frame_init(&uf, pstmt, queryString);
		frame = &uf.frame;
	}
	else
		pssc_context_note_stmt_boundary(queryString, pstmt->stmt_location,
										pstmt->stmt_len);

	INSTR_TIME_SET_ZERO(start);
	if (tracked)
		INSTR_TIME_SET_CURRENT(start);

	pssc_frame_enter(&save, frame, nest);
	PG_TRY();
	{
		chain(PSSC_PROCESS_UTILITY_ARGS);
	}
	PG_FINALLY();
	{
		pssc_frame_leave(&save);
	}
	PG_END_TRY();

	/* pstmt may be gone (COMMIT/ROLLBACK): only the snapshot from here on. */
	if (tracked && frame != NULL)
	{
		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		pssc_frame_refresh(frame);
		if (frame->queryId == 0)
			pssc_store_count_utility_missing_queryid();
		else if (frame->recordable)
			record_frame(frame, INSTR_TIME_GET_MILLISEC(duration));
	}
	if (frame != NULL)
		pssc_flush_extract_stats();
}

void
pssc_utility_init(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pssc_ProcessUtility;
}
