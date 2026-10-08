/*
 * context.h
 *		Execution frames and active-frame tracking (DESIGN.md §3.1 item 3,
 *		§3.2 "Frame lifetime", §6.4, §6.9).
 *
 * A frame holds a statement's resolved tag set and the metadata it is
 * recorded under. There are two kinds:
 *
 *	- Executor frames (pssc_frame_create(), from ExecutorStart after
 *	  chaining) live in the executor's es_query_cxt and are registered in a
 *	  backend-local list. A MemoryContextCallback on es_query_cxt unlinks the
 *	  frame when that context is destroyed (normally by ExecutorEnd's
 *	  FreeExecutorState), so frames of failed executors, aborted
 *	  transactions and portals dropped without ExecutorEnd never dangle.
 *	  ExecutorRun/Finish/End find their frame with pssc_frame_lookup(): a
 *	  frame is not pushed at Start and popped at End, because portals are
 *	  suspended and interleaved (one Start, many Runs, one End, in any order
 *	  relative to other portals).
 *	- Utility frames (pssc_utility_frame_init(), from ProcessUtility before
 *	  chaining) are a snapshot in caller-owned storage, normally a local
 *	  PsscUtilityFrame on the hook's stack. Nothing in them points into
 *	  pstmt, the query string or transaction memory, so they stay valid when
 *	  a ROLLBACK or COMMIT inside the utility (CALL, DO) frees those. They are
 *	  never registered.
 *
 * The active frame is the one whose hook is executing (ExecutorRun,
 * ExecutorFinish or ProcessUtility); nested statements (PL/pgSQL, SPI,
 * triggers, CALL/DO bodies) started meanwhile inherit its tags. Hooks
 * change the active frame and nesting level only through
 * pssc_frame_enter()/pssc_frame_leave(), the latter in PG_FINALLY:
 *
 *		PsscFrameSave save;
 *
 *		pssc_frame_enter(&save, pssc_frame_lookup(queryDesc), true);
 *		PG_TRY();
 *			chain;
 *		PG_FINALLY();
 *			pssc_frame_leave(&save);
 *		PG_END_TRY();
 *
 * Tag resolution (§6.4): a frame created while no frame is active (a
 * top-level statement, or one run during the planning of its parent, such
 * as a constant-folded function) gets its own tags, extracted from its own
 * statement range. A frame created while one is active is nested and
 * follows nested_tags: inherit copies the active frame's tags, scan
 * extracts its own, none gets none. Extraction also runs the appname
 * extractors on application_name as it is at that moment (a statement
 * without source text gets only those tags), so an inheriting nested
 * statement keeps the value its top-level statement started with.
 *
 * Under cardinality_cap_scope = role, the caps a tag set obeys depend on
 * the user (§6.1 "Identity"). A frame whose tags are inherited by,
 * published as the activity row of, or recorded under (after
 * pssc_frame_refresh()) another user than the one its caps were applied
 * for gets them re-applied for that user first.
 *
 * Nothing here throws, except that the executor frame is allocated with
 * palloc semantics turned into "no frame" on out-of-memory.
 */
#ifndef PSSC_CONTEXT_H
#define PSSC_CONTEXT_H

#include "export.h"
#include "executor/execdesc.h"
#include "lib/ilist.h"
#include "nodes/plannodes.h"
#include "utils/palloc.h"

#include "extract.h"

