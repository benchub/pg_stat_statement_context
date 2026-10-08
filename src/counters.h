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
 * nothing here locks or allocates (pssc_evict_sort() and the eviction
 * selector work in the caller's array). The backend-only part at the end is two
 * thin conversions to milliseconds, done exactly as pgss does.
 */
#ifndef PSSC_COUNTERS_H
#define PSSC_COUNTERS_H

#include <stdint.h>
#ifndef true					/* c.h (with stdbool.h) may already define bool */
#include <stdbool.h>
#endif

#include "export.h"

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

/*
 * Bucket arithmetic (§5.2). Times and the interval are in microseconds
 * (TimestampTz units); interval_us must be > 0, bucket_count >= 1.
 *
 * pssc_bucket_floor_div: floor(a / b) for b > 0, also for negative a (C's
 * "/" truncates toward zero, so -1 / 10 would be bucket 0, not -1).
 *
 * pssc_bucket_for_time: floor((now - epoch) / interval). now - epoch
 * saturates instead of overflowing.
 *
 * pssc_bucket_slot_index: ring index of a (real) bucket id,
 * bucket_id mod bucket_count in [0, bucket_count), also for negative ids.
 *
 * pssc_bucket_is_live: bucket_id (not PSSC_BUCKET_NONE) is in the live
 * window [current - bucket_count + 1, current]. Computed without forming
 * current - bucket_count + 1, so it cannot overflow.
 *
 * pssc_bucket_entry_is_dead: an entry whose newest written bucket
 * (last_bucket) is not live has no live slot at all: it is dead and may be
 * reclaimed (§5.3). An entry that was never written (PSSC_BUCKET_NONE) is
 * dead too.
 *
 * pssc_bucket_start: epoch + bucket_id * interval (the bucket's start).
 */
extern PSSC_TEST_API int64 pssc_bucket_floor_div(int64 a, int64 b);
extern PSSC_TEST_API int64 pssc_bucket_for_time(int64 now_us, int64 epoch_us, int64 interval_us);
extern PSSC_TEST_API int pssc_bucket_slot_index(int64 bucket_id, int bucket_count);
extern PSSC_TEST_API bool pssc_bucket_is_live(int64 bucket_id, int64 current, int bucket_count);
extern PSSC_TEST_API bool pssc_bucket_entry_is_dead(int64 last_bucket, int64 current, int bucket_count);
extern PSSC_TEST_API int64 pssc_bucket_start(int64 bucket_id, int64 epoch_us, int64 interval_us);

/*
 * Ring invariants of one entry (§5.2), given its ring, its last_bucket and
 * the header's current_bucket: every slot is empty (PSSC_BUCKET_NONE with
 * zero counters) or holds an id <= current_bucket that is congruent to its
 * index mod bucket_count and has calls >= 1; last_bucket is <=
 * current_bucket and is the newest id in the ring (PSSC_BUCKET_NONE only
 * while every slot is empty). Returns NULL if they hold; otherwise a static
 * description of the first violation, with *bad_slot set to the slot index
 * (-1 if the violation is not about one slot). Pure: safe under a spinlock.
 */
extern PSSC_TEST_API const char *pssc_ring_check(const PsscSlot *slots, int bucket_count,
											   int64 last_bucket, int64 current_bucket,
											   int *bad_slot);

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

/*
 * Eviction planning (§5.3), pure helpers for store.c's eviction pass.
 *
 * pssc_evict_target: how many entries a pass aims to free,
 * max(1, max_entries * PSSC_EVICT_PERCENT / 100) with integer division
 * (pgss's USAGE_DEALLOC_PERCENT; pgss also frees at least 10, but here
 * max_entries is >= 100, so the floor of 1 only guards tiny values).
 *
 * pssc_evict_live_count: how many live entries to evict once dead_freed
 * dead entries have been reclaimed (all dead entries are always reclaimed,
 * even beyond the target): max(0, target - dead_freed), at most nlive.
 *
 * pssc_evict_cmp / pssc_evict_sort: eviction order of live entries,
 * last_bucket ascending (least recently written first), then usage
 * ascending (least used first), then seq ascending (the order in which the
 * eviction scan met them), so the order is total and the victims of a pass
 * are deterministic. entry is opaque here (the store's hash entry).
 *
 * pssc_evict_select_*: the first k candidates in that order without sorting
 * them all (store.c's eviction pass). The caller's buffer of cap elements
 * holds a max-heap of the cap smallest candidates offered so far: n offers
 * cost O(n log cap) at worst and close to n comparisons in practice, since
 * once the heap is full most candidates lose to its top at once, inline.
 * offer() numbers the candidates (seq) in offer order, starting at 0.
 * finish(k), k <= cap, sorts the kept candidates in place (buf[0] first)
 * and returns min(k, number offered): buf[0 .. that) is then exactly the
 * first that many of pssc_evict_sort() over all the candidates offered.
 * Nothing allocates; the selector is done after finish().
 */
#define PSSC_EVICT_PERCENT	5

typedef struct PsscEvictCandidate
{
	int64		last_bucket;
	double		usage;
	void	   *entry;
	uint64		seq;
} PsscEvictCandidate;

typedef struct PsscEvictSelect
{
	PsscEvictCandidate *buf;
	size_t		cap;
	size_t		size;			/* kept so far, <= cap */
	uint64		next_seq;
} PsscEvictSelect;

extern PSSC_TEST_API int64 pssc_evict_target(int64 max_entries);
extern PSSC_TEST_API int64 pssc_evict_live_count(int64 target, int64 dead_freed, int64 nlive);
extern PSSC_TEST_API int pssc_evict_cmp(const void *a, const void *b);
extern PSSC_TEST_API void pssc_evict_sort(PsscEvictCandidate *cands, size_t n);
extern PSSC_TEST_API void pssc_evict_select_init(PsscEvictSelect *sel,
											   PsscEvictCandidate *buf, size_t cap);
extern PSSC_TEST_API void pssc_evict_select_push(PsscEvictSelect *sel, int64 last_bucket,
											   double usage, void *entry, uint64 seq);
extern PSSC_TEST_API size_t pssc_evict_select_finish(PsscEvictSelect *sel, size_t k);

static inline void
pssc_evict_select_offer(PsscEvictSelect *sel, int64 last_bucket, double usage,
						void *entry)
{
	uint64		seq = sel->next_seq++;

	/*
	 * A full heap keeps the candidate only if it sorts before the top. It
	 * cannot tie with the top: its seq is larger, so equal keys lose too.
	 */
	if (sel->size == sel->cap)
	{
		const PsscEvictCandidate *top = &sel->buf[0];

		if (sel->cap == 0 || last_bucket > top->last_bucket ||
			(last_bucket == top->last_bucket && !(usage < top->usage)))
			return;
	}
	pssc_evict_select_push(sel, last_bucket, usage, entry, seq);
}

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
