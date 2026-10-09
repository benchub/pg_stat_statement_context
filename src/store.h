/*
 * store.h
 *		Shared store (DESIGN.md §3.1 item 4, §5.1, §5.4): a fixed-size shared
 *		hash table with one entry per (query x context), each holding a ring
 *		of bucket_count per-bucket counter slots (PsscSlot, counters.h).
 *
 * Sizing (all with overflow-checked add_size/mul_size, fixed at startup):
 *	keysize   = MAXALIGN(offsetof(PsscKey, tags) + max_tagset_bytes)
 *	entrysize = keysize + MAXALIGN(sizeof(PsscEntryHeader))
 *				+ bucket_count * sizeof(PsscSlot) + exemplar block
 *	shmem     = MAXALIGN(offsetof(PsscSharedState, evict_slots)
 *						 + max_entries * sizeof(PsscEvictSlot))
 *				+ hash_estimate_size(max_entries, entrysize)
 * The exemplar block (§6.13) is MAXALIGN(nkeys * (2 + value_len)) bytes
 * (pssc_store_exemplar_layout_for()); 0 when exemplar_keys is empty.
 * The shared header ends with the compact eviction array (store.c), one
 * 24-byte (last_bucket, usage, entry) slot per possible entry.
 * The table is created with init_size = max_size = max_entries; the
 * max_entries cap itself is enforced by pssc_store_record() under the
 * exclusive lock (ShmemInitHash's max_size is only an estimate).
 *
 * Locking (§5.4): one LWLock (named tranche) for the hash table. An existing
 * entry is found under the shared lock and updated under its own spinlock;
 * inserting (and resetting) takes the exclusive lock. Tag extraction and
 * key building happen before any lock is taken.
 *
 * Persistence (§5.5): with pg_stat_statement_context.save on, the
 * postmaster saves the store to pg_stat/pg_stat_statement_context.stat when
 * it exits after a clean shutdown, and loads (then unlinks) it when it
 * creates the store; see the persistence section of store.c.
 *
 * Eviction (§5.3): a record whose key is new while the table holds
 * max_entries entries first runs an eviction pass under the exclusive lock
 * (store_evict() in store.c), which scans the compact eviction array, not
 * the entries: every dead entry is reclaimed, every surviving entry's usage
 * decays by 0.99 (as in pgss), and if fewer than
 * max(1, max_entries * 5 / 100) entries were freed (pssc_evict_target()),
 * live entries are evicted in order of last_bucket, then usage, both
 * ascending, then array order (pssc_evict_cmp(); chosen by partial
 * selection, pssc_evict_select_*()), until that many are. dealloc counts the
 * passes, reclaimed_entries the dead entries they removed and
 * evicted_entries the live ones. The new entry is then inserted. Only if
 * the pass freed nothing (the candidate buffer could not be allocated and no
 * entry was dead) is the record dropped and counted in dropped_records; the
 * statement never fails. The optional reclaim worker (reclaim.c) runs the
 * same dead-entry scan on its own, without decay or live evictions
 * (pssc_store_reclaim_dead()).
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

#include "export.h"
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

/* Shared header counters and sizes (for _info() and tests). */
typedef struct PsscStoreCounters
{
	int64		entries;
	int64		hash_entries;	/* hash_get_num_entries(), for cross-checks */
	int64		max_entries;
	int64		dealloc;		/* eviction passes */
	int64		reclaimed_entries;	/* dead entries they reclaimed */
	int64		evicted_entries;	/* live entries they evicted */
	int64		invalid_tags;
	int64		dropped_tags;
	int64		regex_compile_failures;
	int64		heuristic_scans;
	int64		capped_tags;	/* values collapsed to null by caps */
	int64		cap_table_full; /* of which: the cap table was full */
	int64		utility_missing_queryid;
	int64		dropped_records;	/* records lost: no room after eviction */
	int64		exemplar_values_dropped;	/* too long for exemplar_value_len */
	TimestampTz stats_reset;
	int64		current_bucket; /* watermark (pssc_store_get_info() only) */
	int64		interval_us;	/* bucket_interval */
	Size		shmem_bytes;	/* exactly what was requested */
	Size		keysize;
	Size		entrysize;
	int			bucket_count;
	int			max_tagset_bytes;
#ifdef PSSC_TESTING
	bool		force_collisions;
#endif
	int			exemplar_nkeys; /* exemplar slots per entry (§6.13) */
	int			exemplar_value_len; /* bytes per exemplar value */
	Size		exemplar_shmem_bytes;	/* max_entries * per-entry block */
} PsscStoreCounters;

