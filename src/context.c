/*
 * context.c
 *		Execution frames and active-frame tracking: see context.h
 *		(DESIGN.md §3.1 item 3, §3.2 "Frame lifetime", §6.4, §6.9).
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#include "compat.h"
#include "context.h"
#include "guc.h"
#include "scan.h"

PsscFrame  *pssc_active_frame = NULL;
int			pssc_nesting_level = 0;

/* Registered executor frames, most recently created first. */
static dlist_head frames = DLIST_STATIC_INIT(frames);
static int	nframes = 0;

/*
 * Transaction-end check, armed on TopTransactionContext by the first frame
 * of a transaction. Reset callbacks run when the context is reset or
 * deleted at the very end of commit and abort, after portal cleanup
 * (PreCommit_Portals / AtCleanup_Portals), so every executor of the
 * transaction has been ended or its memory released by then.
 */
static MemoryContextCallback xact_cb;
static bool xact_armed = false;
static PsscFrameXactStats xact_stats;

static planner_hook_type prev_planner = NULL;

/*
 * Owned-start cache (DESIGN.md §6.5): the source text and range end of the
 * last statement of the client's current query string, so its statements
 * find their owned start in O(n) overall. See owned_start().
 */
static const char *owned_src = NULL;
static size_t owned_end = 0;

/* Executor-frame tags are extracted here, then copied into the frame. */
static char extract_buf[PSSC_TAGSET_BYTES_MAX];

static void
xact_end_check(void *arg)
{
	xact_armed = false;
	xact_stats.checks++;
	if (!dlist_is_empty(&frames))
	{
		xact_stats.leaks++;
		Assert(false);
	}
}

static void
frame_unlink(void *arg)
{
	PsscFrame  *frame = (PsscFrame *) arg;

	dlist_delete(&frame->node);
	nframes--;
	if (pssc_active_frame == frame)	/* not expected: hooks restore it */
		pssc_active_frame = NULL;
}

static bool
is_trivia_start(const char *s)
{
	return *s == ';' || *s == ' ' || *s == '\t' || *s == '\n' ||
		*s == '\r' || *s == '\f' || *s == '\v' ||
		(s[0] == '/' && s[1] == '*') || (s[0] == '-' && s[1] == '-');
}

/*
 * Whether a statement with source text src is a statement of the client's
 * query string itself, which uses and advances the owned-start cache (see
 * owned_start()).
 */
static bool
is_client_stmt(const char *src)
{
	return src != NULL && src == debug_query_string &&
		pssc_active_frame == NULL && pssc_nesting_level == 0;
}

/*
 * The owned start of statement r of src (pssc_stmt_owned_start), lexing
 * back at most scan_window bytes. A statement that starts within the first
 * scan_window bytes is always lexed from the string start (exact). Later
 * statements of a top-level multi-statement string start from the end of
 * the previous top-level statement's range, if it is cached for the same
 * string: a parser range ends at a token boundary, so that is a lexically
 * safe point (§6.5; without it PG18, which reports stmt_location at the
 * first token, would lose leading comments past scan_window).
 *
 * Only statements of the client's query string itself (top: src is
 * debug_query_string, at nesting level 0, no frame active) use and update
 * the cache. Other statements run between two of them: SQL run while
 * planning the next one (constant folding; nesting level 1 by the planner
 * hook), nested statements, and EXECUTE'd prepared statements (level 0,
 * but their own saved source text); they would otherwise overwrite the
 * cached boundary. A new query string may reuse the address of the last
 * one, so the cached end must also lie before this statement and start
 * with ';', whitespace or a comment, as it does in the same string; a
 * stale hit can only shift the start of the leading trivia. Statements of
 * the client string that get no frame (PREPARE, EXECUTE, statements run
 * while disabled) advance the cache through
 * pssc_context_note_stmt_boundary().
 */
