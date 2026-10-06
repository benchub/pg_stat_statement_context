/*
 * store.c
 *		Shared store (DESIGN.md §3.1 item 4, §5.1, §5.4): shared memory
 *		sizing and setup, the (query x context) hash table, recording into
 *		the per-entry bucket ring, header counters and reset. See store.h.
 *
 * Entry layout in the hash table (offsets are MAXALIGNed):
 *	[0, keysize)						PsscKey (dynahash key)
 *	[keysize, + MAXALIGN(header))		PsscEntryHeader
 *	[..., + bucket_count * PsscSlot)	the ring; index = bucket_id mod count
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "guc.h"
#include "store.h"

#define PSSC_STORE_NAME			"pg_stat_statement_context"
#define PSSC_STORE_HASH_NAME	"pg_stat_statement_context hash"
#define PSSC_LWLOCK_TRANCHE		"pg_stat_statement_context"

/* Per-entry state after the key (ctxEntry of DESIGN.md §5.1). */
typedef struct PsscEntryHeader
{
	slock_t		mutex;			/* protects everything below and the slots */
	int			encoding;		/* of tags[] (that of key.dbid) */
	int64		last_bucket;	/* newest bucket_id written */
	double		usage;			/* pgss-style, for eviction (§5.3) */
	/* monotonic since stats_since, whatever bucket each call landed in */
	int64		calls_total;
	double		exec_time_total;	/* ms */
	TimestampTz stats_since;	/* when the entry was created */
} PsscEntryHeader;

/* Shared header. */
typedef struct PsscSharedState
{
	LWLock	   *lock;			/* protects the hash table */

	/* fixed at startup */
	int			max_entries;
	int			bucket_count;
	int			max_tagset_bytes;
	Size		keysize;
	Size		entrysize;
	Size		shmem_bytes;	/* exactly what was requested */

	/* changed only under the exclusive lock while the table is empty */
	bool		force_collisions;

	/* time buckets (§5.2): fixed at startup */
	TimestampTz epoch;			/* bucket 0 starts here */
	int64		interval_us;

	/*
	 * current_bucket: the shared monotonic watermark of the newest bucket
	 * observed by any writer or reader (an int64 stored in a uint64; see
	 * watermark_advance()). Lock-free; it never decreases.
	 */
	pg_atomic_uint64 current_bucket;
	pg_atomic_uint64 bucket_advances;

	/*
	 * Debug clock (tests only, store.h). clock_debug is read without a lock
	 * on every clock read; only when it is set are clock_mode and
	 * clock_value read, together, under clock_mutex.
	 */
	pg_atomic_uint32 clock_debug;
	slock_t		clock_mutex;
	PsscDebugClockMode clock_mode;
	int64		clock_value;

	/* under the lock (exclusive to change) */
	int64		entries;
	int64		dealloc;		/* eviction passes (§5.3) */
	int64		reclaimed_entries;	/* dead entries they reclaimed */
	int64		evicted_entries;	/* live entries they evicted */
	TimestampTz stats_reset;

	/* updated without the lock */
	pg_atomic_uint64 invalid_tags;
	pg_atomic_uint64 dropped_tags;
	pg_atomic_uint64 regex_compile_failures;
	pg_atomic_uint64 heuristic_scans;
	pg_atomic_uint64 capped_tags;
	pg_atomic_uint64 cap_table_full;
	pg_atomic_uint64 utility_missing_queryid;
	pg_atomic_uint64 dropped_records;	/* new keys dropped: no room even
										 * after an eviction pass */
} PsscSharedState;

static pssc_shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Size requested by this postmaster (inherited by forked backends). */
static Size requested_shmem_bytes = 0;

/* Attached in shmem_startup_hook; NULL when not preloaded. */
static PsscSharedState *store_state = NULL;
static HTAB *store_htab = NULL;
static Size store_keysize = 0;

static PsscStoreRecordTestHook record_test_hook = NULL;
static void *record_test_hook_arg = NULL;
static PsscStoreRecordTestHook flush_test_hook = NULL;
static void *flush_test_hook_arg = NULL;
static PsscStoreRecordTestHook info_scan_test_hook = NULL;
static void *info_scan_test_hook_arg = NULL;

/* Testing aid: the next eviction pass in this backend gets no candidate buffer. */
static bool debug_fail_next_eviction_alloc = false;

#define ENTRY_HEADER_OFFSET(keysize) (keysize)
#define ENTRY_SLOTS_OFFSET(keysize) \
	((keysize) + MAXALIGN(sizeof(PsscEntryHeader)))

static inline PsscEntryHeader *
entry_header(void *entry)
{
	return (PsscEntryHeader *) ((char *) entry + ENTRY_HEADER_OFFSET(store_keysize));
}

static inline PsscSlot *
entry_slots(void *entry)
{
	return (PsscSlot *) ((char *) entry + ENTRY_SLOTS_OFFSET(store_keysize));
}

/* Largest debug clock offset either way: about 3000 years. */
#define PSSC_DEBUG_CLOCK_MAX_OFFSET INT64CONST(100000000000000000)

/* ---------------------------------------------------------------- sizing */

Size
pssc_store_keysize_for(int max_tagset_bytes)
{
	if (max_tagset_bytes < 0)
		elog(ERROR, "invalid max_tagset_bytes %d", max_tagset_bytes);
	return MAXALIGN(add_size(offsetof(PsscKey, tags), (Size) max_tagset_bytes));
}

Size
pssc_store_entrysize_for(Size keysize, int bucket_count)
{
	if (bucket_count < 1)
		elog(ERROR, "invalid bucket_count %d", bucket_count);
	return add_size(add_size(MAXALIGN(keysize), MAXALIGN(sizeof(PsscEntryHeader))),
					mul_size((Size) bucket_count, sizeof(PsscSlot)));
}