/* A consistent copy of one entry, passed to a PsscStoreVisitor. */
typedef struct PsscStoreEntryView
{
	const PsscKey *key;
	int			encoding;
	int64		last_bucket;	/* PSSC_BUCKET_NONE if never written */
	double		usage;
	int64		calls_total;	/* monotonic since stats_since (§5.1) */
	double		exec_time_total;
	TimestampTz stats_since;	/* when the entry was created */
	int			bucket_count;
	const PsscSlot *slots;		/* [bucket_count], index = bucket_id mod count */

	/*
	 * Exemplar slots (§6.13): exemplar_nkeys slots, slot i for key i of
	 * exemplar_keys; read them with pssc_store_exemplar().
	 */
	int			exemplar_nkeys;
	int			exemplar_value_len;
	const char *exemplars;

	/*
	 * The readers' current bucket (current_bucket, read after this entry was
	 * copied, so every id in it is <= this; non-decreasing across entries):
	 * pass it to pssc_bucket_is_live() for each slot (expired slots must be
	 * hidden) and to pssc_bucket_entry_is_dead().
	 */
	int64		current_bucket;

	/*
	 * current_bucket as observed once at the start of the scan, the same
	 * for every entry: scan_bucket - 1 is closed (no write can land in it
	 * any more) for the whole scan.
	 */
	int64		scan_bucket;
} PsscStoreEntryView;

typedef void (*PsscStoreVisitor) (const PsscStoreEntryView *entry, void *arg);

/*
 * Exemplar slot i (< view->exemplar_nkeys) of an entry view: false if it
 * holds no value, else the value in *val / *len (in the entry's encoding,
 * not NUL-terminated, valid as long as the view).
 */
extern PSSC_TEST_API bool pssc_store_exemplar(const PsscStoreEntryView *view,
											int i, const char **val,
											size_t *len);

/*
 * Exemplar layout (§5.1, §6.13) for nkeys exemplar keys sharing
 * memory_kb kB over max_entries entries: *value_len, the bytes each value
 * may take (0: none fits, every value is dropped), and *block, the bytes
 * each entry adds (max_entries * *block <= memory_kb kB).
 */
extern PSSC_TEST_API void pssc_store_exemplar_layout_for(int max_entries,
													   int memory_kb,
													   int nkeys,
													   int *value_len,
													   Size *block);

/* exemplar_value_len of the running store; 0 if not set up or disabled. */
extern PSSC_TEST_API int pssc_store_exemplar_value_len(void);

/* Registers the shared-memory request and startup hooks; from _PG_init. */
extern void pssc_store_init(void);

/*
 * Sizes for the given settings (see the top of this file). These raise
 * ERROR if a size overflows Size. pssc_store_shmem_size() is the value the
 * store requests at startup for the current settings.
 */
extern PSSC_TEST_API Size pssc_store_keysize_for(int max_tagset_bytes);
extern PSSC_TEST_API Size pssc_store_entrysize_for(Size keysize, int bucket_count);
extern PSSC_TEST_API Size pssc_store_shmem_size_for(int max_entries,
												  int max_tagset_bytes,
												  int bucket_count);
extern PSSC_TEST_API Size pssc_store_shmem_size(void);

/* Whether the shared store is set up (the library was preloaded). */
extern PSSC_TEST_API bool pssc_store_available(void);

/* keysize in effect (0 if the store is not set up). */
extern PSSC_TEST_API Size pssc_store_keysize(void);

/*
 * Builds a key into key, which must hold pssc_store_keysize() bytes
 * (a PsscKeyBuffer always does): zeroes all of them, then fills the fields.
 * Returns false (key unusable) if the store is not set up or tags_len
 * exceeds max_tagset_bytes.
 */
extern PSSC_TEST_API bool pssc_store_build_key(PsscKey *key, Oid dbid, Oid userid,
											 int64 queryid, bool toplevel,
											 const char *tags, size_t tags_len,
											 uint32 tags_hash);

/* Hash table hash of a key (honours the forced-collision debug flag). */
extern PSSC_TEST_API uint32 pssc_store_key_hash(const PsscKey *key);

/*
 * Records one call taking elapsed_ms into the entry for key. The bucket is
 * computed from the clock now (pssc_store_clock_bucket()), so an execution
 * is attributed to the bucket in which it completes (call this at
 * ExecutorEnd / utility completion); the clock is read again once the lock
 * is held, so a stall moves the call forward (see the top of this file).
 */
extern PSSC_TEST_API PsscStoreResult pssc_store_record(const PsscKey *key,
													 double elapsed_ms);

/*
 * The same, and if pending is not NULL also adds those backend-local
 * extraction counters to the header (and zeroes *pending) under the lock
 * the record already holds, so a statement's flush costs no extra lock
 * acquisition. *pending is left untouched if the store is unavailable.
 */
