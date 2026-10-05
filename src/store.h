/*
 * store.h
 *		Shared store (DESIGN.md §3.1 item 4, §5.1, §5.4): a fixed-size shared
 *		hash table with one entry per (query x context), each holding a ring
 *		of bucket_count per-bucket counter slots (PsscSlot, counters.h).
 *
 * Sizing (all with overflow-checked add_size/mul_size, fixed at startup):
 *	keysize   = MAXALIGN(offsetof(PsscKey, tags) + max_tagset_bytes)
 *	entrysize = keysize + MAXALIGN(sizeof(PsscEntryHeader))
 *				+ bucket_count * sizeof(PsscSlot)
 *	shmem     = MAXALIGN(sizeof(PsscSharedState))
 *				+ hash_estimate_size(max_entries, entrysize)
 * The table is created with init_size = max_size = max_entries; the
 * max_entries cap itself is enforced by pssc_store_record() under the
 * exclusive lock (ShmemInitHash's max_size is only an estimate).
 *
 * Locking (§5.4): one LWLock (named tranche) for the hash table. An existing
 * entry is found under the shared lock and updated under its own spinlock;
 * inserting (and resetting) takes the exclusive lock. Tag extraction and
 * key building happen before any lock is taken.
 *
 * Until backlog item 20261005-091225-15 (eviction) lands, a record whose
 * key is new while the table holds max_entries entries is dropped and
 * counted in the dropped_records header counter.
 */
#ifndef PSSC_STORE_H
#define PSSC_STORE_H

#include "datatype/timestamp.h"

#include "counters.h"
#include "extract.h"

/*
 * Hash key (ctxKey of DESIGN.md §5.1). Its size (keysize) is fixed at
 * startup from max_tagset_bytes; always build it with pssc_store_build_key(),
 * which zeroes all keysize bytes first so padding and unused tag bytes are
 * defined.
 */
typedef struct PsscKey
{
	Oid			dbid;
	Oid			userid;
	int64		queryid;
	bool		toplevel;
	uint16		tags_len;		/* bytes used in tags[] */
	uint32		tags_hash;		/* pssc_tagset_hash(tags, tags_len) */
	char		tags[FLEXIBLE_ARRAY_MEMBER];	/* "k\0v\0...", max_tagset_bytes */
} PsscKey;

/* Largest keysize for any max_tagset_bytes setting. */
#define PSSC_KEY_MAX_SIZE \
	MAXALIGN(offsetof(PsscKey, tags) + PSSC_TAGSET_BYTES_MAX)

/* Stack/static storage big enough (and aligned) for any key. */
typedef union PsscKeyBuffer
{
	char		data[PSSC_KEY_MAX_SIZE];
	int64		align_i;
	double		align_d;
} PsscKeyBuffer;

#define PSSC_KEY_FROM_BUFFER(b) ((PsscKey *) (b)->data)

/* Outcome of pssc_store_record(). */
typedef enum PsscStoreResult
{
	PSSC_STORE_UPDATED,			/* existing entry, shared-lock fast path */
	PSSC_STORE_INSERTED,		/* new entry, exclusive-lock slow path */
	PSSC_STORE_FOUND_LATE,		/* another backend inserted it first */
	PSSC_STORE_FULL,			/* table at max_entries: dropped, counted */
	PSSC_STORE_UNAVAILABLE		/* shared memory not set up (not preloaded) */
} PsscStoreResult;

/* Shared header counters and sizes (for _info(), item -21, and tests). */
typedef struct PsscStoreCounters
{
	int64		entries;
	int64		hash_entries;	/* hash_get_num_entries(), for cross-checks */
	int64		max_entries;
	int64		dealloc;
	int64		evicted_entries;
	int64		invalid_tags;
	int64		dropped_tags;
	int64		regex_compile_failures;
	int64		heuristic_scans;
	int64		utility_missing_queryid;
	int64		dropped_records;	/* records lost to a full table */
	TimestampTz stats_reset;
	Size		shmem_bytes;	/* exactly what was requested */
	Size		keysize;
	Size		entrysize;
	int			bucket_count;
	int			max_tagset_bytes;
	bool		force_collisions;
} PsscStoreCounters;