static size_t
owned_start(const char *src, PsscStmtRange r, bool top)
{
	size_t		window = (size_t) pssc_scan_window;
	size_t		from = 0;
	size_t		start;

	if (top && r.start > window && src == owned_src && owned_end < r.start &&
		is_trivia_start(src + owned_end))
		from = owned_end;
	start = pssc_stmt_owned_start(src, from, r.start, window,
								  standard_conforming_strings);
	if (top)
	{
		owned_src = src;
		owned_end = r.end;
	}
	return start;
}

/* The range of a statement as scanned for tags: its owned range (§6.5). */
static PsscStmtRange
owned_range(const char *src, int stmt_location, int stmt_len, bool top)
{
	PsscStmtRange r = pssc_stmt_range(src, stmt_location, stmt_len);

	r.start = owned_start(src, r, top);
	return r;
}

PsscStmtRange
pssc_stmt_owned_range(const char *src, int stmt_location, int stmt_len)
{
	return owned_range(src, stmt_location, stmt_len, false);
}

static void
set_no_tags(PsscFrame *frame, char *buf)
{
	frame->tags = buf;
	frame->tags_len = 0;
	frame->ntags = 0;
	frame->tags_oom = false;
	frame->tags_hash = pssc_tagset_hash(buf, 0);
}

/*
 * Resolves the tags of a new frame into buf (bufsize bytes) per §6.4 and
 * sets frame->nested and the tag fields (frame->tags = buf).
 */
static void
resolve_tags(PsscFrame *frame, char *buf, size_t bufsize, const char *src,
			 int stmt_location, int stmt_len)
{
	PsscFrame  *active = pssc_active_frame;
	PsscStmtRange r;
	PsscExtractResult res;

	frame->nested = (active != NULL);
	if (active != NULL && pssc_nested_tags != PSSC_NESTED_SCAN)
	{
		if (pssc_nested_tags == PSSC_NESTED_INHERIT &&
			active->tags_len <= bufsize)
		{
			memcpy(buf, active->tags, active->tags_len);
			frame->tags = buf;
			frame->tags_len = active->tags_len;
			frame->ntags = active->ntags;
			frame->tags_hash = active->tags_hash;
			frame->tags_oom = active->tags_oom;
		}
		else
			set_no_tags(frame, buf);
		return;
	}
	if (src == NULL)
	{
		/* no text, so no comment: appname extractors may still tag it */
		pssc_extract_tags("", 0, 0, buf, bufsize, &res);
	}
	else
	{
		r = owned_range(src, stmt_location, stmt_len, is_client_stmt(src));
		pssc_extract_tags(src, r.start, r.end, buf, bufsize, &res);
	}
	frame->tags = buf;
	frame->tags_len = (uint32) res.len;
	frame->ntags = res.ntags;
	frame->tags_hash = res.hash;
	frame->tags_oom = res.oom;
}

static void
set_metadata(PsscFrame *frame, uint64 queryId)
{
	frame->queryId = (int64) queryId;
	frame->dbid = MyDatabaseId;
	frame->encoding = GetDatabaseEncoding();
}

void
pssc_frame_refresh(PsscFrame *frame)
{
	frame->userid = GetUserId();
	frame->nesting_level = pssc_nesting_level;
	frame->toplevel = (pssc_nesting_level == 0);
	frame->recordable = pssc_enabled && frame->queryId != 0 &&
		(pssc_track == PSSC_TRACK_ALL ||
		 (pssc_track == PSSC_TRACK_TOP && frame->toplevel)) &&
		(frame->ntags > 0 || pssc_untagged == PSSC_UNTAGGED_RECORD);
}

