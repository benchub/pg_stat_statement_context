/*
 * activity.h
 *		Per-backend current tags for the pg_stat_statement_context_activity
 *		view (DESIGN.md §7, §8; docs/sql-interface.md).
 *
 * Shared memory holds one slot per regular backend (MaxBackends at startup),
 * indexed like pgstat's backend status array (PSSC_MY_BACKEND_SLOT()). Each
 * slot holds the tags and metadata of its backend's current or last
 * top-level frame (state active or idle), or nothing (pid 0).
 *
 * Only the owning backend writes its slot; readers copy it without locks
 * using a change counter, as PgBackendStatus's st_changecount does: the
 * writer makes the counter odd, writes, and makes it even again, inside a
 * critical section with write barriers; a reader retries until it read the
 * same even value before and after its copy. Writers never wait.
 *
 * The context hooks (context.c) call the pssc_activity_* writers when the
 * top-level frame changes: a top-level frame activated by ExecutorRun or
 * ProcessUtility publishes it as active, its deactivation marks the slot
 * idle, and a top-level statement without a frame clears the slot.
 * ExecutorFinish only resumes the row of the frame published last (it also
 * runs from portal cleanup for portals that never ran). The backend clears its slot at exit
 * (on_shmem_exit).
 */
#ifndef PSSC_ACTIVITY_H
#define PSSC_ACTIVITY_H

#include "export.h"
#include "context.h"

/* Installs the shared memory request and startup hooks (from _PG_init). */
extern void pssc_activity_init(void);

/* Whether the slots exist (the library was preloaded). */
extern PSSC_TEST_API bool pssc_activity_available(void);

/*
 * Publish frame as this backend's active top-level statement, as the
 * current user (GetUserId(): the role it executes as, not the frame's).
 * Returns the publication's sequence number (never 0).
 */
extern PSSC_TEST_API uint64 pssc_activity_publish(const PsscFrame *frame);

/*
 * Mark the row active again if it is still publication seq (0: never);
 * returns whether it did.
 */
extern PSSC_TEST_API bool pssc_activity_resume(uint64 seq);

/* Mark the published statement as ended (no-op if nothing is published). */
extern PSSC_TEST_API void pssc_activity_set_idle(void);

/* Remove this backend's row (no-op if nothing is published). */
extern PSSC_TEST_API void pssc_activity_clear(void);

/* A consistent copy of one slot (pssc_activity_read()). */
typedef struct PsscActivityRow
{
	int			pid;			/* 0: empty slot */
	Oid			userid;
	Oid			dbid;
	int64		queryid;
	int			encoding;
	bool		active;
	uint32		tags_len;
	char	   *tags;			/* caller's buffer, pssc_activity_tags_max() */
} PsscActivityRow;

/* Number of slots, and the tags buffer size a reader needs. */
extern PSSC_TEST_API int pssc_activity_nslots(void);
extern PSSC_TEST_API Size pssc_activity_tags_max(void);

/*
 * Copies slot i into *row (row->tags must point to pssc_activity_tags_max()
 * bytes), retrying while its backend is writing it.
 */
extern PSSC_TEST_API void pssc_activity_read(int i, PsscActivityRow *row);

#endif							/* PSSC_ACTIVITY_H */