/* A consistent copy of one entry, passed to a PsscStoreVisitor. */
typedef struct PsscStoreEntryView
{
	const PsscKey *key;
	int			encoding;
	int64		last_bucket;	/* PSSC_BUCKET_NONE if never written */
	double		usage;
	int			bucket_count;
	const PsscSlot *slots;		/* [bucket_count], index = bucket_id mod count */
} PsscStoreEntryView;

typedef void (*PsscStoreVisitor) (const PsscStoreEntryView *entry, void *arg);

/* Registers the shared-memory request and startup hooks; from _PG_init. */
extern void pssc_store_init(void);

/*
 * Sizes for the given settings (see the top of this file). These raise
 * ERROR if a size overflows Size. pssc_store_shmem_size() is the value the
 * store requests at startup for the current settings.
 */
extern PGDLLEXPORT Size pssc_store_keysize_for(int max_tagset_bytes);
extern PGDLLEXPORT Size pssc_store_entrysize_for(Size keysize, int bucket_count);
extern PGDLLEXPORT Size pssc_store_shmem_size_for(int max_entries,
												  int max_tagset_bytes,
												  int bucket_count);
extern PGDLLEXPORT Size pssc_store_shmem_size(void);

/* keysize in effect (0 if the store is not set up). */
extern PGDLLEXPORT Size pssc_store_keysize(void);

/*
 * Builds a key into key, which must hold pssc_store_keysize() bytes
 * (a PsscKeyBuffer always does): zeroes all of them, then fills the fields.
 * Returns false (key unusable) if the store is not set up or tags_len
 * exceeds max_tagset_bytes.
 */
extern PGDLLEXPORT bool pssc_store_build_key(PsscKey *key, Oid dbid, Oid userid,
											 int64 queryid, bool toplevel,
											 const char *tags, size_t tags_len,
											 uint32 tags_hash);

/* Hash table hash of a key (honours the forced-collision debug flag). */
extern PGDLLEXPORT uint32 pssc_store_key_hash(const PsscKey *key);

/*
 * Records one call taking elapsed_ms into the entry for key, in ring slot
 * bucket_id mod bucket_count (relabeled first if it holds an older bucket).
 * bucket_id is supplied by the caller until item -14 clamps it to the
 * header's current bucket; meanwhile an id older than the entry's
 * last_bucket is raised to last_bucket, so slots never go backwards.
 */
extern PGDLLEXPORT PsscStoreResult pssc_store_record(const PsscKey *key,
													 int64 bucket_id,
													 double elapsed_ms);

/*
 * Calls fn for a copy of every entry (taken under its spinlock) while
 * holding the shared lock. fn must not call into the store.
 */
extern PGDLLEXPORT void pssc_store_foreach(PsscStoreVisitor fn, void *arg);

/* Removes every entry and zeroes the counters; sets stats_reset. */
extern PGDLLEXPORT void pssc_store_reset(void);

/* Header counters; false (and *c zeroed) if the store is not set up. */
extern PGDLLEXPORT bool pssc_store_get_counters(PsscStoreCounters *c);

/* Adds backend-local extraction counters to the shared header. */
extern PGDLLEXPORT void pssc_store_add_tagset_stats(const PsscTagsetStats *stats);

/* Counts a recordable utility statement that arrived with queryId 0. */
extern PGDLLEXPORT void pssc_store_count_utility_missing_queryid(void);

/*
 * Testing aid (DESIGN.md §9): while on, every key hashes to the same value,
 * so all entries share one dynahash chain and only the key comparison keeps
 * them apart. The flag lives in the shared header so every backend agrees;
 * it may only be changed while the table is empty (ERROR otherwise).
 * Reachable only from C (the TEST-ONLY module test/modules/pssc_store_test).
 */
extern PGDLLEXPORT void pssc_store_debug_force_collisions(bool on);

/*
 * Testing aid: if set, pssc_store_record() calls hook(arg) once it has
 * hashed the key and before it takes any lock, so tests can interleave
 * other store operations deterministically (e.g. flip forced collisions).
 * NULL (the default) disables it.
 */
typedef void (*PsscStoreRecordTestHook) (void *arg);
extern PGDLLEXPORT void pssc_store_set_record_test_hook(PsscStoreRecordTestHook hook,
														void *arg);

#endif							/* PSSC_STORE_H */