Size
pssc_store_shmem_size_for(int max_entries, int max_tagset_bytes, int bucket_count)
{
	Size		entrysize;

	if (max_entries < 1)
		elog(ERROR, "invalid max_entries %d", max_entries);
	entrysize = pssc_store_entrysize_for(pssc_store_keysize_for(max_tagset_bytes),
										 bucket_count);
	return add_size(MAXALIGN(sizeof(PsscSharedState)),
					hash_estimate_size(max_entries, entrysize));
}

Size
pssc_store_shmem_size(void)
{
	return pssc_store_shmem_size_for(pssc_max_entries, pssc_max_tagset_bytes,
									 pssc_bucket_count);
}

bool
pssc_store_available(void)
{
	return store_state != NULL && store_htab != NULL;
}

Size
pssc_store_keysize(void)
{
	return store_keysize;
}

/* ------------------------------------------------------ hash and compare */

/*
 * Normal hash of a key: the fixed fields combined with tags_hash. Safe to
 * compute without a lock (it does not depend on the collision mode).
 */
static uint32
key_hash_normal(const PsscKey *key)
{
	uint32		h;

	h = hash_bytes_uint32((uint32) key->dbid);
	h = hash_combine(h, hash_bytes_uint32((uint32) key->userid));
	h = hash_combine(h, hash_bytes_uint32((uint32) key->queryid));
	h = hash_combine(h, hash_bytes_uint32((uint32) ((uint64) key->queryid >> 32)));
	h = hash_combine(h, hash_bytes_uint32(((uint32) key->tags_len << 1) |
										  (key->toplevel ? 1 : 0)));
	return hash_combine(h, key->tags_hash);
}

/*
 * The hash the table actually uses. force_collisions changes only under the
 * exclusive lock (while the table is empty), so the caller must hold the
 * lock (any mode) for the result to match the entries' stored hashes.
 */
static inline uint32
key_hash_effective(uint32 normal)
{
	return store_state->force_collisions ? 0 : normal;
}

/* dynahash callback; dynahash only runs it under our lock. */
static uint32
key_hash(const void *k, Size keysize)
{
	return key_hash_effective(key_hash_normal((const PsscKey *) k));
}

/* 0 if equal: fixed fields and tags_len first, then only the used bytes. */
static int
key_compare(const void *a, const void *b, Size keysize)
{
	const PsscKey *k1 = (const PsscKey *) a;
	const PsscKey *k2 = (const PsscKey *) b;

	if (k1->dbid != k2->dbid || k1->userid != k2->userid ||
		k1->queryid != k2->queryid || k1->toplevel != k2->toplevel ||
		k1->tags_len != k2->tags_len || k1->tags_hash != k2->tags_hash)
		return 1;
	return memcmp(k1->tags, k2->tags, k1->tags_len) == 0 ? 0 : 1;
}

uint32
pssc_store_key_hash(const PsscKey *key)
{
	uint32		h;

	if (store_state == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");
	LWLockAcquire(store_state->lock, LW_SHARED);
	h = key_hash(key, store_keysize);
	LWLockRelease(store_state->lock);
	return h;
}

bool
pssc_store_build_key(PsscKey *key, Oid dbid, Oid userid, int64 queryid,
					 bool toplevel, const char *tags, size_t tags_len,
					 uint32 tags_hash)
{
	if (store_state == NULL ||
		tags_len > (size_t) store_state->max_tagset_bytes)
		return false;
	memset(key, 0, store_keysize);
	key->dbid = dbid;
	key->userid = userid;
	key->queryid = queryid;
	key->toplevel = toplevel;
	key->tags_len = (uint16) tags_len;
	key->tags_hash = tags_hash;
	if (tags_len > 0)
		memcpy(key->tags, tags, tags_len);
	return true;
}

/* ---------------------------------------------------------- shmem setup */

static void
store_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	requested_shmem_bytes = pssc_store_shmem_size();
	RequestAddinShmemSpace(requested_shmem_bytes);
	RequestNamedLWLockTranche(PSSC_LWLOCK_TRANCHE, 1);
}

static void
store_shmem_startup(void)
{
	PsscSharedState *state;
	HASHCTL		info;
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	store_state = NULL;
	store_htab = NULL;
	store_keysize = 0;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	state = ShmemInitStruct(PSSC_STORE_NAME, sizeof(PsscSharedState), &found);
	if (!found)
	{
		memset(state, 0, sizeof(PsscSharedState));
		state->lock = &(GetNamedLWLockTranche(PSSC_LWLOCK_TRANCHE))->lock;
		state->max_entries = pssc_max_entries;
		state->bucket_count = pssc_bucket_count;
		state->max_tagset_bytes = pssc_max_tagset_bytes;
		state->keysize = pssc_store_keysize_for(pssc_max_tagset_bytes);
		state->entrysize = pssc_store_entrysize_for(state->keysize,
													pssc_bucket_count);
		state->shmem_bytes = requested_shmem_bytes;
		state->force_collisions = false;
		state->entries = 0;
		state->dealloc = 0;
		state->reclaimed_entries = 0;
		state->evicted_entries = 0;
		state->stats_reset = GetCurrentTimestamp();

		/*
		 * The epoch is the start time rounded down to a multiple of the
		 * interval since the PostgreSQL epoch, so bucket boundaries fall on
		 * wall-clock multiples of the interval and current_bucket starts at 0.
		 */
		state->interval_us = (int64) pssc_bucket_interval * USECS_PER_SEC;
		state->epoch = pssc_bucket_floor_div(state->stats_reset, state->interval_us)
			* state->interval_us;
		pg_atomic_init_u64(&state->current_bucket,
						   (uint64) pssc_bucket_for_time(state->stats_reset, state->epoch,
														 state->interval_us));
		pg_atomic_init_u64(&state->bucket_advances, 0);
		pg_atomic_init_u32(&state->clock_debug, 0);
		SpinLockInit(&state->clock_mutex);
		state->clock_mode = PSSC_CLOCK_REAL;
		state->clock_value = 0;
		pg_atomic_init_u64(&state->invalid_tags, 0);
		pg_atomic_init_u64(&state->dropped_tags, 0);
		pg_atomic_init_u64(&state->regex_compile_failures, 0);
		pg_atomic_init_u64(&state->heuristic_scans, 0);
		pg_atomic_init_u64(&state->capped_tags, 0);
		pg_atomic_init_u64(&state->cap_table_full, 0);
		pg_atomic_init_u64(&state->utility_missing_queryid, 0);
		pg_atomic_init_u64(&state->dropped_records, 0);
	}

	/* key_hash() reads store_state, and keysize is fixed by the header */
	store_state = state;
	store_keysize = state->keysize;

	memset(&info, 0, sizeof(info));
	info.keysize = state->keysize;
	info.entrysize = state->entrysize;
	info.hash = key_hash;
	info.match = key_compare;
	store_htab = ShmemInitHash(PSSC_STORE_HASH_NAME,
							   state->max_entries, state->max_entries,
							   &info,
							   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);
}

