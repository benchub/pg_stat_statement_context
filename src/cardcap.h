/*
 * cardcap.h
 *		Per-key cardinality caps (DESIGN.md §6.11 step 8; backlog item
 *		20261005-091225-32): the GUCs, and the shared table of the distinct
 *		values admitted per key that backs the pipeline's env->cap hook.
 *
 * Each allowed key may take at most its cap of distinct values, counted
 * globally (across buckets, queryids, users and databases) since the last
 * _reset() or server start; further values collapse to JSON null. The cap
 * of a key is its entry in cardinality_cap_overrides, else cardinality_cap;
 * 0 means no cap. With no cap anywhere (the default) the hook is not
 * installed and nothing is tracked.
 *
 * The table is a fixed-size, open-addressed array of 64-bit words in shared
 * memory (cardinality_cap_slots value slots, plus key slots for the per-key
 * counts), read and written only with atomics: lookups and admissions never
 * take a lock, so the hot path stays lock-free until the store write. A
 * value slot holds the table generation and a 44-bit fingerprint of
 * hash(key, value); a key slot holds the generation and a key fingerprint,
 * with a (generation, count) word that admissions reserve with CAS, so a key
 * never exceeds its cap even under concurrency. A fingerprint collision
 * (about n / 2^44 per lookup, n the values tracked) makes a new value look
 * admitted, so at worst a key may show one extra string. Slots are never
 * freed individually: an admitted value stays admitted (it never flips to
 * null) until _reset(), which bumps the generation and so empties the table
 * in O(1). Resets are serialized by an LWLock (never taken by lookups or
 * admissions); once every 2^20 - 1 resets the generation wraps around and
 * that reset clears the whole table, during which new values collapse to
 * null (as they do if resets keep interrupting an admission). When a value
 * finds no free slot within the probe bound, or its key no key slot, it
 * collapses to null (fail closed) and is counted in cap_table_full as well
 * as capped_tags.
 */
#ifndef PSSC_CARDCAP_H
#define PSSC_CARDCAP_H

#include "tagset.h"

/* GUC limits */
#define PSSC_CAP_MAX			1000000
#define PSSC_CAP_SLOTS_MIN		256
#define PSSC_CAP_SLOTS_MAX		(1 << 26)
#define PSSC_CAP_SLOTS_DEFAULT	16384
#define PSSC_CAP_MAX_OVERRIDES	1024

extern PGDLLEXPORT int pssc_cardinality_cap;
extern PGDLLEXPORT char *pssc_cardinality_cap_overrides;	/* raw text */
extern PGDLLEXPORT int pssc_cardinality_cap_slots;

/* Defines the three GUCs; called from pssc_guc_define(). */
extern void pssc_cap_define_gucs(void);

/* Requests and attaches the shared table; called from _PG_init. */
extern void pssc_cap_init(void);

/*
 * The env->cap hook for this backend's configuration, or NULL when no key
 * has a cap. With peek, the hook never admits (the debug function).
 */
extern PsscCapFn pssc_cap_hook(bool peek);

/* Empties every key's set of distinct values (_reset()). */
extern PGDLLEXPORT void pssc_cap_reset(void);

/* Exact size requested for the shared table (_info().cap_shmem_bytes). */
extern PGDLLEXPORT Size pssc_cap_shmem_bytes(void);

/*
 * TEST-ONLY (test/modules/pssc_store_test, 024_cardinality_caps.pl):
 * makes the next _reset() wrap the generation around, and installs a
 * one-shot hook that a wrapping _reset() calls in the middle of clearing
 * the table (after the value slots, before the key slots).
 */
extern PGDLLEXPORT void pssc_cap_test_near_wrap(void);
extern PGDLLEXPORT void pssc_cap_set_reset_test_hook(void (*fn) (void *),
													 void *arg);

#endif							/* PSSC_CARDCAP_H */