extern PSSC_TEST_API PsscStoreResult pssc_store_record_with_stats(const PsscKey *key,
																double elapsed_ms,
																PsscTagsetStats *pending);

/*
 * The same, and also stores the exemplar values ex[0, exlen) (as written by
 * pssc_extract_tags_ex(): per value, a uint8 slot, a native uint16 length
 * and the bytes) into the entry's exemplar slots, under the entry's
 * spinlock with the call itself; slots without a value keep theirs.
 */
extern PSSC_TEST_API PsscStoreResult pssc_store_record_ex(const PsscKey *key,
														double elapsed_ms,
														PsscTagsetStats *pending,
														const char *ex,
														size_t exlen);

/*
 * The same with a bucket id the caller already computed from the clock
 * (pssc_store_clock_bucket()). The id is only a lower bound: current_bucket
 * is raised to max(it, the clock bucket under the lock, this id) and the
 * call is written there.
 */
extern PSSC_TEST_API PsscStoreResult pssc_store_record_at(const PsscKey *key,
														int64 bucket_id,
														double elapsed_ms);

/* The clock as the store sees it (honours the debug clock, below). */
extern PSSC_TEST_API TimestampTz pssc_store_now(void);

/* floor((pssc_store_now() - epoch) / interval); 0 if not set up. */
extern PSSC_TEST_API int64 pssc_store_clock_bucket(void);

/* Start of a bucket: epoch + bucket_id * bucket_interval. */
extern PSSC_TEST_API TimestampTz pssc_store_bucket_start(int64 bucket_id);

#ifdef PSSC_TESTING
/* The debug clock's mode (testing build only, see the end of this file). */
typedef enum PsscDebugClockMode
{
	PSSC_CLOCK_REAL = 0,		/* now = system clock + offset (0 normally) */
	PSSC_CLOCK_PINNED			/* now = a fixed (settable) timestamp */
} PsscDebugClockMode;
#endif

/* Header bucket state (§5.2), for _info() and tests. */
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
#ifdef PSSC_TESTING
	PsscDebugClockMode clock_mode;
	int64		clock_value;	/* offset (us) or pinned timestamp */
#endif
} PsscStoreBuckets;

/*
 * A diagnostic snapshot; unlike readers it does not advance current_bucket.
 * false (and *b zeroed) if the store is not set up.
 */
extern PSSC_TEST_API bool pssc_store_get_buckets(PsscStoreBuckets *b);

/*
 * Checks the ring invariants (top of this file) and the entry count of
 * every entry under the shared lock, in any build. Raises ERROR describing
 * the first violation; returns the number of entries checked.
 */
extern PSSC_TEST_API int64 pssc_store_check_invariants(void);

/*
 * Calls fn for a copy of every entry (taken under its spinlock) while
 * holding the shared lock. fn must not call into the store.
 */
extern PSSC_TEST_API void pssc_store_foreach(PsscStoreVisitor fn, void *arg);

/*
 * The reclaim worker's pass (DESIGN.md §5.3, reclaim.c): raises
 * current_bucket to the clock, as readers do, and if it differs from
 * *last_watermark (PSSC_BUCKET_NONE: the first pass), removes every dead
 * entry under the exclusive lock (the scan of an eviction pass, without
 * usage decay or live evictions), adds them to reclaimed_entries (not to
 * dealloc or evicted_entries) and sets *last_watermark to the watermark it
 * used. Returns the number removed; 0 if the store is not set up.
 */
extern PSSC_TEST_API int64 pssc_store_reclaim_dead(int64 *last_watermark);

/* Removes every entry and zeroes the counters; sets stats_reset. */
extern PSSC_TEST_API void pssc_store_reset(void);

/* Header counters; false (and *c zeroed) if the store is not set up. */
extern PSSC_TEST_API bool pssc_store_get_counters(PsscStoreCounters *c);

/*
 * For _counters() (DESIGN.md §7): the counters as pssc_store_get_counters(),
 * and c->current_bucket, the watermark after raising it to the clock as
 * every reader does. O(1): takes the shared lock only to copy the header.
 * false (*c zeroed) if the store is not set up.
 */
extern PSSC_TEST_API bool pssc_store_get_header(PsscStoreCounters *c);