void
pssc_store_init(void)
{
	PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev_shmem_request_hook, store_shmem_request);
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = store_shmem_startup;
}

/* ----------------------------------------------------------- the clock */

static TimestampTz
store_now(void)
{
	PsscDebugClockMode mode;
	int64		value;

	if (likely(pg_atomic_read_u32(&store_state->clock_debug) == 0))
		return GetCurrentTimestamp();

	SpinLockAcquire(&store_state->clock_mutex);
	mode = store_state->clock_mode;
	value = store_state->clock_value;
	SpinLockRelease(&store_state->clock_mutex);

	if (mode == PSSC_CLOCK_PINNED)
		return (TimestampTz) value;
	/* |value| <= PSSC_DEBUG_CLOCK_MAX_OFFSET, so this cannot overflow */
	return GetCurrentTimestamp() + value;
}

static inline int64
store_clock_bucket_at(TimestampTz now)
{
	return pssc_bucket_for_time(now, store_state->epoch, store_state->interval_us);
}

/*
 * current_bucket, the shared monotonic watermark (§5.2). It is advanced
 * lock-free by a CAS max loop, by writers and readers alike, and never
 * decreases: a clock step backwards cannot bring expired counts back, and a
 * stale id can never be written. Every id in any ring is <= it.
 */
static inline int64
watermark_read(void)
{
	return (int64) pg_atomic_read_u64(&store_state->current_bucket);
}

/* Raises current_bucket to at least target; returns the (new) value. */
static int64
watermark_advance(int64 target)
{
	uint64		old = pg_atomic_read_u64(&store_state->current_bucket);

	while ((int64) old < target)
	{
		/* on failure, old is reloaded with the value another backend set */
		if (pg_atomic_compare_exchange_u64(&store_state->current_bucket,
										   &old, (uint64) target))
		{
			pg_atomic_fetch_add_u64(&store_state->bucket_advances, 1);
			return target;
		}
	}
	return (int64) old;
}

/*
 * Reads the clock and raises current_bucket to max(it, the clock's bucket,
 * at_least). Readers call this so that liveness never depends on writers;
 * writers call it after taking the lock, so a stall (or a stale caller id)
 * cannot make them write an expired bucket. Must not be called with an
 * entry spinlock held (the debug clock takes its own spinlock).
 */
static inline int64
observe_current_bucket(int64 at_least)
{
	return watermark_advance(Max(at_least, store_clock_bucket_at(store_now())));
}

TimestampTz
pssc_store_now(void)
{
	if (store_state == NULL)
		return GetCurrentTimestamp();
	return store_now();
}

int64
pssc_store_clock_bucket(void)
{
	if (store_state == NULL)
		return 0;
	return store_clock_bucket_at(store_now());
}

