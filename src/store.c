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
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
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
	double		usage;			/* pgss-style, for eviction (item -15) */
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

	/* under the lock (exclusive to change) */
	int64		entries;
	int64		dealloc;
	int64		evicted_entries;
	TimestampTz stats_reset;

	/* updated without the lock */
	pg_atomic_uint64 invalid_tags;
	pg_atomic_uint64 dropped_tags;
	pg_atomic_uint64 regex_compile_failures;
	pg_atomic_uint64 heuristic_scans;
	pg_atomic_uint64 utility_missing_queryid;
	pg_atomic_uint64 dropped_records;	/* new keys dropped: table full */
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

/* Ring index of a bucket id (also correct for negative ids). */
static inline int
slot_index(int64 bucket_id, int bucket_count)
{
	int64		i = bucket_id % bucket_count;

	return (int) (i < 0 ? i + bucket_count : i);
}

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
		state->evicted_entries = 0;
		state->stats_reset = GetCurrentTimestamp();
		pg_atomic_init_u64(&state->invalid_tags, 0);
		pg_atomic_init_u64(&state->dropped_tags, 0);
		pg_atomic_init_u64(&state->regex_compile_failures, 0);
		pg_atomic_init_u64(&state->heuristic_scans, 0);
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
	for (int i = 0; i < store_state->bucket_count; i++)
		pssc_slot_init(&slots[i]);
}

/* Adds one call to the entry's ring; caller holds the lock (any mode). */
static void
entry_accum(void *entry, int64 bucket_id, double elapsed_ms)
{
	PsscEntryHeader *hdr = entry_header(entry);
	PsscSlot   *slot;

	SpinLockAcquire(&hdr->mutex);

	/*
	 * Written ids never go backwards within an entry, so a slot never holds
	 * a newer id than the one written. Item -14 guarantees this by clamping
	 * to current_bucket; until then, raise a stale caller-supplied id.
	 */
	if (bucket_id < hdr->last_bucket)
		bucket_id = hdr->last_bucket;

	slot = &entry_slots(entry)[slot_index(bucket_id, store_state->bucket_count)];
	pssc_slot_roll(slot, bucket_id);
	pssc_slot_accum(slot, elapsed_ms);
	hdr->last_bucket = bucket_id;
	pssc_usage_exec(&hdr->usage);

	SpinLockRelease(&hdr->mutex);
}

PsscStoreResult
pssc_store_record(const PsscKey *key, int64 bucket_id, double elapsed_ms)
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

	/* Fast path: an existing entry, under the shared lock. */
	LWLockAcquire(store_state->lock, LW_SHARED);
	hash = key_hash_effective(normal);
	entry = hash_search_with_hash_value(store_htab, key, hash, HASH_FIND, NULL);
	if (entry != NULL)
	{
		entry_accum(entry, bucket_id, elapsed_ms);
		LWLockRelease(store_state->lock);
		return PSSC_STORE_UPDATED;
	}
	LWLockRelease(store_state->lock);

	/*
	 * Slow path: LWLocks cannot be upgraded, so take the exclusive lock and
	 * look again (another backend may have inserted the key meanwhile).
	 */
	LWLockAcquire(store_state->lock, LW_EXCLUSIVE);
	hash = key_hash_effective(normal);	/* the mode may have changed */
	if (store_state->entries < store_state->max_entries)
	{
		entry = hash_search_with_hash_value(store_htab, key, hash,
											HASH_ENTER_NULL, &found);
		if (entry != NULL && !found)
		{
			entry_init(entry);
			store_state->entries++;
		}
		result = found ? PSSC_STORE_FOUND_LATE : PSSC_STORE_INSERTED;
	}
	else
	{
		/* Full: only an existing key may be recorded (no eviction yet, -15). */
		entry = hash_search_with_hash_value(store_htab, key, hash, HASH_FIND, NULL);
		result = PSSC_STORE_FOUND_LATE;
	}

	if (entry == NULL)
	{
		pg_atomic_fetch_add_u64(&store_state->dropped_records, 1);
		result = PSSC_STORE_FULL;
	}
	else
		entry_accum(entry, bucket_id, elapsed_ms);

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

		chdr = entry_header(copy);
		view.key = (const PsscKey *) copy;
		view.encoding = chdr->encoding;
		view.last_bucket = chdr->last_bucket;
		view.usage = chdr->usage;
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
	store_state->evicted_entries = 0;
	pg_atomic_write_u64(&store_state->invalid_tags, 0);
	pg_atomic_write_u64(&store_state->dropped_tags, 0);
	pg_atomic_write_u64(&store_state->regex_compile_failures, 0);
	pg_atomic_write_u64(&store_state->heuristic_scans, 0);
	pg_atomic_write_u64(&store_state->utility_missing_queryid, 0);
	pg_atomic_write_u64(&store_state->dropped_records, 0);
	store_state->stats_reset = GetCurrentTimestamp();
	Assert(hash_get_num_entries(store_htab) == 0);
	LWLockRelease(store_state->lock);
}

bool
pssc_store_get_counters(PsscStoreCounters *c)
{
	memset(c, 0, sizeof(*c));
	if (store_state == NULL || store_htab == NULL)
		return false;

	LWLockAcquire(store_state->lock, LW_SHARED);
	c->entries = store_state->entries;
	c->hash_entries = (int64) hash_get_num_entries(store_htab);
	c->dealloc = store_state->dealloc;
	c->evicted_entries = store_state->evicted_entries;
	c->stats_reset = store_state->stats_reset;
	c->force_collisions = store_state->force_collisions;
	LWLockRelease(store_state->lock);

	c->max_entries = store_state->max_entries;
	c->invalid_tags = (int64) pg_atomic_read_u64(&store_state->invalid_tags);
	c->dropped_tags = (int64) pg_atomic_read_u64(&store_state->dropped_tags);
	c->regex_compile_failures =
		(int64) pg_atomic_read_u64(&store_state->regex_compile_failures);
	c->heuristic_scans = (int64) pg_atomic_read_u64(&store_state->heuristic_scans);
	c->utility_missing_queryid =
		(int64) pg_atomic_read_u64(&store_state->utility_missing_queryid);
	c->dropped_records = (int64) pg_atomic_read_u64(&store_state->dropped_records);
	c->shmem_bytes = store_state->shmem_bytes;
	c->keysize = store_state->keysize;
	c->entrysize = store_state->entrysize;
	c->bucket_count = store_state->bucket_count;
	c->max_tagset_bytes = store_state->max_tagset_bytes;
	return true;
}

void
pssc_store_add_tagset_stats(const PsscTagsetStats *stats)
{
	if (store_state == NULL)
		return;
	if (stats->invalid_tags)
		pg_atomic_fetch_add_u64(&store_state->invalid_tags, stats->invalid_tags);
	if (stats->dropped_tags)
		pg_atomic_fetch_add_u64(&store_state->dropped_tags, stats->dropped_tags);
	if (stats->heuristic_scans)
		pg_atomic_fetch_add_u64(&store_state->heuristic_scans, stats->heuristic_scans);
	if (stats->regex_compile_failures)
		pg_atomic_fetch_add_u64(&store_state->regex_compile_failures,
								stats->regex_compile_failures);
}

void
pssc_store_count_utility_missing_queryid(void)
{
	if (store_state != NULL)
		pg_atomic_fetch_add_u64(&store_state->utility_missing_queryid, 1);
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
pssc_store_set_record_test_hook(PsscStoreRecordTestHook hook, void *arg)
{
	record_test_hook = hook;
	record_test_hook_arg = arg;
}
