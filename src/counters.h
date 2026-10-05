/*
 * counters.h
 *		Per-bucket counter slot of a (query x context) entry and the pure
 *		functions that work on it (DESIGN.md §5.1, §5.2, §5.3, §7).
 *
 * The extension is a pg_stat_statements companion: a slot stores only
 * calls and total_exec_time (ms); everything else is left to pgss.
 *
 * Bucket ids (DESIGN.md §5.2) are signed int64 bucket numbers,
 * floor((now - epoch) / bucket_interval). The ids actually written are
 * clamped to the header's current_bucket, which never decreases, so they
 * are non-decreasing over time, and "older" is plain signed comparison (an
 * int64 of intervals cannot wrap). A slot that has never been written holds
 * PSSC_BUCKET_NONE, which is below every real id: it is older than any id
 * written, and is never a contributing (or live) bucket.
 *
 * Backend-independent like tagset.c: test/unit builds counters.c with
 * -DPSSC_STANDALONE. Callers serialize access (the entry spinlock, §5.4);
 * nothing here locks or allocates. The backend-only part at the end is two
 * thin conversions to milliseconds, done exactly as pgss does.
 */
#ifndef PSSC_COUNTERS_H
#define PSSC_COUNTERS_H

#include <stdint.h>
#ifndef true					/* c.h (with stdbool.h) may already define bool */
#include <stdbool.h>
#endif

/* Label of a slot that holds no bucket (never written, or reset). */
#define PSSC_BUCKET_NONE	INT64_MIN

/* ctxSlot of DESIGN.md §5.1; index in the ring = bucket_id mod bucket_count */
typedef struct PsscSlot
{
	int64		bucket_id;		/* absolute bucket number, or PSSC_BUCKET_NONE */
	int64		calls;
	double		total_exec_time;	/* ms */
} PsscSlot;

/* Empty slot: PSSC_BUCKET_NONE, zero counters. */
extern void pssc_slot_init(PsscSlot *slot);

/* Zero the counters and label the slot with bucket_id. */
extern void pssc_slot_relabel(PsscSlot *slot, int64 bucket_id);

/*
 * Ring rollover step (§5.2): relabel the slot for bucket_id if it holds an
 * older bucket (or none). Returns true if it was relabeled. A slot never
 * holds a newer id than the one written (written ids are clamped to
 * current_bucket); that is asserted, and such a slot is left unchanged.
 */
extern bool pssc_slot_roll(PsscSlot *slot, int64 bucket_id);

/* Add one call that took elapsed_ms milliseconds. */
extern void pssc_slot_accum(PsscSlot *slot, double elapsed_ms);

/*
 * merge_buckets (§7): add src's calls and total_exec_time to dst, and keep
 * the oldest contributing bucket_id in dst. Start from pssc_slot_init().
 * An empty src (PSSC_BUCKET_NONE) contributes nothing. Order-independent.
 */
extern void pssc_slot_merge(PsscSlot *dst, const PsscSlot *src);

/*
 * pgss-style usage (§5.3; pg_stat_statements' USAGE_* constants), kept per
 * entry for eviction ordering and never exposed. A new entry starts at
 * PSSC_USAGE_INIT; every recorded call adds PSSC_USAGE_EXEC (so an entry
 * has usage 2.0 after its first call, as a pgss entry does); every eviction
 * pass multiplies every entry's usage by PSSC_USAGE_DECREASE_FACTOR. Entries
 * are created only when a call is recorded, so pgss's "sticky" entries
 * (created at parse time with zero calls) have no counterpart here.
 */
#define PSSC_USAGE_INIT				1.0
#define PSSC_USAGE_EXEC				1.0
#define PSSC_USAGE_DECREASE_FACTOR	0.99

extern double pssc_usage_init(void);
extern void pssc_usage_exec(double *usage);
extern void pssc_usage_decay(double *usage);

/* Seconds to milliseconds, as pgss converts queryDesc->totaltime->total. */
static inline double
pssc_ms_from_seconds(double seconds)
{
	return seconds * 1000.0;
}

#ifndef PSSC_STANDALONE
#include "executor/instrument.h"
#include "portability/instr_time.h"

/*
 * Executor time of a finished statement in ms, from queryDesc->totaltime,
 * exactly as pgss_ExecutorEnd: InstrEndLoop() first (it is a no-op if
 * another hook already ended the loop), then total (s) * 1000.0.
 */
static inline double
pssc_exec_ms_from_totaltime(Instrumentation *totaltime)
{
	InstrEndLoop(totaltime);
	return pssc_ms_from_seconds(totaltime->total);
}

/* A measured utility duration in ms, as pgss_ProcessUtility. */
static inline double
pssc_ms_from_instr_time(instr_time duration)
{
	return INSTR_TIME_GET_MILLISEC(duration);
}
#endif							/* !PSSC_STANDALONE */

#endif							/* PSSC_COUNTERS_H */