TimestampTz
pssc_store_bucket_start(int64 bucket_id)
{
	if (store_state == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");
	return (TimestampTz) pssc_bucket_start(bucket_id, store_state->epoch,
										   store_state->interval_us);
}

/* ------------------------------------------------------------- recording */

static void
entry_init(void *entry)
{
	PsscEntryHeader *hdr = entry_header(entry);
	PsscSlot   *slots = entry_slots(entry);

	SpinLockInit(&hdr->mutex);
	hdr->encoding = GetDatabaseEncoding();
	hdr->last_bucket = PSSC_BUCKET_NONE;
	hdr->usage = pssc_usage_init();
	hdr->calls_total = 0;
	hdr->exec_time_total = 0.0;
	hdr->stats_since = GetCurrentTimestamp();
	for (int i = 0; i < store_state->bucket_count; i++)
		pssc_slot_init(&slots[i]);
}

/*
 * Adds one call to the entry's ring in bucket current_bucket, read under the
 * entry spinlock: that bucket is live at the moment of the write, and it is
 * >= the entry's last_bucket (which an earlier writer read the same way from
 * the monotonic watermark), so every id in the ring stays <= current_bucket.
 * The caller holds the lock (any mode) and has already advanced the
 * watermark (observe_current_bucket()). Returns the id written.
 */
static int64
entry_accum(void *entry, double elapsed_ms)
{
	PsscEntryHeader *hdr = entry_header(entry);
	PsscSlot   *slots = entry_slots(entry);
	int			count = store_state->bucket_count;
	int64		bucket_id;

	SpinLockAcquire(&hdr->mutex);
	bucket_id = watermark_read();
	Assert(hdr->last_bucket == PSSC_BUCKET_NONE || hdr->last_bucket <= bucket_id);

	/* lazy per-entry rollover: relabel the slot if it holds an older bucket */
	{
		PsscSlot   *slot = &slots[pssc_bucket_slot_index(bucket_id, count)];

		pssc_slot_roll(slot, bucket_id);
		pssc_slot_accum(slot, elapsed_ms);
	}
	hdr->last_bucket = bucket_id;
	hdr->calls_total++;
	hdr->exec_time_total += elapsed_ms;
	pssc_usage_exec(&hdr->usage);

#ifdef USE_ASSERT_CHECKING
	{
		int			bad;

		Assert(pssc_ring_check(slots, count, hdr->last_bucket,
							   bucket_id, &bad) == NULL);
	}
#endif

	SpinLockRelease(&hdr->mutex);
	return bucket_id;
}

/*
 * Candidate buffer of store_evict(), reused across passes. A backend keeps
 * one of at most PSSC_EVICT_BUF_KEEP bytes in TopMemoryContext (500
 * candidates at the default max_entries = 10000 take 16 kB); a larger one
 * (max_entries above ~40000) is allocated for the pass and freed after it.
 */
#define PSSC_EVICT_BUF_KEEP		((Size) 64 * 1024)

static PsscEvictCandidate *evict_buf = NULL;
static size_t evict_buf_cap = 0;

/*
 * A buffer of cap candidates, or NULL if it cannot be allocated (never an
 * ERROR). *transient tells the caller to pfree() it after the pass.
 *
 * The testing aid (pssc_store_debug_fail_next_eviction_alloc()) drops any
 * kept buffer and makes this pass's allocation return NULL, in either
 * branch, as MCXT_ALLOC_NO_OOM does when out of memory.
 */
static PsscEvictCandidate *
evict_buffer(size_t cap, bool *transient)
{
	Size		bytes = (Size) cap * sizeof(PsscEvictCandidate);
	PsscEvictCandidate *buf;

	bool		fail = debug_fail_next_eviction_alloc;

	debug_fail_next_eviction_alloc = false;
	*transient = false;
	if (fail && evict_buf != NULL)
	{
		pfree(evict_buf);
		evict_buf = NULL;
		evict_buf_cap = 0;
	}
	if (cap <= evict_buf_cap)
		return evict_buf;
	if (bytes > PSSC_EVICT_BUF_KEEP)
	{
		buf = fail ? NULL :
			MemoryContextAllocExtended(CurrentMemoryContext, bytes,
									   MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
		*transient = (buf != NULL);
		return buf;
	}
	buf = fail ? NULL :
		MemoryContextAllocExtended(TopMemoryContext, bytes, MCXT_ALLOC_NO_OOM);
	if (buf == NULL)
		return NULL;
	if (evict_buf != NULL)
		pfree(evict_buf);
	evict_buf = buf;
	evict_buf_cap = cap;
	return buf;
}

/*
 * One eviction pass (§5.3); the caller holds the exclusive lock and found
 * the table at max_entries. Returns the number of entries removed.
 *
 *	1. Raise current_bucket to the clock, as readers do, and scan the table
 *	   once: reclaim every dead entry (no slot in the live window); every
 *	   surviving entry's usage decays by PSSC_USAGE_DECREASE_FACTOR, as in
 *	   pgss's entry_dealloc(), and is offered to a selector
 *	   (pssc_evict_select_*()) that keeps the pssc_evict_target() first
 *	   live entries in eviction order: last_bucket, then usage, then scan
 *	   order. A pass never evicts more live entries than the target, so
 *	   the victims are among them.
 *	2. If the dead entries were fewer than the target, evict the first
 *	   (target - dead) live entries in that order: exactly the front of a
 *	   full sort of every live entry (pssc_evict_sort()), at O(n) cost
 *	   instead of O(n log n), in one scan, with a buffer of target entries
 *	   (not nlive) that is reused across passes.
 *	3. dealloc += 1; reclaimed_entries += the dead entries removed,
 *	   evicted_entries += the live ones (a sign that max_entries is too
 *	   small, where reclaiming dead entries is normal housekeeping).
 *
 * The entry fields are read and written without the entry spinlocks: those
 * are only ever taken by a backend holding the table lock (writers and
 * readers alike take it in shared mode), so under the exclusive lock nobody
 * else can be touching any entry.
 *
 * The selector's buffer (evict_buffer()) is allocated with
 * MCXT_ALLOC_NO_OOM: recording runs inside user statements, so an
 * out-of-memory condition must not fail the statement. If it cannot be
 * allocated, only the dead entries are freed; the caller then drops its
 * record (dropped_records) if that left no room. Nothing between the
 * allocation and the pfree can raise an ERROR, and the table is consistent
 * after every single removal.
 */
static int64
store_evict(void)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;
	int			count = store_state->bucket_count;
	int64		current;
	int64		target;
	int64		dead = 0;
	int64		nlive = 0;
	int64		nvictims;
	int64		evicted = 0;	/* live entries */
	PsscEvictCandidate *cands = NULL;
	PsscEvictSelect sel;
	bool		transient = false;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));

	/*
	 * The same watermark readers use (§5.2): an entry dead here is hidden
	 * from every reader already, and stays dead because it never decreases.
	 * Only lock holders advance it, so it is stable while we hold the lock.
	 */
	current = observe_current_bucket(PSSC_BUCKET_NONE);
	target = pssc_evict_target(store_state->max_entries);

	cands = evict_buffer((size_t) target, &transient);
	if (cands != NULL)
		pssc_evict_select_init(&sel, cands, (size_t) target);

	/* deleting the entry just returned by hash_seq_search() is allowed */
	hash_seq_init(&seq, store_htab);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		PsscEntryHeader *hdr = entry_header(entry);

		if (pssc_bucket_entry_is_dead(hdr->last_bucket, current, count))
		{
			hash_search(store_htab, entry, HASH_REMOVE, NULL);
			dead++;
		}
		else
		{
			pssc_usage_decay(&hdr->usage);
			nlive++;
			if (cands != NULL)
				pssc_evict_select_offer(&sel, hdr->last_bucket, hdr->usage, entry);
		}
	}
	nvictims = pssc_evict_live_count(target, dead, nlive);
	if (nvictims > 0 && cands != NULL)
	{
		size_t		n = pssc_evict_select_finish(&sel, (size_t) nvictims);

		Assert(n == (size_t) nvictims);
		for (size_t i = 0; i < n; i++)
			hash_search(store_htab, cands[i].entry, HASH_REMOVE, NULL);
		evicted = (int64) n;
	}
	if (transient && cands != NULL)
		pfree(cands);

	store_state->entries -= dead + evicted;
	store_state->dealloc++;
	store_state->reclaimed_entries += dead;
	store_state->evicted_entries += evicted;
	Assert(store_state->entries == hash_get_num_entries(store_htab));
	return dead + evicted;
}