PsscFrame *
pssc_frame_create(QueryDesc *queryDesc)
{
	MemoryContext cxt = queryDesc->estate->es_query_cxt;
	const PlannedStmt *pstmt = queryDesc->plannedstmt;
	PsscFrame	tmp;
	PsscFrame  *frame;
	Size		hdr = MAXALIGN(sizeof(PsscFrame));

	Assert(pssc_frame_lookup(queryDesc) == NULL);

	memset(&tmp, 0, sizeof(tmp));
	resolve_tags(&tmp, extract_buf, sizeof(extract_buf),
				 queryDesc->sourceText, pstmt->stmt_location,
				 pstmt->stmt_len);

	frame = MemoryContextAllocExtended(cxt, hdr + tmp.tags_len,
									   MCXT_ALLOC_NO_OOM);
	if (frame == NULL)
		return NULL;
	*frame = tmp;
	frame->tags = (char *) frame + hdr;
	memcpy(frame->tags, extract_buf, tmp.tags_len);
	set_metadata(frame, pstmt->queryId);
	pssc_frame_refresh(frame);

	frame->queryDesc = queryDesc;
	frame->unlink_cb.func = frame_unlink;
	frame->unlink_cb.arg = frame;
	MemoryContextRegisterResetCallback(cxt, &frame->unlink_cb);
	dlist_push_head(&frames, &frame->node);
	nframes++;

	if (!xact_armed && TopTransactionContext != NULL)
	{
		xact_cb.func = xact_end_check;
		xact_cb.arg = NULL;
		MemoryContextRegisterResetCallback(TopTransactionContext, &xact_cb);
		xact_armed = true;
	}
	return frame;
}

PsscFrame *
pssc_frame_lookup(const QueryDesc *queryDesc)
{
	dlist_iter	it;

	dlist_foreach(it, &frames)
	{
		PsscFrame  *frame = dlist_container(PsscFrame, node, it.cur);

		if (frame->queryDesc == queryDesc)
			return frame;
	}
	return NULL;
}

void
pssc_utility_frame_init(PsscUtilityFrame *uf, const PlannedStmt *pstmt,
						const char *queryString)
{
	PsscFrame  *frame = &uf->frame;

	memset(frame, 0, sizeof(*frame));
	frame->utility = true;
	resolve_tags(frame, uf->tagbuf, sizeof(uf->tagbuf), queryString,
				 pstmt->stmt_location, pstmt->stmt_len);
	set_metadata(frame, pstmt->queryId);
	pssc_frame_refresh(frame);
}

void
pssc_context_note_stmt_boundary(const char *src, int stmt_location,
								int stmt_len)
{
	if (!is_client_stmt(src))
		return;
	owned_src = src;
	owned_end = pssc_stmt_range(src, stmt_location, stmt_len).end;
}

void
pssc_frame_enter(PsscFrameSave *save, PsscFrame *frame, bool nest)
{
	save->active = pssc_active_frame;
	save->nesting_level = pssc_nesting_level;
	if (frame != NULL)
		pssc_active_frame = frame;
	if (nest)
		pssc_nesting_level++;
}

void
pssc_frame_leave(const PsscFrameSave *save)
{
	pssc_active_frame = save->active;
	pssc_nesting_level = save->nesting_level;
}

/*
 * Planning is a nesting level, as in pgss on PG17+ (pgss_planner increments
 * its nesting_level around planning even when it does not track planning):
 * statements run by functions evaluated at plan time (constant folding)
 * are not top level. No timing and no frame: such a statement still gets
 * only its own tags (no frame is active while planning). Installed only
 * where pgss does this (PSSC_HAS_PLANNER_NESTING), so toplevel matches
 * pgss's key on every version.
 */
static PlannedStmt *
pssc_planner(PSSC_PLANNER_PARAMS)
{
	PlannedStmt *result;
	int			save_level = pssc_nesting_level;

	pssc_nesting_level++;
	PG_TRY();
	{
		if (prev_planner)
			result = prev_planner(PSSC_PLANNER_ARGS);
		else
			result = standard_planner(PSSC_PLANNER_ARGS);
	}
	PG_FINALLY();
	{
		pssc_nesting_level = save_level;
	}
	PG_END_TRY();
	return result;
}

void
pssc_context_init(void)
{
	if (!PSSC_HAS_PLANNER_NESTING)
		return;
	prev_planner = planner_hook;
	planner_hook = pssc_planner;
}

int
pssc_frame_count(void)
{
	return nframes;
}

void
pssc_frame_xact_stats(PsscFrameXactStats *stats)
{
	*stats = xact_stats;
}
