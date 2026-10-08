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
 *
 * The compact eviction array (§5.3): the shared header ends with
 * evict_slots[max_entries], one PsscEvictSlot per entry in the table, in
 * slots [0, entries) (dense: removing an entry moves the last slot into its
 * hole). A slot holds the entry's last_bucket (a copy of the header's), its
 * usage (kept only here) and a pointer to the entry, which holds the slot's
 * index in evict_index. An eviction pass scans these 24-byte slots instead
 * of the whole entries (about 870 bytes each at the defaults) and touches
 * only the entries it removes. Locking follows the entries: the slot of an
 * entry is written under that entry's spinlock by a holder of the table
 * lock (shared), as the rest of the entry; slots are added, moved or
 * removed, and evict_index changes, only under the exclusive lock. So a
 * holder of the shared lock may read evict_index without the spinlock, and
 * the exclusive lock holder may read and write every slot without any.
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "catalog/pg_control.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "extract.h"
#include "guc.h"
#include "store.h"
#include "tagset.h"

#define PSSC_STORE_NAME			"pg_stat_statement_context"
#define PSSC_STORE_HASH_NAME	"pg_stat_statement_context hash"
#define PSSC_LWLOCK_TRANCHE		"pg_stat_statement_context"

/* Per-entry state after the key (ctxEntry of DESIGN.md §5.1). */
typedef struct PsscEntryHeader
{
	slock_t		mutex;			/* protects everything below and the slots,
								 * and this entry's evict_slots[] slot */
	int			encoding;		/* of tags[] (that of key.dbid) */
	int			evict_index;	/* its slot in evict_slots[] (changed only
								 * under the exclusive lock) */
	int64		last_bucket;	/* newest bucket_id written (also in the slot) */
	/* monotonic since stats_since, whatever bucket each call landed in */
	int64		calls_total;
	double		exec_time_total;	/* ms */
	TimestampTz stats_since;	/* when the entry was created */
} PsscEntryHeader;

/*
 * One slot of the compact eviction array (top of this file): what an
 * eviction pass needs to know about an entry without reading it.
 */
typedef struct PsscEvictSlot
{
	int64		last_bucket;	/* = the entry header's last_bucket */
	double		usage;			/* pgss-style, for eviction (§5.3) */
	void	   *entry;			/* the hash entry */
} PsscEvictSlot;

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
	int			exemplar_nkeys; /* exemplar slots per entry (§6.13) */
	int			exemplar_value_len; /* bytes per exemplar value */
	Size		exemplar_block; /* per-entry exemplar bytes (in entrysize) */

#ifdef PSSC_TESTING
	/* changed only under the exclusive lock while the table is empty */
	bool		force_collisions;
#endif

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

#ifdef PSSC_TESTING

	/*
	 * Debug clock (testing build only, store.h). clock_debug is read without
	 * a lock on every clock read; only when it is set are clock_mode and
	 * clock_value read, together, under clock_mutex.
	 */
	pg_atomic_uint32 clock_debug;
	slock_t		clock_mutex;
	PsscDebugClockMode clock_mode;
	int64		clock_value;
#endif

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
	pg_atomic_uint64 exemplar_values_dropped;

	/*
	 * The compact eviction array: [0, entries) in use (top of this file).
	 * Its length is entries, so it changes with entries, under the
	 * exclusive lock.
	 */
	PsscEvictSlot evict_slots[FLEXIBLE_ARRAY_MEMBER];	/* [max_entries] */
} PsscSharedState;

static pssc_shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Size requested by this postmaster (inherited by forked backends). */
static Size requested_shmem_bytes = 0;

/* Attached in shmem_startup_hook; NULL when not preloaded. */
static PsscSharedState *store_state = NULL;
static HTAB *store_htab = NULL;
static Size store_keysize = 0;

#ifdef PSSC_TESTING
static PsscStoreRecordTestHook record_test_hook = NULL;
static void *record_test_hook_arg = NULL;
static PsscStoreRecordTestHook flush_test_hook = NULL;
static void *flush_test_hook_arg = NULL;
static PsscStoreRecordTestHook info_scan_test_hook = NULL;
static void *info_scan_test_hook_arg = NULL;

/* Testing aid: the next eviction pass in this backend gets no candidate buffer. */
static bool debug_fail_next_eviction_alloc = false;
#endif

static void store_shmem_shutdown(int code, Datum arg);
static void store_load(void);

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

/*
 * The exemplar block (§6.13) after the ring: exemplar_nkeys slots of
 * EXEMPLAR_SLOT_SIZE bytes, each a uint16 length (EXEMPLAR_EMPTY: no value)
 * and value_len value bytes. The stride is not aligned, so the length is
 * read and written with memcpy.
 */
#define EXEMPLAR_EMPTY			((uint16) 0xFFFF)
#define EXEMPLAR_SLOT_SIZE(value_len)	(sizeof(uint16) + (Size) (value_len))

static inline char *
entry_exemplars(void *entry)
{
	return (char *) entry + ENTRY_SLOTS_OFFSET(store_keysize)
		+ (Size) store_state->bucket_count * sizeof(PsscSlot);
}

/* Marks every exemplar slot of a block empty. */
static void
exemplars_clear(char *block, int nkeys, int value_len)
{
	uint16		empty = EXEMPLAR_EMPTY;

	for (int i = 0; i < nkeys; i++)
		memcpy(block + (Size) i * EXEMPLAR_SLOT_SIZE(value_len), &empty,
			   sizeof(uint16));
}

static inline PsscEvictSlot *
entry_slot(void *entry)
{
	return &store_state->evict_slots[entry_header(entry)->evict_index];
}

#ifdef PSSC_TESTING
/* Largest debug clock offset either way: about 3000 years. */
#define PSSC_DEBUG_CLOCK_MAX_OFFSET INT64CONST(100000000000000000)
#endif

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

void
pssc_store_exemplar_layout_for(int max_entries, int memory_kb, int nkeys,
							   int *value_len, Size *block)
{
	Size		per_entry;
	Size		per_key;
	Size		len;

	*value_len = 0;
	*block = 0;
	if (nkeys <= 0 || max_entries < 1 || memory_kb <= 0)
		return;
	/* MAXALIGN rounds up, so take whole alignment units from the share */
	per_entry = mul_size((Size) memory_kb, 1024) / (Size) max_entries;
	per_entry -= per_entry % MAXIMUM_ALIGNOF;
	per_key = per_entry / (Size) nkeys;
	if (per_key <= sizeof(uint16))
		return;
	len = Min(per_key - sizeof(uint16), (Size) PSSC_EXEMPLAR_VALUE_MAX);
	*value_len = (int) len;
	*block = MAXALIGN((Size) nkeys * EXEMPLAR_SLOT_SIZE(len));
	Assert(*block <= per_entry);
}

/* Exemplar layout for the current settings. */
static void
exemplar_layout(int *nkeys, int *value_len, Size *block)
{
	*nkeys = pssc_tag_list_count(pssc_guc_exemplar_keys());
	pssc_store_exemplar_layout_for(pssc_max_entries, pssc_exemplar_memory,
								   *nkeys, value_len, block);
	/*
	 * No room for even an empty value: the entries have no exemplar slots at
	 * all, and every value captured is dropped (value_len 0) and counted.
	 */
	if (*block == 0)
	{
		*nkeys = 0;
		*value_len = 0;
	}
}

int
pssc_store_exemplar_value_len(void)
{
	if (store_state == NULL || store_htab == NULL)
		return 0;
	return store_state->exemplar_value_len;
}

/* The shared header with its eviction array of max_entries slots. */
static Size
header_size_for(int max_entries)
{
	return add_size(offsetof(PsscSharedState, evict_slots),
					mul_size((Size) max_entries, sizeof(PsscEvictSlot)));
}