static inline bool
stats_nonzero(const PsscTagsetStats *s)
{
	return s->invalid_tags != 0 || s->dropped_tags != 0 ||
		s->heuristic_scans != 0 || s->regex_compile_failures != 0 ||
		s->capped_tags != 0 || s->cap_table_full != 0;
}

/*
 * Adds one statement's diagnostic counters (and its utility_missing_queryid,
 * 0 or 1) to the header, then zeroes *stats. The caller holds the lock in
 * either mode: pssc_store_reset() zeroes these atomics under the exclusive
 * lock, so a flush is wholly before or wholly after any reset (never split).
 */
static void
add_diagnostics_locked(PsscTagsetStats *stats, uint64 utility_missing_queryid)
{
	Assert(LWLockHeldByMe(store_state->lock));
	if (stats->invalid_tags)
		pg_atomic_fetch_add_u64(&store_state->invalid_tags, stats->invalid_tags);
	/* testing aid: a stall between two adds, which a reset must not split */
	if (unlikely(flush_test_hook != NULL))
		flush_test_hook(flush_test_hook_arg);
	if (stats->dropped_tags)
		pg_atomic_fetch_add_u64(&store_state->dropped_tags, stats->dropped_tags);
	if (stats->heuristic_scans)
		pg_atomic_fetch_add_u64(&store_state->heuristic_scans, stats->heuristic_scans);
	if (stats->regex_compile_failures)
		pg_atomic_fetch_add_u64(&store_state->regex_compile_failures,
								stats->regex_compile_failures);
	if (stats->capped_tags)
		pg_atomic_fetch_add_u64(&store_state->capped_tags, stats->capped_tags);
	if (stats->cap_table_full)
		pg_atomic_fetch_add_u64(&store_state->cap_table_full, stats->cap_table_full);
	if (utility_missing_queryid)
		pg_atomic_fetch_add_u64(&store_state->utility_missing_queryid,
								utility_missing_queryid);
	memset(stats, 0, sizeof(*stats));
}

static PsscStoreResult record_impl(const PsscKey *key, int64 bucket_id,
								   double elapsed_ms, PsscTagsetStats *pending);

PsscStoreResult
pssc_store_record(const PsscKey *key, double elapsed_ms)
{
	return pssc_store_record_with_stats(key, elapsed_ms, NULL);
}

PsscStoreResult
pssc_store_record_with_stats(const PsscKey *key, double elapsed_ms,
							 PsscTagsetStats *pending)
{
	if (store_state == NULL || store_htab == NULL)
		return PSSC_STORE_UNAVAILABLE;

	/*
	 * The bucket in which the execution completes. record_impl() re-reads
	 * the clock once it holds the lock, so a stall moves the call forward.
	 */
	return record_impl(key, pssc_store_clock_bucket(), elapsed_ms, pending);
}

PsscStoreResult
pssc_store_record_at(const PsscKey *key, int64 bucket_id, double elapsed_ms)
{
	return record_impl(key, bucket_id, elapsed_ms, NULL);
}