/*
 * For _info() (DESIGN.md §7): the counters as pssc_store_get_counters(),
 * and *oldest_bucket, the oldest live slot of any entry (PSSC_BUCKET_NONE
 * if no slot is live), all under one acquisition of the shared lock, so
 * the snapshot is wholly before or wholly after any reset. Like every
 * reader it first raises current_bucket to the clock; every slot is judged
 * against that watermark, returned as c->current_bucket. If the watermark
 * moved during the scan, the scan is repeated from scratch under a new
 * acquisition, at most 3 passes in all; the last pass is returned even if
 * the watermark moved during it. Any pass stops for a pending cancel or
 * termination (and a pass but the last for any interrupt), releases the
 * lock and services it, so a cancel is prompt; a cut-short pass is never
 * returned.
 * Scans the whole table. false (*c zeroed, *oldest_bucket
 * PSSC_BUCKET_NONE) if the store is not set up.
 */
extern PSSC_TEST_API bool pssc_store_get_info(PsscStoreCounters *c,
											int64 *oldest_bucket);

/*
 * Adds backend-local extraction counters to the shared header. All of them
 * are added under the store lock (shared), so pssc_store_reset() (exclusive)
 * never splits one flush; when all are zero (the common case) no lock is
 * taken. Must not be called while holding the store lock.
 */
extern PSSC_TEST_API void pssc_store_add_tagset_stats(const PsscTagsetStats *stats);

/*
 * Counts a recordable utility statement that arrived with queryId 0, and
 * adds *pending (if not NULL; then zeroed) under the same lock acquisition.
 */
extern PSSC_TEST_API void pssc_store_count_utility_missing_queryid(PsscTagsetStats *pending);

#ifdef PSSC_TESTING

/*
 * The testing aids below are compiled only into the testing build (make
 * PSSC_TESTING=1; DESIGN.md §9), for the TEST-ONLY modules.
 */

/*
 * Testing aid (DESIGN.md §9): while on, every key hashes to the same value,
 * so all entries share one dynahash chain and only the key comparison keeps
 * them apart. The flag lives in the shared header so every backend agrees;
 * it may only be changed while the table is empty (ERROR otherwise).
 * Reachable only from C (the TEST-ONLY module test/modules/pssc_store_test).
 */
extern PSSC_TEST_API void pssc_store_debug_force_collisions(bool on);

/*
 * Testing aid: if set, pssc_store_record() calls hook(arg) once it has
 * hashed the key and before it takes any lock, so tests can interleave
 * other store operations deterministically (e.g. flip forced collisions).
 * NULL (the default) disables it.
 */
typedef void (*PsscStoreRecordTestHook) (void *arg);
/* (The hook runs after the record has computed its bucket id.) */
extern PSSC_TEST_API void pssc_store_set_record_test_hook(PsscStoreRecordTestHook hook,
														void *arg);

/*
 * Testing aid: if set, every diagnostic-counter flush in this backend calls
 * hook(arg) while holding the store lock, after adding invalid_tags and
 * before the other counters, so tests can show that a concurrent reset waits
 * for the whole flush. NULL (the default) disables it.
 */
extern PSSC_TEST_API void pssc_store_set_flush_test_hook(PsscStoreRecordTestHook hook,
													   void *arg);

/*
 * Testing aid: if set, pssc_store_get_info() calls hook(arg) after judging
 * each entry's slots, under the shared store lock, so tests can move the
 * watermark in the middle of the scan. NULL (the default) disables it.
 */
extern PSSC_TEST_API void pssc_store_set_info_scan_test_hook(PsscStoreRecordTestHook hook,
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
extern PSSC_TEST_API void pssc_store_debug_fail_next_eviction_alloc(void);

/*
 * Testing aid: calls fn for every slot in use of the compact eviction array
 * (store.c), in index order, under the shared lock: its index, the key of
 * the hash entry it points to, and the slot's last_bucket and usage (read
 * under that entry's spinlock). fn must not call into the store. Returns
 * the number of slots visited. Reachable only from C
 * (test/modules/pssc_store_test).
 */
typedef void (*PsscEvictSlotVisitor) (int64 index, const PsscKey *key,
									  int64 last_bucket, double usage,
									  void *arg);
extern PSSC_TEST_API int64 pssc_store_debug_evict_slots(PsscEvictSlotVisitor fn,
													  void *arg);

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
extern PSSC_TEST_API void pssc_store_debug_set_clock(PsscDebugClockMode mode,
												   int64 value);
extern PSSC_TEST_API void pssc_store_debug_advance_clock(int64 usec);

/*
 * Testing aid: raises current_bucket to the (debug) clock as a reader does,
 * without taking the store lock (the watermark is lock-free, §5.2), so a
 * test hook running under the lock can move it. Returns the watermark.
 * Reachable only from C (test/modules/pssc_store_test).
 */
extern PSSC_TEST_API int64 pssc_store_debug_observe_clock(void);

#endif							/* PSSC_TESTING */

#endif							/* PSSC_STORE_H */