Size
pssc_store_shmem_size_for(int max_entries, int max_tagset_bytes, int bucket_count)
{
	Size		entrysize;
	int			nkeys;
	int			value_len;
	Size		block;

	if (max_entries < 1)
		elog(ERROR, "invalid max_entries %d", max_entries);
	entrysize = pssc_store_entrysize_for(pssc_store_keysize_for(max_tagset_bytes),
										 bucket_count);
	/* the exemplar block, from the current exemplar settings (§5.1) */
	nkeys = pssc_tag_list_count(pssc_guc_exemplar_keys());
	pssc_store_exemplar_layout_for(max_entries, pssc_exemplar_memory, nkeys,
								   &value_len, &block);
	entrysize = add_size(entrysize, block);
	return add_size(MAXALIGN(header_size_for(max_entries)),
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
#ifdef PSSC_TESTING
	return store_state->force_collisions ? 0 : normal;
#else
	return normal;
#endif
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

	state = ShmemInitStruct(PSSC_STORE_NAME, header_size_for(pssc_max_entries),
							&found);
	if (!found)
	{
		memset(state, 0, header_size_for(pssc_max_entries));
		state->lock = &(GetNamedLWLockTranche(PSSC_LWLOCK_TRANCHE))->lock;
		state->max_entries = pssc_max_entries;
		state->bucket_count = pssc_bucket_count;
		state->max_tagset_bytes = pssc_max_tagset_bytes;
		state->keysize = pssc_store_keysize_for(pssc_max_tagset_bytes);
		exemplar_layout(&state->exemplar_nkeys, &state->exemplar_value_len,
						&state->exemplar_block);
		state->entrysize = add_size(pssc_store_entrysize_for(state->keysize,
															 pssc_bucket_count),
									state->exemplar_block);
		state->shmem_bytes = requested_shmem_bytes;
#ifdef PSSC_TESTING
		state->force_collisions = false;
#endif
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
#ifdef PSSC_TESTING
		pg_atomic_init_u32(&state->clock_debug, 0);
		SpinLockInit(&state->clock_mutex);
		state->clock_mode = PSSC_CLOCK_REAL;
		state->clock_value = 0;
#endif
		pg_atomic_init_u64(&state->invalid_tags, 0);
		pg_atomic_init_u64(&state->dropped_tags, 0);
		pg_atomic_init_u64(&state->regex_compile_failures, 0);
		pg_atomic_init_u64(&state->heuristic_scans, 0);
		pg_atomic_init_u64(&state->capped_tags, 0);
		pg_atomic_init_u64(&state->cap_table_full, 0);
		pg_atomic_init_u64(&state->utility_missing_queryid, 0);
		pg_atomic_init_u64(&state->dropped_records, 0);
		pg_atomic_init_u64(&state->exemplar_values_dropped, 0);
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

	/*
	 * Persistence (§5.5), as pg_stat_statements: only the postmaster (or a
	 * standalone backend) saves the store when it exits, and loads it when
	 * it creates shared memory. No other process exists yet at the load.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(store_shmem_shutdown, (Datum) 0);
	if (!found)
		store_load();
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
#ifdef PSSC_TESTING
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
#else
	return GetCurrentTimestamp();
#endif
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

/*
 * Initializes a new entry and appends its slot to the eviction array; the
 * caller holds the exclusive lock, has room (entries < max_entries) and
 * counts the entry.
 */
static void
entry_init(void *entry)
{
	PsscEntryHeader *hdr = entry_header(entry);
	PsscSlot   *slots = entry_slots(entry);
	PsscEvictSlot *es;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	Assert(store_state->entries < store_state->max_entries);
	SpinLockInit(&hdr->mutex);
	hdr->encoding = GetDatabaseEncoding();
	hdr->evict_index = (int) store_state->entries;
	hdr->last_bucket = PSSC_BUCKET_NONE;
	es = &store_state->evict_slots[hdr->evict_index];
	es->last_bucket = PSSC_BUCKET_NONE;
	es->usage = pssc_usage_init();
	es->entry = entry;
	hdr->calls_total = 0;
	hdr->exec_time_total = 0.0;
	hdr->stats_since = GetCurrentTimestamp();
	for (int i = 0; i < store_state->bucket_count; i++)
		pssc_slot_init(&slots[i]);
	exemplars_clear(entry_exemplars(entry), store_state->exemplar_nkeys,
					store_state->exemplar_value_len);
}

/*
 * Removes the entry in eviction slot idx from the hash table and the array
 * (the last slot moves into its hole) and uncounts it; the caller holds
 * the exclusive lock. Reads only the removed entry and, if a slot moves,
 * writes the evict_index of the entry it points to.
 */
static void
entry_remove(int64 idx)
{
	PsscEvictSlot *slots = store_state->evict_slots;
	int64		last = store_state->entries - 1;
	void	   *entry = slots[idx].entry;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	Assert(idx >= 0 && idx <= last);
	Assert(entry_header(entry)->evict_index == idx);
	hash_search(store_htab, entry, HASH_REMOVE, NULL);
	if (idx != last)
	{
		slots[idx] = slots[last];
		entry_header(slots[idx].entry)->evict_index = (int) idx;
	}
	store_state->entries--;
}

/*
 * Copies the exemplar values ex[0, exlen) (store.h) into the entry's slots;
 * the caller holds the entry spinlock. Malformed input, and values for a
 * slot or of a length this store does not have (tagset.c never produces
 * them), are skipped.
 */
static void
exemplars_write(void *entry, const char *ex, size_t exlen)
{
	char	   *block = entry_exemplars(entry);
	int			nkeys = store_state->exemplar_nkeys;
	int			value_len = store_state->exemplar_value_len;
	size_t		pos = 0;

	while (pos + 1 + sizeof(uint16) <= exlen)
	{
		uint8		idx = (uint8) ex[pos];
		uint16		len;

		memcpy(&len, ex + pos + 1, sizeof(uint16));
		pos += 1 + sizeof(uint16);
		if (len > exlen - pos)
			break;
		if (idx < nkeys && len <= value_len)
		{
			char	   *slot = block + (Size) idx * EXEMPLAR_SLOT_SIZE(value_len);

			memcpy(slot, &len, sizeof(uint16));
			memcpy(slot + sizeof(uint16), ex + pos, len);
		}
		pos += len;
	}
}

/*
 * Adds one call to the entry's ring in bucket current_bucket, read under the
 * entry spinlock: that bucket is live at the moment of the write, and it is
 * >= the entry's last_bucket (which an earlier writer read the same way from
 * the monotonic watermark), so every id in the ring stays <= current_bucket.
 * last_bucket and usage in the entry's eviction slot change under the same
 * spinlock. The caller holds the lock (any mode) and has already advanced
 * the watermark (observe_current_bucket()). Returns the id written.
 */
static int64
entry_accum(void *entry, double elapsed_ms, const char *ex, size_t exlen)
{
	PsscEntryHeader *hdr = entry_header(entry);
	PsscSlot   *slots = entry_slots(entry);
	PsscEvictSlot *es = entry_slot(entry);
	int			count = store_state->bucket_count;
	int64		bucket_id;

	SpinLockAcquire(&hdr->mutex);
	Assert(es->entry == entry && es->last_bucket == hdr->last_bucket);
	bucket_id = watermark_read();
	Assert(hdr->last_bucket == PSSC_BUCKET_NONE || hdr->last_bucket <= bucket_id);

	/* lazy per-entry rollover: relabel the slot if it holds an older bucket */
	{
		PsscSlot   *slot = &slots[pssc_bucket_slot_index(bucket_id, count)];

		pssc_slot_roll(slot, bucket_id);
		pssc_slot_accum(slot, elapsed_ms);
	}
	hdr->last_bucket = bucket_id;
	es->last_bucket = bucket_id;
	hdr->calls_total++;
	hdr->exec_time_total += elapsed_ms;
	pssc_usage_exec(&es->usage);
	if (exlen > 0)
		exemplars_write(entry, ex, exlen);

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
#ifdef PSSC_TESTING
	bool		fail = debug_fail_next_eviction_alloc;

	debug_fail_next_eviction_alloc = false;
#else
	const bool	fail = false;
#endif

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

#ifdef USE_ASSERT_CHECKING
/*
 * The eviction array matches the table: as many slots as entries, each
 * pointing to an entry that points back to it, with that entry's
 * last_bucket. The caller holds the exclusive lock.
 */
static void
assert_evict_slots(void)
{
	PsscEvictSlot *slots = store_state->evict_slots;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	Assert(store_state->entries == hash_get_num_entries(store_htab));
	Assert(store_state->entries <= store_state->max_entries);
	for (int64 i = 0; i < store_state->entries; i++)
	{
		PsscEntryHeader *hdr = entry_header(slots[i].entry);

		Assert(hdr->evict_index == i);
		Assert(slots[i].last_bucket == hdr->last_bucket);
	}
}
#endif

/*
 * The scan of the compact eviction array shared by an eviction pass
 * (store_evict(), step 1) and the reclaim worker (pssc_store_reclaim_dead()):
 * removes every entry that is dead at current (the watermark the caller
 * raised under the exclusive lock it holds), at which the last slot moves
 * into the hole and is judged next. Returns the number removed.
 *
 * Under pressure, every surviving entry's usage also decays and, if sel is
 * not NULL, is offered to the selector; *nlive counts the survivors. The
 * reclaim worker passes pressure = false: it only removes dead entries and
 * leaves the survivors' usage alone, since decay paces eviction and must
 * not depend on how often an idle worker wakes up.
 */
static int64
evict_scan(int64 current, bool pressure, PsscEvictSelect *sel, int64 *nlive)
{
	PsscEvictSlot *slots = store_state->evict_slots;
	int			count = store_state->bucket_count;
	int64		dead = 0;
	int64		live = 0;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	Assert(pressure || sel == NULL);
	for (int64 i = 0; i < store_state->entries;)
	{
		PsscEvictSlot *es = &slots[i];

		if (pssc_bucket_entry_is_dead(es->last_bucket, current, count))
		{
			/* the last slot (not yet seen) moves here: judge it next */
			entry_remove(i);
			dead++;
			continue;
		}
		if (pressure)
		{
			pssc_usage_decay(&es->usage);
			if (sel != NULL)
				pssc_evict_select_offer(sel, es->last_bucket, es->usage, es->entry);
		}
		live++;
		i++;
	}
	if (nlive != NULL)
		*nlive = live;
	return dead;
}

/*
 * One eviction pass (§5.3); the caller holds the exclusive lock and found
 * the table at max_entries. Returns the number of entries removed.
 *
 *	1. Raise current_bucket to the clock, as readers do, and scan the
 *	   compact eviction array once (not the entries; top of this file):
 *	   reclaim every dead entry (no slot in the live window), at which the
 *	   last slot moves into the hole and is judged next; every surviving
 *	   entry's usage decays by PSSC_USAGE_DECREASE_FACTOR, as in pgss's
 *	   entry_dealloc(), and is offered to a selector
 *	   (pssc_evict_select_*()) that keeps the pssc_evict_target() first
 *	   live entries in eviction order: last_bucket, then usage, then scan
 *	   order (the order of the array as the pass meets it). A pass never
 *	   evicts more live entries than the target, so the victims are among
 *	   them.
 *	2. If the dead entries were fewer than the target, evict the first
 *	   (target - dead) live entries in that order: exactly the front of a
 *	   full sort of every live entry (pssc_evict_sort()), at O(n) cost
 *	   instead of O(n log n), in one scan, with a buffer of target entries
 *	   (not nlive) that is reused across passes.
 *	3. dealloc += 1; reclaimed_entries += the dead entries removed,
 *	   evicted_entries += the live ones (a sign that max_entries is too
 *	   small, where reclaiming dead entries is normal housekeeping).
 *
 * Only the removed entries are read (for their key and evict_index), plus
 * the one whose slot moves into each hole (its evict_index is written).
 *
 * The slots and entries are read and written without the entry spinlocks:
 * those are only ever taken by a backend holding the table lock (writers
 * and readers alike take it in shared mode), so under the exclusive lock
 * nobody else can be touching any entry or slot.
 *
 * The selector's buffer (evict_buffer()) is allocated with
 * MCXT_ALLOC_NO_OOM: recording runs inside user statements, so an
 * out-of-memory condition must not fail the statement. If it cannot be
 * allocated, only the dead entries are freed; the caller then drops its
 * record (dropped_records) if that left no room. Nothing between the
 * allocation and the pfree can raise an ERROR, and the table and the array
 * are consistent after every single removal.
 */
static int64
store_evict(void)
{
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

	dead = evict_scan(current, true, cands != NULL ? &sel : NULL, &nlive);
	nvictims = pssc_evict_live_count(target, dead, nlive);
	if (nvictims > 0 && cands != NULL)
	{
		size_t		n = pssc_evict_select_finish(&sel, (size_t) nvictims);

		Assert(n == (size_t) nvictims);
		/* removals move slots, so look each victim's index up afresh */
		for (size_t j = 0; j < n; j++)
			entry_remove(entry_header(cands[j].entry)->evict_index);
		evicted = (int64) n;
	}
	if (transient && cands != NULL)
		pfree(cands);

	store_state->dealloc++;
	store_state->reclaimed_entries += dead;
	store_state->evicted_entries += evicted;
#ifdef USE_ASSERT_CHECKING
	assert_evict_slots();
#endif
	return dead + evicted;
}

int64
pssc_store_reclaim_dead(int64 *last_watermark)
{
	int64		current;
	int64		dead;

	if (store_state == NULL || store_htab == NULL)
		return 0;

	/*
	 * An entry only dies when the watermark moves past its live window: if
	 * it has not moved since our last pass (which left no dead entry), and
	 * every later write lands at or above it, nothing can be dead now. So
	 * an idle worker takes the exclusive lock at most once per bucket.
	 * Like every reader, raise the watermark to the clock (under the lock).
	 */
	LWLockAcquire(store_state->lock, LW_SHARED);
	current = observe_current_bucket(PSSC_BUCKET_NONE);
	LWLockRelease(store_state->lock);
	if (*last_watermark != PSSC_BUCKET_NONE && current == *last_watermark)
		return 0;

	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	current = observe_current_bucket(PSSC_BUCKET_NONE);
	dead = evict_scan(current, false, NULL, NULL);

	/*
	 * Housekeeping, not an eviction pass: reclaimed_entries counts what was
	 * removed, as it does for passes, but dealloc (passes forced by a full
	 * table, a sign of undersizing) and evicted_entries are left alone.
	 */
	store_state->reclaimed_entries += dead;
#ifdef USE_ASSERT_CHECKING
	assert_evict_slots();
#endif
	LWLockRelease(store_state->lock);
	*last_watermark = current;
	return dead;
}

static inline bool
stats_nonzero(const PsscTagsetStats *s)
{
	return s->invalid_tags != 0 || s->dropped_tags != 0 ||
		s->heuristic_scans != 0 || s->regex_compile_failures != 0 ||
		s->capped_tags != 0 || s->cap_table_full != 0 ||
		s->exemplars_dropped != 0;
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
#ifdef PSSC_TESTING
	/* testing aid: a stall between two adds, which a reset must not split */
	if (unlikely(flush_test_hook != NULL))
		flush_test_hook(flush_test_hook_arg);
#endif
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
	if (stats->exemplars_dropped)
		pg_atomic_fetch_add_u64(&store_state->exemplar_values_dropped,
								stats->exemplars_dropped);
	if (utility_missing_queryid)
		pg_atomic_fetch_add_u64(&store_state->utility_missing_queryid,
								utility_missing_queryid);
	memset(stats, 0, sizeof(*stats));
}

static PsscStoreResult record_impl(const PsscKey *key, int64 bucket_id,
								   double elapsed_ms, PsscTagsetStats *pending,
								   const char *ex, size_t exlen);

PsscStoreResult
pssc_store_record(const PsscKey *key, double elapsed_ms)
{
	return pssc_store_record_with_stats(key, elapsed_ms, NULL);
}

PsscStoreResult
pssc_store_record_with_stats(const PsscKey *key, double elapsed_ms,
							 PsscTagsetStats *pending)
{
	return pssc_store_record_ex(key, elapsed_ms, pending, NULL, 0);
}

PsscStoreResult
pssc_store_record_ex(const PsscKey *key, double elapsed_ms,
					 PsscTagsetStats *pending, const char *ex, size_t exlen)
{
	if (store_state == NULL || store_htab == NULL)
		return PSSC_STORE_UNAVAILABLE;

	/*
	 * The bucket in which the execution completes. record_impl() re-reads
	 * the clock once it holds the lock, so a stall moves the call forward.
	 */
	return record_impl(key, pssc_store_clock_bucket(), elapsed_ms, pending,
					   ex, exlen);
}

PsscStoreResult
pssc_store_record_at(const PsscKey *key, int64 bucket_id, double elapsed_ms)
{
	return record_impl(key, bucket_id, elapsed_ms, NULL, NULL, 0);
}

static PsscStoreResult
record_impl(const PsscKey *key, int64 bucket_id, double elapsed_ms,
			PsscTagsetStats *pending, const char *ex, size_t exlen)
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

#ifdef PSSC_TESTING
	if (unlikely(record_test_hook != NULL))
		record_test_hook(record_test_hook_arg);
#endif

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
		(void) entry_accum(entry, elapsed_ms, ex, exlen);
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
		(void) entry_accum(entry, elapsed_ms, ex, exlen);
	if (pending != NULL && stats_nonzero(pending))
		add_diagnostics_locked(pending, 0);

	Assert(store_state->entries == hash_get_num_entries(store_htab));
	Assert(store_state->entries <= store_state->max_entries);
	Assert(entry == NULL || entry_slot(entry)->entry == entry);
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
		double		usage;

		/*
		 * The key is immutable once inserted; the rest, and the usage in
		 * the entry's eviction slot, need the spinlock.
		 */
		memcpy(copy, entry, keysize);
		SpinLockAcquire(&hdr->mutex);
		memcpy(copy + keysize, (char *) entry + keysize, entrysize - keysize);
		usage = entry_slot(entry)->usage;
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
		view.usage = usage;
		view.calls_total = chdr->calls_total;
		view.exec_time_total = chdr->exec_time_total;
		view.stats_since = chdr->stats_since;
		view.bucket_count = store_state->bucket_count;
		view.slots = entry_slots(copy);
		view.exemplar_nkeys = store_state->exemplar_nkeys;
		view.exemplar_value_len = store_state->exemplar_value_len;
		view.exemplars = entry_exemplars(copy);
		fn(&view, arg);
	}
	LWLockRelease(store_state->lock);
	pfree(copy);
}

bool
pssc_store_exemplar(const PsscStoreEntryView *view, int i, const char **val,
					size_t *len)
{
	const char *slot;
	uint16		l;

	if (i < 0 || i >= view->exemplar_nkeys)
		return false;
	slot = view->exemplars + (Size) i * EXEMPLAR_SLOT_SIZE(view->exemplar_value_len);
	memcpy(&l, slot, sizeof(uint16));
	if (l == EXEMPLAR_EMPTY || l > view->exemplar_value_len)
		return false;
	*val = slot + sizeof(uint16);
	*len = l;
	return true;
}

/* Removes every entry; the caller holds the exclusive lock. */
static void
store_clear_entries_locked(void)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	hash_seq_init(&seq, store_htab);
	/* key_hash() runs under the exclusive lock, so the mode is stable */
	while ((entry = hash_seq_search(&seq)) != NULL)
		hash_search(store_htab, entry, HASH_REMOVE, NULL);
	store_state->entries = 0;	/* and so the eviction array is empty */
}

void
pssc_store_reset(void)
{
	if (store_state == NULL || store_htab == NULL)
		return;

	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	store_clear_entries_locked();
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
	pg_atomic_write_u64(&store_state->exemplar_values_dropped, 0);
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
#ifdef PSSC_TESTING
	c->force_collisions = store_state->force_collisions;
#endif
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
	c->exemplar_values_dropped =
		(int64) pg_atomic_read_u64(&store_state->exemplar_values_dropped);
	c->exemplar_nkeys = store_state->exemplar_nkeys;
	c->exemplar_value_len = store_state->exemplar_value_len;
	c->exemplar_shmem_bytes = mul_size((Size) store_state->max_entries,
									   store_state->exemplar_block);
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
pssc_store_get_header(PsscStoreCounters *c)
{
	if (!pssc_store_get_counters(c))
		return false;
	/* as every reader (§5.2); the watermark is lock-free and never reset */
	c->current_bucket = observe_current_bucket(PSSC_BUCKET_NONE);
	return true;
}

/*
 * The most passes pssc_store_get_info() makes over the table (DESIGN.md
 * §7). A pass is repeated only if the watermark moved during it, which
 * happens at most once per bucket_interval unless the clock is stepped, so
 * a second pass is rare and a third one rarer still.
 */
#define INFO_MAX_PASSES 3

bool
pssc_store_get_info(PsscStoreCounters *c, int64 *oldest_bucket)
{
	HASH_SEQ_STATUS seq;
	void	   *entry;
	int			count;
	int64	   *ids;
	int64		oldest = PSSC_BUCKET_NONE;
	int64		current = PSSC_BUCKET_NONE;
	bool		can_interrupt;

	memset(c, 0, sizeof(*c));
	*oldest_bucket = PSSC_BUCKET_NONE;
	if (store_state == NULL || store_htab == NULL)
		return false;
	count = store_state->bucket_count;
	ids = palloc(count * sizeof(int64));

	/*
	 * The lock holds off interrupts, so a pass gives way to one itself: it
	 * stops, releases the lock and lets CHECK_FOR_INTERRUPTS() service it
	 * (a cancel or termination raises ERROR or FATAL there). Any pass,
	 * the last one included, gives way to a cancel or termination; the
	 * last one completes despite any other interrupt, which waits for the
	 * end of the pass, so that the passes stay bounded. Only if the caller
	 * could have taken the interrupt, or the pass would just start over
	 * with it still pending.
	 */
	can_interrupt = INTERRUPTS_CAN_BE_PROCESSED();

	for (int pass = 1;; pass++)
	{
		bool		interrupted = false;
		int64		after;

		/*
		 * Each pass is a fresh snapshot under one acquisition of the lock
		 * (counters, watermark and slots together, wholly before or after
		 * any reset): a scan is never resumed across a release.
		 */
		LWLockAcquire(store_state->lock, LW_SHARED);
		read_counters_locked(c);

		/* as every reader (§5.2): advance the watermark to the clock first */
		current = observe_current_bucket(PSSC_BUCKET_NONE);

		/*
		 * Judge every slot against one watermark, the one returned as
		 * current_bucket, so the row is self-consistent; if a writer or
		 * reader moved it during the pass, make another one, up to
		 * INFO_MAX_PASSES. The last pass is kept whatever happens to the
		 * watermark meanwhile, so oldest_bucket may be up to that
		 * movement stale when the row is returned (but is never a bucket
		 * expired at the row's own current_bucket); and every row comes
		 * from a whole pass.
		 */
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

#ifdef PSSC_TESTING
			if (unlikely(info_scan_test_hook != NULL))
				info_scan_test_hook(info_scan_test_hook_arg);
#endif

			/* on WIN32, this also dispatches queued signals */
			if (unlikely(INTERRUPTS_PENDING_CONDITION()) && can_interrupt &&
				(pass < INFO_MAX_PASSES || QueryCancelPending || ProcDiePending))
			{
				hash_seq_term(&seq);
				interrupted = true;
				break;
			}
		}
		after = watermark_read();
		LWLockRelease(store_state->lock);

		if (!interrupted && (after == current || pass >= INFO_MAX_PASSES))
			break;
		CHECK_FOR_INTERRUPTS();

		/*
		 * A cut-short last pass whose cancel did not raise an error (none
		 * in practice: the statement is running) is made again: every row
		 * comes from a whole pass.
		 */
		if (interrupted && pass >= INFO_MAX_PASSES)
			pass = INFO_MAX_PASSES - 1;
	}
	c->current_bucket = current;
	CHECK_FOR_INTERRUPTS();

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

#ifdef PSSC_TESTING
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

int64
pssc_store_debug_evict_slots(PsscEvictSlotVisitor fn, void *arg)
{
	int64		n;

	if (store_state == NULL || store_htab == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");

	LWLockAcquire(store_state->lock, LW_SHARED);
	n = store_state->entries;
	for (int64 i = 0; i < n; i++)
	{
		PsscEvictSlot *es = &store_state->evict_slots[i];
		PsscEntryHeader *hdr = entry_header(es->entry);
		int64		last_bucket;
		double		usage;

		/* the slot changes under the entry's spinlock (top of this file) */
		SpinLockAcquire(&hdr->mutex);
		last_bucket = es->last_bucket;
		usage = es->usage;
		SpinLockRelease(&hdr->mutex);
		fn(i, (const PsscKey *) es->entry, last_bucket, usage, arg);
	}
	LWLockRelease(store_state->lock);
	return n;
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
#endif							/* PSSC_TESTING */


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

#ifdef PSSC_TESTING
	SpinLockAcquire(&store_state->clock_mutex);
	b->clock_mode = store_state->clock_mode;
	b->clock_value = store_state->clock_value;
	SpinLockRelease(&store_state->clock_mutex);
#endif

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
	bool		bad_is_evict = false;
	int64		bad_index = 0;
	PsscEvictSlot bad_evict = {0};

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
		PsscEvictSlot es = {0};
		int64		idx;

		memcpy(copy, entry, keysize);
		SpinLockAcquire(&hdr->mutex);
		memcpy(copy + keysize, (char *) entry + keysize, entrysize - keysize);
		/* evict_index changes only under the exclusive lock */
		idx = hdr->evict_index;
		if (idx >= 0 && idx < entries)
			es = store_state->evict_slots[idx];
		SpinLockRelease(&hdr->mutex);
		current = watermark_read();	/* after the copy, as readers do */

		n++;
		problem = pssc_ring_check(entry_slots(copy), store_state->bucket_count,
								  entry_header(copy)->last_bucket, current,
								  &bad_slot);

		/*
		 * The eviction array: the entry's slot is in use and points back
		 * to it with its last_bucket. With n == entries (below) this makes
		 * the slots in use and the entries a one-to-one match.
		 */
		if (problem == NULL)
		{
			if (idx < 0 || idx >= entries)
				problem = "slot index out of range";
			else if (es.entry != entry)
				problem = "slot points to another entry";
			else if (es.last_bucket != entry_header(copy)->last_bucket)
				problem = "slot last_bucket differs from the entry's";
			bad_is_evict = (problem != NULL);
			if (bad_is_evict)
				bad_slot = -1;
			bad_index = idx;
			bad_evict = es;
		}
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

	if (bad_is_evict)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_stat_statement_context eviction array invariant violated: %s",
						problem),
				 errdetail("queryid " INT64_FORMAT ", last_bucket " INT64_FORMAT
						   ", eviction slot " INT64_FORMAT " of " INT64_FORMAT
						   " (last_bucket " INT64_FORMAT ").",
						   bad_key->queryid, bad_last, bad_index, entries,
						   bad_evict.last_bucket)));
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

#ifdef PSSC_TESTING

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

int64
pssc_store_debug_observe_clock(void)
{
	if (store_state == NULL)
		elog(ERROR, "pg_stat_statement_context shared store is not set up");
	return observe_current_bucket(PSSC_BUCKET_NONE);
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
#endif							/* PSSC_TESTING */

/* -------------------------------------------------------- persistence */

/*
 * Saving the store across clean restarts (DESIGN.md §5.5), after
 * pg_stat_statements' pgss_shmem_shutdown() and pgss_shmem_startup().
 *
 * File layout (native byte order and alignment: the file is only read by
 * the same build on the same machine; PG_MAJORVERSION_NUM and the format
 * version guard the rest):
 *	PsscDumpHeader
 *	nentries times, in eviction array order [0, entries):
 *		PsscDumpRecord, tags_len bytes of tags, bucket_count PsscSlot
 *	pg_crc32c of everything above
 * and nothing after it.
 */
#define PSSC_DUMP_FILE		PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_stat_statement_context.stat"
#define PSSC_DUMP_FILE_TMP	PSSC_DUMP_FILE ".tmp"
#define PSSC_DUMP_MAGIC		0x50535343	/* "PSSC" */
#define PSSC_DUMP_FORMAT	1			/* bump on any layout change */
#define PSSC_DUMP_EXT_VERSION_LEN 20
#define PSSC_CONTROL_FILE	"global/pg_control"

#ifndef PSSC_EXT_VERSION
#error "PSSC_EXT_VERSION must be defined (see Makefile)"
#endif

typedef struct PsscDumpHeader
{
	uint32		magic;
	uint32		format;			/* PSSC_DUMP_FORMAT */
	uint32		pg_major;		/* PG_MAJORVERSION_NUM */
	char		ext_version[PSSC_DUMP_EXT_VERSION_LEN];	/* PSSC_EXT_VERSION */
	int64		epoch;
	int64		interval_us;
	int32		bucket_count;
	int32		max_entries;
	int32		max_tagset_bytes;
	int32		pad;
	int64		current_bucket;
	int64		nentries;
	TimestampTz stats_reset;
	int64		dealloc;
	int64		reclaimed_entries;
	int64		evicted_entries;
	uint64		invalid_tags;
	uint64		dropped_tags;
	uint64		regex_compile_failures;
	uint64		heuristic_scans;
	uint64		capped_tags;
	uint64		cap_table_full;
	uint64		utility_missing_queryid;
	uint64		dropped_records;
} PsscDumpHeader;

/* test/t/028_persist.pl patches these offsets */
StaticAssertDecl(offsetof(PsscDumpHeader, format) == 4, "dump header layout");
StaticAssertDecl(offsetof(PsscDumpHeader, pg_major) == 8, "dump header layout");
StaticAssertDecl(offsetof(PsscDumpHeader, ext_version) == 12, "dump header layout");
StaticAssertDecl(sizeof(PSSC_EXT_VERSION) <= PSSC_DUMP_EXT_VERSION_LEN,
				 "extension version too long for the dump header");

typedef struct PsscDumpRecord
{
	Oid			dbid;
	Oid			userid;
	int64		queryid;
	int64		last_bucket;
	int64		calls_total;
	double		exec_time_total;
	double		usage;
	TimestampTz stats_since;
	int32		encoding;
	uint32		tags_hash;
	uint16		tags_len;
	uint8		toplevel;
	uint8		pad[5];
} PsscDumpRecord;

/* test/t/028_persist.pl patches the first record's calls_total */
StaticAssertDecl(sizeof(PsscDumpHeader) == 176, "dump header layout");
StaticAssertDecl(offsetof(PsscDumpRecord, queryid) == 8, "dump record layout");
StaticAssertDecl(offsetof(PsscDumpRecord, calls_total) == 24, "dump record layout");

/*
 * Whether pg_control says the cluster shut down cleanly: every child has
 * exited and the checkpointer wrote a shutdown checkpoint, so no process
 * can be in the middle of an update of the store. An immediate shutdown
 * also exits the postmaster with code 0, but its children were killed at
 * any point (pg_control then still says "in production").
 */
static bool
cluster_shut_down_cleanly(void)
{
	ControlFileData cf;
	pg_crc32c	crc;
	int			fd;
	ssize_t		r;

	/*
	 * Read directly, not with get_controlfile(): this runs in proc_exit(),
	 * where an ERROR would be promoted to FATAL.
	 */
	fd = open(PSSC_CONTROL_FILE, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return false;
	r = read(fd, &cf, sizeof(cf));
	close(fd);
	if (r != (ssize_t) sizeof(cf) || cf.pg_control_version != PG_CONTROL_VERSION)
		return false;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &cf, offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);
	return EQ_CRC32C(crc, cf.crc) &&
		(cf.state == DB_SHUTDOWNED || cf.state == DB_SHUTDOWNED_IN_RECOVERY);
}

static bool
dump_write(FILE *file, const void *buf, size_t len, pg_crc32c *crc)
{
	if (len == 0)
		return true;
	COMP_CRC32C(*crc, buf, len);
	return fwrite(buf, len, 1, file) == 1;
}

/*
 * on_shmem_exit callback of the postmaster: writes the store to
 * PSSC_DUMP_FILE_TMP and renames it over PSSC_DUMP_FILE. It runs after
 * every other process has exited, so it reads the store without any lock.
 */
static void
store_shmem_shutdown(int code, Datum arg)
{
	PsscDumpHeader hdr;
	FILE	   *file = NULL;
	pg_crc32c	crc;
	int64		n;

	if (store_state == NULL || store_htab == NULL || !pssc_save)
		return;
	if (code != 0 || !cluster_shut_down_cleanly())
	{
		ereport(LOG,
				(errmsg("pg_stat_statement_context: not saving statistics: the server did not shut down cleanly")));
		return;
	}

	n = store_state->entries;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = PSSC_DUMP_MAGIC;
	hdr.format = PSSC_DUMP_FORMAT;
	hdr.pg_major = PG_MAJORVERSION_NUM;
	strlcpy(hdr.ext_version, PSSC_EXT_VERSION, sizeof(hdr.ext_version));
	hdr.epoch = store_state->epoch;
	hdr.interval_us = store_state->interval_us;
	hdr.bucket_count = store_state->bucket_count;
	hdr.max_entries = store_state->max_entries;
	hdr.max_tagset_bytes = store_state->max_tagset_bytes;
	hdr.current_bucket = watermark_read();
	hdr.nentries = n;
	hdr.stats_reset = store_state->stats_reset;
	hdr.dealloc = store_state->dealloc;
	hdr.reclaimed_entries = store_state->reclaimed_entries;
	hdr.evicted_entries = store_state->evicted_entries;
	hdr.invalid_tags = pg_atomic_read_u64(&store_state->invalid_tags);
	hdr.dropped_tags = pg_atomic_read_u64(&store_state->dropped_tags);
	hdr.regex_compile_failures = pg_atomic_read_u64(&store_state->regex_compile_failures);
	hdr.heuristic_scans = pg_atomic_read_u64(&store_state->heuristic_scans);
	hdr.capped_tags = pg_atomic_read_u64(&store_state->capped_tags);
	hdr.cap_table_full = pg_atomic_read_u64(&store_state->cap_table_full);
	hdr.utility_missing_queryid = pg_atomic_read_u64(&store_state->utility_missing_queryid);
	hdr.dropped_records = pg_atomic_read_u64(&store_state->dropped_records);

	file = AllocateFile(PSSC_DUMP_FILE_TMP, PG_BINARY_W);
	if (file == NULL)
		goto error;

	INIT_CRC32C(crc);
	if (!dump_write(file, &hdr, sizeof(hdr), &crc))
		goto error;
	for (int64 i = 0; i < n; i++)
	{
		PsscEvictSlot *es = &store_state->evict_slots[i];
		const PsscKey *key = (const PsscKey *) es->entry;
		PsscEntryHeader *eh = entry_header(es->entry);
		PsscDumpRecord rec;

		memset(&rec, 0, sizeof(rec));
		rec.dbid = key->dbid;
		rec.userid = key->userid;
		rec.queryid = key->queryid;
		rec.last_bucket = eh->last_bucket;
		rec.calls_total = eh->calls_total;
		rec.exec_time_total = eh->exec_time_total;
		rec.usage = es->usage;
		rec.stats_since = eh->stats_since;
		rec.encoding = eh->encoding;
		rec.tags_hash = key->tags_hash;
		rec.tags_len = key->tags_len;
		rec.toplevel = key->toplevel ? 1 : 0;
		if (!dump_write(file, &rec, sizeof(rec), &crc) ||
			!dump_write(file, key->tags, key->tags_len, &crc) ||
			!dump_write(file, entry_slots(es->entry),
						sizeof(PsscSlot) * store_state->bucket_count, &crc))
			goto error;
	}
	FIN_CRC32C(crc);
	if (fwrite(&crc, sizeof(crc), 1, file) != 1)
		goto error;
	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}
	file = NULL;

	/* durable, and atomic: a reader sees the old file or the new one */
	if (durable_rename(PSSC_DUMP_FILE_TMP, PSSC_DUMP_FILE, LOG) != 0)
	{
		unlink(PSSC_DUMP_FILE_TMP);
		return;
	}
	ereport(LOG,
			(errmsg("pg_stat_statement_context: saved " INT64_FORMAT " entries to \"%s\"",
					n, PSSC_DUMP_FILE)));
	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m", PSSC_DUMP_FILE_TMP)));
	if (file)
		FreeFile(file);
	unlink(PSSC_DUMP_FILE_TMP);
}

/* One saved entry, as read by the first pass of store_load(). */
typedef struct PsscLoadRecord
{
	PsscDumpRecord rec;
	char		fate;			/* 'k'ept, 'd'ead, 's'kipped, 'e'victed */
} PsscLoadRecord;

typedef enum
{
	LOAD_OK,
	LOAD_EOF,					/* short read: truncated */
	LOAD_IO,					/* read error (errno set) */
	LOAD_INVALID,				/* the data fails a check */
	LOAD_NOMEM,					/* out of memory */
} PsscLoadStatus;

/*
 * Memory for the load, sized from the file: never an ERROR (which would be
 * FATAL for the postmaster), so the file is discarded instead.
 */
static void *
load_alloc(Size size)
{
	return MemoryContextAllocExtended(CurrentMemoryContext, size,
									  MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
}

static PsscLoadStatus
load_read(FILE *file, void *buf, size_t len, pg_crc32c *crc)
{
	if (len == 0)
		return LOAD_OK;
	if (fread(buf, len, 1, file) != 1)
		return ferror(file) ? LOAD_IO : LOAD_EOF;
	if (crc != NULL)
		COMP_CRC32C(*crc, buf, len);
	return LOAD_OK;
}

/*
 * Reads one record with its tags and slots, and checks what a well-formed
 * entry satisfies (src/counters.h's ring invariants against the saved
 * watermark, a tag set "k\0v\0..." that fits the saved max_tagset_bytes,
 * and a tags_hash recomputed from those tags).
 */
static PsscLoadStatus
load_record(FILE *file, const PsscDumpHeader *hdr, PsscDumpRecord *rec,
			char *tags, PsscSlot *slots, pg_crc32c *crc)
{
	PsscLoadStatus st;
	size_t		off = 0;
	PsscTagView tag;
	int			bad;

	if ((st = load_read(file, rec, sizeof(*rec), crc)) != LOAD_OK)
		return st;
	if (rec->tags_len > hdr->max_tagset_bytes || rec->toplevel > 1 ||
		!PG_VALID_BE_ENCODING(rec->encoding) ||
		rec->last_bucket == PSSC_BUCKET_NONE || rec->calls_total < 1)
		return LOAD_INVALID;
	if ((st = load_read(file, tags, rec->tags_len, crc)) != LOAD_OK ||
		(st = load_read(file, slots, sizeof(PsscSlot) * hdr->bucket_count,
						crc)) != LOAD_OK)
		return st;
	/* well-formed tags (a null value is key \0 \0 \0) up to the end */
	while (off < rec->tags_len && pssc_tagset_next(tags, rec->tags_len, &off, &tag))
		;
	if (off != rec->tags_len)
		return LOAD_INVALID;
	/* defense in depth: the key's hash must be the one its tags give */
	if (pssc_tagset_hash(tags, rec->tags_len) != rec->tags_hash)
		return LOAD_INVALID;
	if (pssc_ring_check(slots, hdr->bucket_count, rec->last_bucket,
						hdr->current_bucket, &bad) != NULL)
		return LOAD_INVALID;
	return LOAD_OK;
}

/*
 * Inserts a saved entry into the table and appends its slot to the
 * eviction array, as entry_init() and entry_accum() would have left it;
 * slots that are no longer live at current are cleared. The caller holds
 * the exclusive lock and has room. Returns false on a duplicate key.
 */
static bool
load_insert(PsscKey *key, const PsscDumpRecord *rec, const char *tags,
			const PsscSlot *slots, int64 current)
{
	void	   *entry;
	bool		found;
	PsscEntryHeader *eh;
	PsscSlot   *es_slots;
	PsscEvictSlot *es;
	int			count = store_state->bucket_count;

	Assert(LWLockHeldByMeInMode(store_state->lock, LW_EXCLUSIVE));
	if (!pssc_store_build_key(key, rec->dbid, rec->userid, rec->queryid,
							  rec->toplevel != 0, tags, rec->tags_len,
							  rec->tags_hash))
		return false;
	entry = hash_search(store_htab, key, HASH_ENTER_NULL, &found);
	if (entry == NULL || found)
		return false;
	eh = entry_header(entry);
	es_slots = entry_slots(entry);
	SpinLockInit(&eh->mutex);
	eh->encoding = rec->encoding;
	eh->evict_index = (int) store_state->entries;
	eh->last_bucket = rec->last_bucket;
	eh->calls_total = rec->calls_total;
	eh->exec_time_total = rec->exec_time_total;
	eh->stats_since = rec->stats_since;
	for (int i = 0; i < count; i++)
	{
		if (slots[i].bucket_id != PSSC_BUCKET_NONE &&
			pssc_bucket_is_live(slots[i].bucket_id, current, count))
			es_slots[i] = slots[i];
		else
			pssc_slot_init(&es_slots[i]);
	}
	/* exemplars are not saved (§5.5) */
	exemplars_clear(entry_exemplars(entry), store_state->exemplar_nkeys,
					store_state->exemplar_value_len);
	es = &store_state->evict_slots[eh->evict_index];
	es->last_bucket = rec->last_bucket;
	es->usage = rec->usage;
	es->entry = entry;
	store_state->entries++;
	return true;
}

/*
 * Loads PSSC_DUMP_FILE into the empty store (in the postmaster, before any
 * other process exists) and unlinks it, whatever the outcome: a crash
 * before the next clean shutdown must not replay it. A file that cannot be
 * used is discarded with a LOG message; startup never fails because of it.
 *
 * Two passes: the first reads and checks every record (and the checksum),
 * and decides which ones to keep; the second inserts those.
 *	- a different format, PostgreSQL major or extension version, or a
 *	  different bucket_interval or bucket_count: the file is discarded;
 *	- the saved epoch is kept, so the saved bucket ids keep their meaning,
 *	  and current_bucket becomes max(saved, the clock's bucket);
 *	- entries dead at that watermark are dropped (reclaimed_entries) and
 *	  slots that expired are cleared;
 *	- entries whose tag set exceeds the current max_tagset_bytes are skipped;
 *	- if more live entries remain than the current max_entries, the excess
 *	  is evicted in the §5.3 order (last_bucket, usage, array order), as one
 *	  eviction pass (dealloc, evicted_entries).
 */
static void
store_load(void)
{
	FILE	   *file = NULL;
	PsscDumpHeader hdr;
	PsscLoadStatus st = LOAD_OK;
	MemoryContext cxt;
	MemoryContext oldcxt;
	PsscLoadRecord *recs = NULL;
	int64		nalloc;
	char	   *tags;
	PsscSlot   *slots = NULL;
	PsscKey    *key;
	pg_crc32c	crc;
	pg_crc32c	saved_crc;
	int64		current;
	int64		ndead = 0;
	int64		nskipped = 0;
	int64		nlive = 0;
	int64		nevicted = 0;
	int64		loaded = 0;

	if (!pssc_save)
	{
		if (unlink(PSSC_DUMP_FILE) == 0)
			ereport(LOG,
					(errmsg("pg_stat_statement_context: discarding saved statistics in \"%s\": pg_stat_statement_context.save is off",
							PSSC_DUMP_FILE)));
		return;
	}

	file = AllocateFile(PSSC_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", PSSC_DUMP_FILE)));
			unlink(PSSC_DUMP_FILE);
		}
		return;
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"pg_stat_statement_context load",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	tags = palloc(PSSC_TAGSET_BYTES_MAX);
	key = palloc(store_keysize);

	INIT_CRC32C(crc);
	if ((st = load_read(file, &hdr, sizeof(hdr), &crc)) != LOAD_OK)
		goto fail;
	if (hdr.magic != PSSC_DUMP_MAGIC)
	{
		st = LOAD_INVALID;
		goto fail;
	}
	hdr.ext_version[PSSC_DUMP_EXT_VERSION_LEN - 1] = '\0';
	if (hdr.format != PSSC_DUMP_FORMAT || hdr.pg_major != PG_MAJORVERSION_NUM ||
		strcmp(hdr.ext_version, PSSC_EXT_VERSION) != 0)
	{
		ereport(LOG,
				(errmsg("pg_stat_statement_context: discarding saved statistics in \"%s\": written by a different version",
						PSSC_DUMP_FILE),
				 errdetail("The file has format %u, PostgreSQL %u, extension version \"%s\"; expected format %d, PostgreSQL %d, extension version \"%s\".",
						   hdr.format, hdr.pg_major, hdr.ext_version,
						   PSSC_DUMP_FORMAT, PG_MAJORVERSION_NUM, PSSC_EXT_VERSION)));
		goto discard;
	}
	if (hdr.bucket_count < 1 || hdr.interval_us <= 0 || hdr.max_entries < 1 ||
		hdr.max_tagset_bytes < 0 || hdr.max_tagset_bytes > PSSC_TAGSET_BYTES_MAX ||
		hdr.nentries < 0 || hdr.nentries > hdr.max_entries ||
		!IS_VALID_TIMESTAMP(hdr.epoch) || hdr.current_bucket == PSSC_BUCKET_NONE)
	{
		st = LOAD_INVALID;
		goto fail;
	}
	if (hdr.bucket_count != store_state->bucket_count ||
		hdr.interval_us != store_state->interval_us)
	{
		ereport(LOG,
				(errmsg("pg_stat_statement_context: discarding saved statistics in \"%s\": bucket_interval or bucket_count changed",
						PSSC_DUMP_FILE),
				 errdetail("Saved with bucket_interval = " INT64_FORMAT " s and bucket_count = %d, now " INT64_FORMAT " s and %d.",
						   hdr.interval_us / USECS_PER_SEC, hdr.bucket_count,
						   store_state->interval_us / USECS_PER_SEC,
						   store_state->bucket_count)));
		goto discard;
	}
	/* the same bucket_count as the store's, so within its GUC's bounds */
	slots = palloc(sizeof(PsscSlot) * hdr.bucket_count);

	/*
	 * Pass 1: read and check everything. The array grows as records arrive,
	 * so a corrupt nentries cannot make us allocate more than the file holds.
	 */
	nalloc = Max(Min(hdr.nentries, 1024), 1);
	recs = load_alloc(sizeof(PsscLoadRecord) * nalloc);
	if (recs == NULL)
	{
		st = LOAD_NOMEM;
		goto fail;
	}
	for (int64 i = 0; i < hdr.nentries; i++)
	{
		if (i == nalloc)
		{
			PsscLoadRecord *grown;

			nalloc = Min(nalloc * 2, hdr.nentries);
			grown = load_alloc(sizeof(PsscLoadRecord) * nalloc);
			if (grown == NULL)
			{
				st = LOAD_NOMEM;
				goto fail;
			}
			memcpy(grown, recs, sizeof(PsscLoadRecord) * i);
			pfree(recs);
			recs = grown;
		}
		if ((st = load_record(file, &hdr, &recs[i].rec, tags, slots, &crc)) != LOAD_OK)
			goto fail;
	}
	FIN_CRC32C(crc);
	if ((st = load_read(file, &saved_crc, sizeof(saved_crc), NULL)) != LOAD_OK)
		goto fail;
	if (!EQ_CRC32C(crc, saved_crc) || fgetc(file) != EOF)
	{
		st = LOAD_INVALID;
		goto fail;
	}

	/* the watermark never goes back, and time has passed */
	current = Max(hdr.current_bucket,
				  pssc_bucket_for_time(GetCurrentTimestamp(), hdr.epoch, hdr.interval_us));
	for (int64 i = 0; i < hdr.nentries; i++)
	{
		PsscLoadRecord *r = &recs[i];

		if (pssc_bucket_entry_is_dead(r->rec.last_bucket, current, hdr.bucket_count))
		{
			r->fate = 'd';
			ndead++;
		}
		else if (r->rec.tags_len > store_state->max_tagset_bytes)
		{
			r->fate = 's';
			nskipped++;
		}
		else
		{
			r->fate = 'k';
			nlive++;
		}
	}
	if (nlive > store_state->max_entries)
	{
		PsscEvictCandidate *cands = load_alloc(sizeof(PsscEvictCandidate) * nlive);
		int64		c = 0;

		if (cands == NULL)
		{
			st = LOAD_NOMEM;
			goto fail;
		}

		/* the order of an eviction pass (§5.3) over the array as saved */
		for (int64 i = 0; i < hdr.nentries; i++)
		{
			if (recs[i].fate != 'k')
				continue;
			cands[c].last_bucket = recs[i].rec.last_bucket;
			cands[c].usage = recs[i].rec.usage;
			cands[c].entry = &recs[i];
			cands[c].seq = (uint64) c;
			c++;
		}
		pssc_evict_sort(cands, (size_t) nlive);
		nevicted = nlive - store_state->max_entries;
		for (int64 j = 0; j < nevicted; j++)
			((PsscLoadRecord *) cands[j].entry)->fate = 'e';
	}

	/* pass 2: insert the kept entries, in the saved order */
	if (fseeko(file, (off_t) sizeof(hdr), SEEK_SET) != 0)
	{
		st = LOAD_IO;
		goto fail;
	}
	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	for (int64 i = 0; i < hdr.nentries; i++)
	{
		PsscDumpRecord rec;

		/* the file was checked; only the postmaster could have changed it */
		st = load_record(file, &hdr, &rec, tags, slots, NULL);
		if (st == LOAD_OK && memcmp(&rec, &recs[i].rec, sizeof(rec)) != 0)
			st = LOAD_INVALID;
		if (st == LOAD_OK && recs[i].fate == 'k' &&
			!load_insert(key, &rec, tags, slots, current))
			st = LOAD_INVALID;	/* a duplicate key */
		if (st != LOAD_OK)
		{
			store_clear_entries_locked();
			LWLockRelease(store_state->lock);
			goto fail;
		}
	}
	loaded = store_state->entries;

	store_state->epoch = hdr.epoch;
	pg_atomic_write_u64(&store_state->current_bucket, (uint64) current);
	store_state->stats_reset = hdr.stats_reset;
	store_state->dealloc = hdr.dealloc + (nevicted > 0 ? 1 : 0);
	store_state->reclaimed_entries = hdr.reclaimed_entries + ndead;
	store_state->evicted_entries = hdr.evicted_entries + nevicted;
	pg_atomic_write_u64(&store_state->invalid_tags, hdr.invalid_tags);
	pg_atomic_write_u64(&store_state->dropped_tags, hdr.dropped_tags);
	pg_atomic_write_u64(&store_state->regex_compile_failures, hdr.regex_compile_failures);
	pg_atomic_write_u64(&store_state->heuristic_scans, hdr.heuristic_scans);
	pg_atomic_write_u64(&store_state->capped_tags, hdr.capped_tags);
	pg_atomic_write_u64(&store_state->cap_table_full, hdr.cap_table_full);
	pg_atomic_write_u64(&store_state->utility_missing_queryid, hdr.utility_missing_queryid);
	pg_atomic_write_u64(&store_state->dropped_records, hdr.dropped_records);
#ifdef USE_ASSERT_CHECKING
	assert_evict_slots();
#endif
	LWLockRelease(store_state->lock);

	if (nskipped > 0)
		ereport(LOG,
				(errmsg("pg_stat_statement_context: skipped " INT64_FORMAT " saved entries whose tag set exceeds max_tagset_bytes (%d)",
						nskipped, store_state->max_tagset_bytes)));
	if (nevicted > 0)
		ereport(LOG,
				(errmsg("pg_stat_statement_context: max_entries shrank from %d to %d: evicted " INT64_FORMAT " saved entries",
						hdr.max_entries, store_state->max_entries, nevicted)));
	ereport(LOG,
			(errmsg("pg_stat_statement_context: loaded " INT64_FORMAT " of " INT64_FORMAT " saved entries from \"%s\"",
					loaded, hdr.nentries, PSSC_DUMP_FILE),
			 errdetail(INT64_FORMAT " expired, " INT64_FORMAT " skipped, " INT64_FORMAT " evicted.",
					   ndead, nskipped, nevicted)));
	goto discard;

fail:
	if (st == LOAD_IO)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", PSSC_DUMP_FILE)));
	else if (st == LOAD_NOMEM)
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_stat_statement_context: discarding saved statistics in \"%s\": out of memory",
						PSSC_DUMP_FILE)));
	else
		ereport(LOG,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("ignoring invalid data in file \"%s\"", PSSC_DUMP_FILE),
				 errdetail("%s", st == LOAD_EOF ? "The file is truncated." :
						   "The file fails a consistency check.")));

discard:
	FreeFile(file);
	unlink(PSSC_DUMP_FILE);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