static PsscStoreResult
record_impl(const PsscKey *key, int64 bucket_id, double elapsed_ms,
			PsscTagsetStats *pending)
{
	PsscStoreResult result;
	uint32		normal;
	uint32		hash;
	void	   *entry;
	bool		found = false;

	if (store_state == NULL || store_htab == NULL)
		return PSSC_STORE_UNAVAILABLE;
	Assert(bucket_id != PSSC_BUCKET_NONE);

	/* hashing happens before any lock; the collision mode is applied under it */
	normal = key_hash_normal(key);

	if (unlikely(record_test_hook != NULL))
		record_test_hook(record_test_hook_arg);

	/*
	 * §5.2: once the lock is held (a wait for it is a stall too), re-read
	 * the clock and raise current_bucket to max(it, clock bucket, the
	 * computed id). entry_accum() writes into current_bucket as read under
	 * the entry spinlock, so an older computed id (stalled writer, clock
	 * stepped back) is clamped up and the write always lands in a live slot.
	 */
	LWLockAcquire(store_state->lock, LW_SHARED);
	(void) observe_current_bucket(bucket_id);

	/* Fast path: an existing entry, under the shared lock. */
	hash = key_hash_effective(normal);
	entry = hash_search_with_hash_value(store_htab, key, hash, HASH_FIND, NULL);
	if (entry != NULL)
	{
		(void) entry_accum(entry, elapsed_ms);
		if (pending != NULL && stats_nonzero(pending))
			add_diagnostics_locked(pending, 0);
		LWLockRelease(store_state->lock);
		return PSSC_STORE_UPDATED;
	}
	LWLockRelease(store_state->lock);

	/*
	 * Slow path: LWLocks cannot be upgraded, so take the exclusive lock and
	 * look again (another backend may have inserted the key meanwhile).
	 */
	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	(void) observe_current_bucket(bucket_id);	/* the clock moved while waiting */
	hash = key_hash_effective(normal);	/* the mode may have changed */
	entry = hash_search_with_hash_value(store_htab, key, hash, HASH_FIND, NULL);
	if (entry != NULL)
		result = PSSC_STORE_FOUND_LATE;
	else
	{
		/* §5.3: make room first (the new key cannot be among the victims) */
		if (store_state->entries >= store_state->max_entries)
			(void) store_evict();
		if (store_state->entries < store_state->max_entries)
		{
			entry = hash_search_with_hash_value(store_htab, key, hash,
												HASH_ENTER_NULL, &found);
			if (entry != NULL)
			{
				Assert(!found);
				entry_init(entry);
				store_state->entries++;
			}
		}
		result = PSSC_STORE_INSERTED;
	}

	if (entry == NULL)
	{
		pg_atomic_fetch_add_u64(&store_state->dropped_records, 1);
		result = PSSC_STORE_FULL;
	}
	else
		(void) entry_accum(entry, elapsed_ms);
	if (pending != NULL && stats_nonzero(pending))
		add_diagnostics_locked(pending, 0);

	Assert(store_state->entries == hash_get_num_entries(store_htab));
	Assert(store_state->entries <= store_state->max_entries);
	LWLockRelease(store_state->lock);
	return result;
}

/* ------------------------------------------------------- reading, reset */

void
pssc_store_foreach(PsscStoreVisitor fn, void *arg)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;
	char	   *copy;
	Size		keysize = store_keysize;
	Size		entrysize;
	PsscStoreEntryView view;

	if (store_state == NULL || store_htab == NULL)
		return;
	entrysize = store_state->entrysize;
	copy = palloc(entrysize);

	LWLockAcquire(store_state->lock, LW_SHARED);

	/* readers advance the watermark to the clock: no dependence on writers */
	view.scan_bucket = observe_current_bucket(PSSC_BUCKET_NONE);
	hash_seq_init(&seq, store_htab);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		PsscEntryHeader *hdr = entry_header(entry);
		PsscEntryHeader *chdr;

		/* the key is immutable once inserted; the rest needs the spinlock */
		memcpy(copy, entry, keysize);
		SpinLockAcquire(&hdr->mutex);
		memcpy(copy + keysize, (char *) entry + keysize, entrysize - keysize);
		SpinLockRelease(&hdr->mutex);

		/*
		 * Read after the copy: the copied ids were written from the
		 * watermark under the spinlock, so they are all <= this value.
		 */
		view.current_bucket = watermark_read();

		chdr = entry_header(copy);
		view.key = (const PsscKey *) copy;
		view.encoding = chdr->encoding;
		view.last_bucket = chdr->last_bucket;
		view.usage = chdr->usage;
		view.calls_total = chdr->calls_total;
		view.exec_time_total = chdr->exec_time_total;
		view.stats_since = chdr->stats_since;
		view.bucket_count = store_state->bucket_count;
		view.slots = entry_slots(copy);
		fn(&view, arg);
	}
	LWLockRelease(store_state->lock);
	pfree(copy);
}

void
pssc_store_reset(void)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;

	if (store_state == NULL || store_htab == NULL)
		return;

	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	hash_seq_init(&seq, store_htab);
	/* key_hash() runs under the exclusive lock, so the mode is stable */
	while ((entry = hash_seq_search(&seq)) != NULL)
		hash_search(store_htab, entry, HASH_REMOVE, NULL);
	store_state->entries = 0;
	store_state->dealloc = 0;
	store_state->reclaimed_entries = 0;
	store_state->evicted_entries = 0;
	pg_atomic_write_u64(&store_state->invalid_tags, 0);
	pg_atomic_write_u64(&store_state->dropped_tags, 0);
	pg_atomic_write_u64(&store_state->regex_compile_failures, 0);
	pg_atomic_write_u64(&store_state->heuristic_scans, 0);
	pg_atomic_write_u64(&store_state->capped_tags, 0);
	pg_atomic_write_u64(&store_state->cap_table_full, 0);
	pg_atomic_write_u64(&store_state->utility_missing_queryid, 0);
	pg_atomic_write_u64(&store_state->dropped_records, 0);
	/* current_bucket never decreases, and the epoch is fixed: both kept */
	pg_atomic_write_u64(&store_state->bucket_advances, 0);
	store_state->stats_reset = GetCurrentTimestamp();
	Assert(hash_get_num_entries(store_htab) == 0);
	LWLockRelease(store_state->lock);
}

/*
 * Fills *c; the caller holds the lock (either mode). The lock-free counters
 * are read under it too: pssc_store_reset() zeroes them under the exclusive
 * lock, so the copy is wholly from before or wholly from after any reset.
 */
