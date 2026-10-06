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
 * Eviction (§5.3): a record whose key is new while the table holds
 * max_entries entries first runs an eviction pass under the exclusive lock
 * (store_evict() in store.c): every dead entry is reclaimed, every
 * surviving entry's usage decays by 0.99 (as in pgss), and if fewer than
 * max(1, max_entries * 5 / 100) entries were freed (pssc_evict_target()),
 * live entries are evicted in order of last_bucket, then usage, both
 * ascending, then scan order (pssc_evict_cmp(); chosen by partial
 * selection, pssc_evict_select_*()), until that many are. dealloc counts the
 * passes, evicted_entries every entry removed (dead or live). The new
 * entry is then inserted. Only if the pass freed nothing (the candidate
 * buffer could not be allocated and no entry was dead) is the record dropped and
 * counted in dropped_records; the statement never fails.
 *
 * Time buckets (§5.2). The header holds the epoch, bucket_interval,
 * bucket_count and current_bucket. The epoch is the postmaster's start time
 * rounded down to a multiple of bucket_interval since the PostgreSQL epoch
 * (2000-01-01 00:00 UTC), so bucket boundaries fall on wall-clock multiples
 * of the interval (e.g. :00, :05 for 300 s) and current_bucket starts at 0.
 * Every backend computes bucket_id = floor((now - epoch) / interval) as a
 * signed int64 (pssc_bucket_for_time(), counters.h; ids are negative if the
 * clock is before the epoch).
 *	- current_bucket is a shared monotonic watermark: the newest bucket
 *	  any writer or reader has observed. It is a pg_atomic_uint64 (holding
 *	  the signed id) raised lock-free by a CAS max loop and never decreases,
 *	  so a clock step backwards cannot bring expired counts back.
 *	- Writers, once they hold the lock (shared fast path, or exclusive
 *	  insert path), re-read the clock and raise current_bucket to
 *	  max(current_bucket, clock bucket, computed id); a stall before or
 *	  while waiting for the lock therefore cannot leave them behind. The id
 *	  written is current_bucket, read under the entry spinlock: an older
 *	  computed id (stalled writer, clock stepped back) is clamped up to it,
 *	  so every write lands in a bucket that is live at the moment of the
 *	  write, and every written id is <= current_bucket.
 *	- Each entry's ring rolls over lazily, under the entry spinlock: slot
 *	  bucket_id mod bucket_count is zeroed and relabeled if it holds an older
 *	  bucket, and last_bucket is set to the id written.
 *	- Readers do not depend on writers: they too raise current_bucket to
 *	  the clock bucket, then use it (read after copying each entry) as their
 *	  current bucket. A slot is live if its id is in
 *	  [current - bucket_count + 1, current] (pssc_bucket_is_live()); an
 *	  entry with no live slot is dead (pssc_bucket_entry_is_dead()). Once a
 *	  reader saw a slot expire it stays expired, whatever the clock does.
 *
 * Ring invariants (asserted on every write in assert-enabled builds, and
 * checked on demand by pssc_store_check_invariants() in any build): every
 * slot is empty (PSSC_BUCKET_NONE, zero counters) or holds an id <=
 * current_bucket that is congruent to its index mod bucket_count and has
 * calls >= 1; an entry's last_bucket is <= current_bucket and is the
 * newest id in its ring.
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
	PSSC_STORE_FULL,			/* no room even after eviction: dropped,
								 * counted in dropped_records */
	PSSC_STORE_UNAVAILABLE		/* shared memory not set up (not preloaded) */
} PsscStoreResult;