typedef struct PsscFrame
{
	/* statement metadata (the store key, DESIGN.md §5.1) */
	int64		queryId;
	Oid			dbid;
	Oid			userid;			/* GetUserId() when made or refreshed */
	int			encoding;		/* database encoding the tags are in */
	int			nesting_level;	/* pssc_nesting_level when made/refreshed */
	bool		toplevel;		/* nesting_level == 0 */

	/*
	 * Generic recording eligibility: enabled, queryId != 0, track allows
	 * the level (top: toplevel only; all: any; none: never) and the untagged
	 * policy allows the tag set. Utility-specific rules (track_utility,
	 * EXECUTE/PREPARE/DEALLOCATE, §6.7) are the caller's.
	 */
	bool		recordable;
	bool		utility;		/* utility frame (snapshot), not executor */
	bool		nested;			/* another frame was active when made */

	/* resolved tag set, "k\0v\0..." (DESIGN.md §5.1) */
	uint64		activity_seq;	/* pssc_activity_publish() of it, 0: none */

	bool		tags_oom;		/* extraction ran out of memory: no tags */
	int			ntags;
	uint32		tags_len;
	uint32		tags_hash;		/* pssc_tagset_hash(tags, tags_len) */
	char	   *tags;

	/* exemplar values (§6.13), as pssc_store_record_ex() takes them */
	uint32		exemplars_len;
	char	   *exemplars;

	/*
	 * Role-scoped cardinality caps (DESIGN.md §6.1 "Identity"): the tags
	 * obey the caps of cap_userid. cands (NULL: the tags do not depend on
	 * the identity) is their input, for a recap (pssc_extract_recap()) when
	 * they are published or recorded under, or inherited by, another user.
	 */
	Oid			cap_userid;
	uint32		cands_len;
	char	   *cands;
	void	   *recap_mem;		/* chunk of a recapped executor frame's tags */

	/* executor frames only */
	const QueryDesc *queryDesc; /* lookup key */
	dlist_node	node;			/* registry link */
	MemoryContextCallback unlink_cb;	/* on es_query_cxt */
} PsscFrame;

/* cands of a utility frame up to this size are stored inline */
#define PSSC_UTILITY_CANDS_INLINE 72

/*
 * Storage for a utility frame snapshot; declare it on the hook's stack and
 * pass it to pssc_utility_frame_release() when done, on error too.
 */
typedef struct PsscUtilityFrame
{
	PsscFrame	frame;
	char		tagbuf[PSSC_TAGSET_BYTES_MAX];
	char		exbuf[PSSC_EXEMPLARS_BUF_MAX];
	char		candbuf[PSSC_UTILITY_CANDS_INLINE];
} PsscUtilityFrame;

/* What pssc_frame_enter() saves and pssc_frame_leave() restores. */
typedef struct PsscFrameSave
{
	PsscFrame  *active;
	int			nesting_level;
	bool		activity;		/* entered as the top-level row (activity.h) */
} PsscFrameSave;

/*
 * The active frame (NULL if none) and the executor/utility/planner nesting
 * level (as pgss's nesting_level: 0 for a top-level statement; ExecutorRun,
 * ExecutorFinish, ProcessUtility and, on PG17+, planning each add one). Read-only
 * outside context.c; change them only with pssc_frame_enter() and
 * pssc_frame_leave().
 */
extern PSSC_TEST_API PsscFrame *pssc_active_frame;
extern PSSC_TEST_API int pssc_nesting_level;

/*
 * Installs the planner hook (from _PG_init, while preloading), which only
 * counts planning as a nesting level, like pgss's planner hook on PG17+:
 * statements run by functions evaluated while planning (constant folding)
 * are not top level. It neither times planning nor activates a frame. On
 * PG14-16, where pgss's toplevel ignores planning, nothing is installed.
 */
extern void pssc_context_init(void);

/*
 * Creates and registers the executor frame of queryDesc, which must have
 * been through standard_ExecutorStart (it allocates in
 * queryDesc->estate->es_query_cxt) and must not have a frame yet. Resolves
 * the tags eagerly. The caller decides whether a frame is wanted (enabled,
 * not a parallel worker, queryId != 0; §3.3). Returns NULL, creating
 * nothing, if memory runs out.
 */
extern PSSC_TEST_API PsscFrame *pssc_frame_create(QueryDesc *queryDesc);

/* The registered frame of queryDesc, or NULL. */
extern PSSC_TEST_API PsscFrame *pssc_frame_lookup(const QueryDesc *queryDesc);