static void
read_counters_locked(PsscStoreCounters *c)
{
	Assert(LWLockHeldByMe(store_state->lock));
	c->entries = store_state->entries;
	c->hash_entries = (int64) hash_get_num_entries(store_htab);
	c->dealloc = store_state->dealloc;
	c->reclaimed_entries = store_state->reclaimed_entries;
	c->evicted_entries = store_state->evicted_entries;
	c->stats_reset = store_state->stats_reset;
	c->force_collisions = store_state->force_collisions;
	c->max_entries = store_state->max_entries;
	c->invalid_tags = (int64) pg_atomic_read_u64(&store_state->invalid_tags);
	c->dropped_tags = (int64) pg_atomic_read_u64(&store_state->dropped_tags);
	c->regex_compile_failures =
		(int64) pg_atomic_read_u64(&store_state->regex_compile_failures);
	c->heuristic_scans = (int64) pg_atomic_read_u64(&store_state->heuristic_scans);
	c->capped_tags = (int64) pg_atomic_read_u64(&store_state->capped_tags);
	c->cap_table_full = (int64) pg_atomic_read_u64(&store_state->cap_table_full);
	c->utility_missing_queryid =
		(int64) pg_atomic_read_u64(&store_state->utility_missing_queryid);
	c->dropped_records = (int64) pg_atomic_read_u64(&store_state->dropped_records);
	c->shmem_bytes = store_state->shmem_bytes;
	c->keysize = store_state->keysize;
	c->entrysize = store_state->entrysize;
	c->bucket_count = store_state->bucket_count;
	c->interval_us = store_state->interval_us;
	c->max_tagset_bytes = store_state->max_tagset_bytes;
}

bool
pssc_store_get_counters(PsscStoreCounters *c)
{
	memset(c, 0, sizeof(*c));
	if (store_state == NULL || store_htab == NULL)
		return false;

	LWLockAcquire(store_state->lock, LW_SHARED);
	read_counters_locked(c);
	LWLockRelease(store_state->lock);
	return true;
}

bool
pssc_store_get_info(PsscStoreCounters *c, int64 *oldest_bucket)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;
	int			count;
	int64	   *ids;
	int64		oldest = PSSC_BUCKET_NONE;
	int64		current;

	memset(c, 0, sizeof(*c));
	*oldest_bucket = PSSC_BUCKET_NONE;
	if (store_state == NULL || store_htab == NULL)
		return false;
	count = store_state->bucket_count;
	ids = palloc(count * sizeof(int64));

	LWLockAcquire(store_state->lock, LW_SHARED);
	read_counters_locked(c);

	/* as every reader (§5.2): advance the watermark to the clock first */
	current = observe_current_bucket(PSSC_BUCKET_NONE);

	/*
	 * Judge every slot against one watermark, the one returned as
	 * current_bucket, so the row is self-consistent; if a writer or reader
	 * moved it during the scan, scan again (it moves at most once per
	 * bucket_interval, so this is rare and settles quickly).
	 */
	for (;;)
	{
		int64		after;

		oldest = PSSC_BUCKET_NONE;
		hash_seq_init(&seq, store_htab);
		while ((entry = hash_seq_search(&seq)) != NULL)
		{
			PsscEntryHeader *hdr = entry_header(entry);
			PsscSlot   *slots = entry_slots(entry);

			SpinLockAcquire(&hdr->mutex);
			for (int i = 0; i < count; i++)
				ids[i] = slots[i].bucket_id;
			SpinLockRelease(&hdr->mutex);

			for (int i = 0; i < count; i++)
				if (pssc_bucket_is_live(ids[i], current, count) &&
					(oldest == PSSC_BUCKET_NONE || ids[i] < oldest))
					oldest = ids[i];

			if (unlikely(info_scan_test_hook != NULL))
				info_scan_test_hook(info_scan_test_hook_arg);
		}
		after = watermark_read();
		if (after == current)
			break;
		current = after;
	}
	c->current_bucket = current;
	LWLockRelease(store_state->lock);

	pfree(ids);
	*oldest_bucket = oldest;
	return true;
}

void
pssc_store_add_tagset_stats(const PsscTagsetStats *stats)
{
	PsscTagsetStats copy;

	/* the common case (nothing pending) takes no lock */
	if (store_state == NULL || !stats_nonzero(stats))
		return;
	/* LWLocks are not reentrant; no caller flushes while holding the lock */
	Assert(!LWLockHeldByMe(store_state->lock));
	copy = *stats;
	LWLockAcquire(store_state->lock, LW_SHARED);
	add_diagnostics_locked(&copy, 0);
	LWLockRelease(store_state->lock);
}

void
pssc_store_count_utility_missing_queryid(PsscTagsetStats *pending)
{
	PsscTagsetStats none;

	if (store_state == NULL)
		return;
	if (pending == NULL)
	{
		memset(&none, 0, sizeof(none));
		pending = &none;
	}
	Assert(!LWLockHeldByMe(store_state->lock));
	LWLockAcquire(store_state->lock, LW_SHARED);
	add_diagnostics_locked(pending, 1);
	LWLockRelease(store_state->lock);
}

void
pssc_store_debug_force_collisions(bool on)
{
	if (store_state == NULL || store_htab == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");

	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	if (hash_get_num_entries(store_htab) != 0)
	{
		LWLockRelease(store_state->lock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statement_context hash table is not empty"),
				 errhint("Reset the store before changing forced collisions.")));
	}
	store_state->force_collisions = on;
	LWLockRelease(store_state->lock);
}

void
pssc_store_debug_fail_next_eviction_alloc(void)
{
	debug_fail_next_eviction_alloc = true;
}

void
pssc_store_set_record_test_hook(PsscStoreRecordTestHook hook, void *arg)
{
	record_test_hook = hook;
	record_test_hook_arg = arg;
}

void
pssc_store_set_flush_test_hook(PsscStoreRecordTestHook hook, void *arg)
{
	flush_test_hook = hook;
	flush_test_hook_arg = arg;
}