/* Shared header counters and sizes (for _info(), item -21, and tests). */
typedef struct PsscStoreCounters
{
	int64		entries;
	int64		hash_entries;	/* hash_get_num_entries(), for cross-checks */
	int64		max_entries;
	int64		dealloc;		/* eviction passes */
	int64		evicted_entries;	/* entries they removed, dead or live */
	int64		invalid_tags;
	int64		dropped_tags;
	int64		regex_compile_failures;
	int64		heuristic_scans;
	int64		capped_tags;	/* values collapsed to null by caps */
	int64		cap_table_full; /* of which: the cap table was full */
	int64		utility_missing_queryid;
	int64		dropped_records;	/* records lost: no room after eviction */
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

	/*
	 * The readers' current bucket (current_bucket, read after this entry was
	 * copied, so every id in it is <= this; non-decreasing across entries):
	 * pass it to pssc_bucket_is_live() for each slot (expired slots must be
	 * hidden) and to pssc_bucket_entry_is_dead().
	 */
	int64		current_bucket;
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

/* Whether the shared store is set up (the library was preloaded). */
extern PGDLLEXPORT bool pssc_store_available(void);

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
 * Records one call taking elapsed_ms into the entry for key. The bucket is
 * computed from the clock now (pssc_store_clock_bucket()), so an execution
 * is attributed to the bucket in which it completes (call this at
 * ExecutorEnd / utility completion); the clock is read again once the lock
 * is held, so a stall moves the call forward (see the top of this file).
 */
extern PGDLLEXPORT PsscStoreResult pssc_store_record(const PsscKey *key,
													 double elapsed_ms);

/*
 * The same, and if pending is not NULL also adds those backend-local
 * extraction counters to the header (and zeroes *pending) under the lock
 * the record already holds, so a statement's flush costs no extra lock
 * acquisition. *pending is left untouched if the store is unavailable.
 */
extern PGDLLEXPORT PsscStoreResult pssc_store_record_with_stats(const PsscKey *key,
																double elapsed_ms,
																PsscTagsetStats *pending);

/*
 * The same with a bucket id the caller already computed from the clock
 * (pssc_store_clock_bucket()). The id is only a lower bound: current_bucket
 * is raised to max(it, the clock bucket under the lock, this id) and the
 * call is written there.
 */
extern PGDLLEXPORT PsscStoreResult pssc_store_record_at(const PsscKey *key,
														int64 bucket_id,
														double elapsed_ms);

/* The clock as the store sees it (honours the debug clock, below). */
extern PGDLLEXPORT TimestampTz pssc_store_now(void);

/* floor((pssc_store_now() - epoch) / interval); 0 if not set up. */
extern PGDLLEXPORT int64 pssc_store_clock_bucket(void);

/* Start of a bucket: epoch + bucket_id * bucket_interval. */
extern PGDLLEXPORT TimestampTz pssc_store_bucket_start(int64 bucket_id);

/* Header bucket state (§5.2), for _info() (item -21) and tests. */
typedef enum PsscDebugClockMode
{
	PSSC_CLOCK_REAL = 0,		/* now = system clock + offset (0 normally) */
	PSSC_CLOCK_PINNED			/* now = a fixed (settable) timestamp */
} PsscDebugClockMode;

typedef struct PsscStoreBuckets
{
	TimestampTz epoch;
	int64		interval_us;
	int			bucket_count;
	int64		current_bucket; /* the watermark (written ids are <= this) */
	int64		clock_bucket;	/* from pssc_store_now() */
	int64		reader_bucket;	/* max(clock_bucket, current_bucket): what
								 * the next reader will raise it to */
	TimestampTz now;			/* pssc_store_now() */
	int64		advances;		/* watermark advances since startup or reset */
	PsscDebugClockMode clock_mode;
	int64		clock_value;	/* offset (us) or pinned timestamp */
} PsscStoreBuckets;

/*
 * A diagnostic snapshot; unlike readers it does not advance current_bucket.
 * false (and *b zeroed) if the store is not set up.
 */
extern PGDLLEXPORT bool pssc_store_get_buckets(PsscStoreBuckets *b);

/*
 * Checks the ring invariants (top of this file) and the entry count of
 * every entry under the shared lock, in any build. Raises ERROR describing
 * the first violation; returns the number of entries checked.
 */
extern PGDLLEXPORT int64 pssc_store_check_invariants(void);

/*
 * Calls fn for a copy of every entry (taken under its spinlock) while
 * holding the shared lock. fn must not call into the store.
 */
extern PGDLLEXPORT void pssc_store_foreach(PsscStoreVisitor fn, void *arg);

/* Removes every entry and zeroes the counters; sets stats_reset. */
extern PGDLLEXPORT void pssc_store_reset(void);

/* Header counters; false (and *c zeroed) if the store is not set up. */
extern PGDLLEXPORT bool pssc_store_get_counters(PsscStoreCounters *c);

/*
 * For _info() (DESIGN.md §7): the counters as pssc_store_get_counters(),
 * and *oldest_bucket, the oldest live slot of any entry (PSSC_BUCKET_NONE
 * if no slot is live), all under one acquisition of the shared lock, so
 * the snapshot is wholly before or wholly after any reset. Like every
 * reader it first raises current_bucket to the clock, and it judges each
 * entry's slots against the watermark read after copying them (as
 * pssc_store_foreach()). Scans the whole table. false (*c zeroed,
 * *oldest_bucket PSSC_BUCKET_NONE) if the store is not set up.
 */
extern PGDLLEXPORT bool pssc_store_get_info(PsscStoreCounters *c,
											int64 *oldest_bucket);

/*
 * Adds backend-local extraction counters to the shared header. All of them
 * are added under the store lock (shared), so pssc_store_reset() (exclusive)
 * never splits one flush; when all are zero (the common case) no lock is
 * taken. Must not be called while holding the store lock.
 */
extern PGDLLEXPORT void pssc_store_add_tagset_stats(const PsscTagsetStats *stats);

/*
 * Counts a recordable utility statement that arrived with queryId 0, and
 * adds *pending (if not NULL; then zeroed) under the same lock acquisition.
 */
extern PGDLLEXPORT void pssc_store_count_utility_missing_queryid(PsscTagsetStats *pending);

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
/* (The hook runs after the record has computed its bucket id.) */
extern PGDLLEXPORT void pssc_store_set_record_test_hook(PsscStoreRecordTestHook hook,
														void *arg);

/*
 * Testing aid: if set, every diagnostic-counter flush in this backend calls
 * hook(arg) while holding the store lock, after adding invalid_tags and
 * before the other counters, so tests can show that a concurrent reset waits
 * for the whole flush. NULL (the default) disables it.
 */
extern PGDLLEXPORT void pssc_store_set_flush_test_hook(PsscStoreRecordTestHook hook,
													   void *arg);

/*
 * Testing aid: in the next eviction pass of this backend, the candidate
 * buffer allocation returns NULL as if out of memory (the pass still
 * reclaims dead entries). A buffer kept from an earlier pass is dropped
 * first, so the real allocation path runs in both the kept and the per-pass
 * (above 64 kB) branch. One-shot; the flag is cleared by that pass whether
 * or not it needed the buffer. Reachable only from C
 * (test/modules/pssc_store_test).
 */
extern PGDLLEXPORT void pssc_store_debug_fail_next_eviction_alloc(void);

/*
 * Testing aid (DESIGN.md §9): a debug clock for clock steps and bucket
 * boundaries, shared through the header so every backend (and every reader)
 * sees the same time. PSSC_CLOCK_REAL with value = offset in microseconds
 * (at most +-1e17, about 3000 years) adds the offset to the system clock;
 * PSSC_CLOCK_PINNED with value = a finite timestamp freezes the clock
 * there. advance adds usec to the value atomically (both modes), so
 * concurrent backends can step a pinned clock. Startup is REAL with
 * offset 0. Reachable only from C (test/modules/pssc_store_test); it is
 * not a GUC.
 */
extern PGDLLEXPORT void pssc_store_debug_set_clock(PsscDebugClockMode mode,
												   int64 value);
extern PGDLLEXPORT void pssc_store_debug_advance_clock(int64 usec);

#endif							/* PSSC_STORE_H */