/*
 * Snapshots a utility statement into *uf, before chaining ProcessUtility:
 * pstmt->queryId and the tags of pstmt's range of queryString (NULL
 * queryString: no tags). Afterwards uf->frame does not reference pstmt or
 * queryString. Never fails.
 */
extern PSSC_TEST_API void pssc_utility_frame_init(PsscUtilityFrame *uf,
												const PlannedStmt *pstmt,
												const char *queryString);

/* Frees what *uf allocated outside itself. Never fails. */
extern PSSC_TEST_API void pssc_utility_frame_release(PsscUtilityFrame *uf);

/*
 * The range of statement (stmt_location, stmt_len) of src that the hooks scan
 * for tags: the parser range (pssc_stmt_range) extended back over its owned
 * leading trivia (pssc_stmt_owned_start), lexing back at most scan_window
 * bytes from the string start, as the hooks do for the first statement of
 * a string. It neither uses nor updates the hooks' boundary cache, so for a
 * later statement of a multi-statement string whose start lies more than
 * scan_window bytes in, the hooks may own more leading trivia than this.
 * Used by pg_stat_statement_context_extract().
 */
extern PsscStmtRange pssc_stmt_owned_range(const char *src, int stmt_location,
										   int stmt_len);

/*
 * Advances the PG18 client statement-boundary cache (DESIGN.md §3.2, §6.5)
 * past a statement that gets no frame (EXECUTE, PREPARE, a statement run
 * while disabled or without a queryId), so the leading comment of the next
 * statement of the client's query string stays in reach. Does nothing
 * unless src is the client's query string (debug_query_string) at nesting
 * level 0 with no active frame, the same condition under which making a
 * frame advances it. Never fails.
 */
extern PSSC_TEST_API void pssc_context_note_stmt_boundary(const char *src,
														int stmt_location,
														int stmt_len);

/*
 * Saves the active frame and nesting level in *save, then makes frame the
 * active frame (frame NULL: keep the current one, e.g. for an executor
 * without a frame) and, if nest, increments the nesting level. Pair every
 * call with pssc_frame_leave(save) in PG_FINALLY.
 */
extern PSSC_TEST_API void pssc_frame_enter(PsscFrameSave *save, PsscFrame *frame,
										 bool nest);

/*
 * pssc_frame_enter(save, frame, true) for ExecutorFinish, which also runs
 * from portal cleanup for portals that never ran: at top level it only
 * resumes frame's activity row if that is still the row, and never
 * publishes or clears one.
 */
extern PSSC_TEST_API void pssc_frame_enter_finish(PsscFrameSave *save,
												PsscFrame *frame);
extern PSSC_TEST_API void pssc_frame_leave(const PsscFrameSave *save);

/*
 * Recomputes frame->userid (GetUserId()), nesting_level, toplevel and
 * recordable for the current user and nesting level. pgss takes both when
 * it records (at ExecutorEnd, or after a utility returns), which can differ
 * from those at ExecutorStart (e.g. a cursor opened in a function or under
 * SET ROLE and ended by a later statement, or a SECURITY DEFINER utility);
 * a recording hook calls this first so its key matches pgss's. Re-applies
 * role-scoped caps to the tags for a changed user (see above).
 */
extern PSSC_TEST_API void pssc_frame_refresh(PsscFrame *frame);

/* Number of registered executor frames in this backend. */
extern PSSC_TEST_API int pssc_frame_count(void);

/*
 * Transaction-end check: at the end of every transaction that created an
 * executor frame (when TopTransactionContext is reset or deleted, after
 * portal cleanup), the registry must be empty. checks counts the ends
 * checked, leaks those that found frames still registered. In
 * assert-enabled builds a leak also fails an assertion.
 */
typedef struct PsscFrameXactStats
{
	uint64		checks;
	uint64		leaks;
} PsscFrameXactStats;

extern PSSC_TEST_API void pssc_frame_xact_stats(PsscFrameXactStats *stats);

#endif							/* PSSC_CONTEXT_H */