void
pssc_store_set_info_scan_test_hook(PsscStoreRecordTestHook hook, void *arg)
{
	info_scan_test_hook = hook;
	info_scan_test_hook_arg = arg;
}


/* ------------------------------------------------------- bucket state */

bool
pssc_store_get_buckets(PsscStoreBuckets *b)
{
	memset(b, 0, sizeof(*b));
	if (store_state == NULL || store_htab == NULL)
		return false;

	b->epoch = store_state->epoch;
	b->interval_us = store_state->interval_us;
	b->bucket_count = store_state->bucket_count;

	SpinLockAcquire(&store_state->clock_mutex);
	b->clock_mode = store_state->clock_mode;
	b->clock_value = store_state->clock_value;
	SpinLockRelease(&store_state->clock_mutex);

	/* a diagnostic snapshot: unlike readers, it does not advance anything */
	b->now = store_now();
	b->clock_bucket = store_clock_bucket_at(b->now);
	b->current_bucket = watermark_read();
	b->advances = (int64) pg_atomic_read_u64(&store_state->bucket_advances);
	b->reader_bucket = Max(b->clock_bucket, b->current_bucket);
	return true;
}

int64
pssc_store_check_invariants(void)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;
	char	   *copy;
	Size		keysize = store_keysize;
	Size		entrysize;
	int64		current = 0;
	int64		n = 0;
	int64		entries;
	int64		hash_entries;
	const char *problem = NULL;
	int			bad_slot = -1;
	PsscKey    *bad_key = NULL;
	int64		bad_last = 0;
	PsscSlot	bad_contents = {0};

	if (store_state == NULL || store_htab == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");
	entrysize = store_state->entrysize;
	copy = palloc(entrysize);

	LWLockAcquire(store_state->lock, LW_SHARED);
	(void) observe_current_bucket(PSSC_BUCKET_NONE);
	entries = store_state->entries;
	hash_entries = (int64) hash_get_num_entries(store_htab);
	hash_seq_init(&seq, store_htab);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		PsscEntryHeader *hdr = entry_header(entry);

		memcpy(copy, entry, keysize);
		SpinLockAcquire(&hdr->mutex);
		memcpy(copy + keysize, (char *) entry + keysize, entrysize - keysize);
		SpinLockRelease(&hdr->mutex);
		current = watermark_read();	/* after the copy, as readers do */

		n++;
		problem = pssc_ring_check(entry_slots(copy), store_state->bucket_count,
								  entry_header(copy)->last_bucket, current,
								  &bad_slot);
		if (problem != NULL)
		{
			bad_key = (PsscKey *) copy;
			bad_last = entry_header(copy)->last_bucket;
			if (bad_slot >= 0)
				bad_contents = entry_slots(copy)[bad_slot];
			hash_seq_term(&seq);
			break;
		}
	}
	LWLockRelease(store_state->lock);

	if (problem != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_stat_statement_context ring invariant violated: %s", problem),
				 errdetail("queryid " INT64_FORMAT ", last_bucket " INT64_FORMAT
						   ", current_bucket " INT64_FORMAT ", slot %d (bucket "
						   INT64_FORMAT ", calls " INT64_FORMAT ").",
						   bad_key->queryid, bad_last, current, bad_slot,
						   bad_contents.bucket_id, bad_contents.calls)));
	if (entries != hash_entries || n != entries)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_stat_statement_context entry count mismatch"),
				 errdetail("header " INT64_FORMAT ", hash table " INT64_FORMAT
						   ", scanned " INT64_FORMAT ".", entries, hash_entries, n)));
	pfree(copy);
	return n;
}

/* ------------------------------------------------------- debug clock */

static void
check_clock_value(PsscDebugClockMode mode, int64 value)
{
	if (mode == PSSC_CLOCK_PINNED ? !IS_VALID_TIMESTAMP(value)
		: (value > PSSC_DEBUG_CLOCK_MAX_OFFSET || value < -PSSC_DEBUG_CLOCK_MAX_OFFSET))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("pg_stat_statement_context debug clock value out of range")));
}

static inline void
clock_set_locked(PsscDebugClockMode mode, int64 value)
{
	store_state->clock_mode = mode;
	store_state->clock_value = value;
	pg_atomic_write_u32(&store_state->clock_debug,
						(mode == PSSC_CLOCK_REAL && value == 0) ? 0 : 1);
}

void
pssc_store_debug_set_clock(PsscDebugClockMode mode, int64 value)
{
	if (store_state == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");
	if (mode != PSSC_CLOCK_REAL && mode != PSSC_CLOCK_PINNED)
		elog(ERROR, "invalid debug clock mode %d", (int) mode);
	check_clock_value(mode, value);

	SpinLockAcquire(&store_state->clock_mutex);
	clock_set_locked(mode, value);
	SpinLockRelease(&store_state->clock_mutex);
}

void
pssc_store_debug_advance_clock(int64 usec)
{
	PsscDebugClockMode mode;
	int64		value;
	bool		overflow;

	if (store_state == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");

	SpinLockAcquire(&store_state->clock_mutex);
	mode = store_state->clock_mode;
	overflow = pg_add_s64_overflow(store_state->clock_value, usec, &value);
	if (!overflow &&
		(mode == PSSC_CLOCK_PINNED ? IS_VALID_TIMESTAMP(value)
		 : (value <= PSSC_DEBUG_CLOCK_MAX_OFFSET && value >= -PSSC_DEBUG_CLOCK_MAX_OFFSET)))
	{
		clock_set_locked(mode, value);
		SpinLockRelease(&store_state->clock_mutex);
		return;
	}
	SpinLockRelease(&store_state->clock_mutex);
	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("pg_stat_statement_context debug clock value out of range")));
}
