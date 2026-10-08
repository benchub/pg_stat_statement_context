/*
 * cardcap.c
 *		Per-key cardinality caps: GUCs and the lock-free shared table of
 *		admitted values. See cardcap.h.
 */
#include "postgres.h"

#include <limits.h>

#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"

#include "cardcap.h"
#include "compat.h"
#include "guc.h"

#define PSSC_CAP_SHMEM_NAME "pg_stat_statement_context cardinality caps"

#define IS_ASCII_SPACE(c) \
	((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r' || (c) == '\f' || (c) == '\v')

/*
 * Probe bounds: a lookup or insert inspects at most this many consecutive
 * slots. They bound the work per tag; a value (or key) that finds no free
 * slot within them is treated as "table full".
 */
#define VALUE_PROBES	64
#define KEY_PROBES		32

/*
 * Slot words: generation in the top GEN_BITS bits, a fingerprint (never 0)
 * in the low FP_BITS. 0 is an empty slot; a slot of another generation is
 * free too (the generation bump of _reset() empties the table).
 *
 * The generation is derived from seq, a 64-bit count of resets that never
 * repeats in practice: gen_of(seq) cycles through 1..GEN_MAX. When it wraps
 * back to 1, words of the earlier generation 1 may still be in the table, so
 * that _reset() clears the whole table first, with SEQ_CLEARING set in seq:
 * admissions that see it collapse their value to null without writing.
 * Resets are serialized by a lock (they are rare), so only one clears at a
 * time and the next one waits for it. An admission compares the whole seq
 * it started with before each write, so one that started before any reset
 * never writes into a later generation, even a reused one (except within
 * the few instructions between that check and its compare-and-swap, while
 * a million resets would have to run).
 */
#define GEN_BITS		20
#define FP_BITS			44
#define GEN_MAX			((1U << GEN_BITS) - 1)
#define FP_MASK			((UINT64CONST(1) << FP_BITS) - 1)
#define SEQ_CLEARING	(UINT64CONST(1) << 63)

#define PSSC_CAP_LWLOCK_TRANCHE "pg_stat_statement_context cardinality caps"

typedef struct CapShared
{
	pg_atomic_uint64 seq;		/* resets + 1, | SEQ_CLEARING while clearing */
	pg_atomic_uint64 secret;	/* hash key of scoped caps (scope_seed()) */
	LWLock	   *lock;			/* serializes pssc_cap_reset() */
	uint32		nvalues;		/* value slots */
	uint32		nkeys;			/* key slots (two words each) */

	/*
	 * nvalues value words, then per key slot an identity word (generation
	 * and key fingerprint) and a count word (generation << 32 | count).
	 */
	pg_atomic_uint64 words[FLEXIBLE_ARRAY_MEMBER];
} CapShared;

/* One per-key override, sorted by (klen, key) for binary search. */
typedef struct CapOverride
{
	int32		cap;
	uint8		klen;
	char		key[PSSC_MAX_KEY_LEN + 1];
} CapOverride;

/* The parsed overrides (the GUC's extra blob: pointer-free). */
typedef struct CapOverrides
{
	int			n;
	bool		any_cap;		/* some entry has cap > 0 */
	CapOverride e[FLEXIBLE_ARRAY_MEMBER];
} CapOverrides;

int			pssc_cardinality_cap = 0;
char	   *pssc_cardinality_cap_overrides = NULL;
int			pssc_cardinality_cap_slots = PSSC_CAP_SLOTS_DEFAULT;
int			pssc_cardinality_cap_scope = PSSC_CAP_SCOPE_ROLE;

static const struct config_enum_entry cap_scope_options[] = {
	{"server", PSSC_CAP_SCOPE_SERVER, false},
	{"database", PSSC_CAP_SCOPE_DATABASE, false},
	{"role", PSSC_CAP_SCOPE_ROLE, false},
	{NULL, 0, false}
};

static const CapOverrides *cur_overrides = NULL;
static CapShared *cap_shared = NULL;

/* The role and database of the current hook's scope. */
static Oid	cap_scope_userid = InvalidOid;
static Oid	cap_scope_dbid = InvalidOid;

static pssc_shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ---------------- overrides GUC ---------------- */

static int
override_cmp(const char *a, size_t alen, const char *b, size_t blen)
{
	if (alen != blen)
		return alen < blen ? -1 : 1;
	return memcmp(a, b, alen);
}

static int
override_qsort_cmp(const void *x, const void *y)
{
	const CapOverride *a = x;
	const CapOverride *b = y;

	return override_cmp(a->key, a->klen, b->key, b->klen);
}

/*
 * Validates "key:N, ..." (whitespace around entries, keys and numbers is
 * ignored; a key is everything before the entry's last ':', and follows the
 * rules of tags keys) into out (NULL: count only). Returns the number of
 * entries, or -1 after setting the GUC error detail.
 */
static int
overrides_pass(const char *value, CapOverride *out)
{
	const char *p = value;
	int			n = 0;
	bool		blank = true;

	for (const char *c = value; *c; c++)
		if (!IS_ASCII_SPACE(*c))
			blank = false;
	if (blank)
		return 0;

	for (;;)
	{
		const char *start = p;
		const char *end;
		const char *colon = NULL;
		const char *ks,
				   *ke,
				   *ns,
				   *ne;
		int64		cap = 0;

		while (*p && *p != ',')
		{
			if (*p == ':')
				colon = p;
			p++;
		}
		end = p;
		while (start < end && IS_ASCII_SPACE(*start))
			start++;
		while (end > start && IS_ASCII_SPACE(end[-1]))
			end--;
		if (start == end)
		{
			GUC_check_errdetail("The list contains an empty entry.");
			return -1;
		}
		if (colon == NULL || colon < start || colon >= end)
		{
			GUC_check_errdetail("Entry \"%.*s\" is not of the form key:N.",
								(int) Min(end - start, 200), start);
			return -1;
		}
		ks = start;
		ke = colon;
		while (ke > ks && IS_ASCII_SPACE(ke[-1]))
			ke--;
		ns = colon + 1;
		while (ns < end && IS_ASCII_SPACE(*ns))
			ns++;
		ne = end;
		if (ks == ke)
		{
			GUC_check_errdetail("Entry \"%.*s\" has an empty key.",
								(int) Min(end - start, 200), start);
			return -1;
		}
		if (ke - ks > PSSC_MAX_KEY_LEN)
		{
			int			shown = pg_mbcliplen(ks,
											 (int) Min(ke - ks, PSSC_MAX_KEY_LEN + MAX_MULTIBYTE_CHAR_LEN),
											 PSSC_MAX_KEY_LEN);

			GUC_check_errdetail("Tag key \"%.*s...\" is longer than %d bytes.",
								shown, ks, PSSC_MAX_KEY_LEN);
			return -1;
		}
		for (const char *c = ks; c < ke; c++)
		{
			if (IS_ASCII_SPACE(*c) || *c == '*')
			{
				GUC_check_errdetail("Tag key \"%.*s\" contains %s.",
									(int) (ke - ks), ks,
									*c == '*' ? "\"*\"" : "whitespace");
				return -1;
			}
		}
		if (ns == ne)
		{
			GUC_check_errdetail("Entry \"%.*s\" has no cap after \":\".",
								(int) Min(end - start, 200), start);
			return -1;
		}
		for (const char *c = ns; c < ne; c++)
		{
			if (*c < '0' || *c > '9')
			{
				GUC_check_errdetail("The cap of key \"%.*s\" is not a non-negative integer.",
									(int) (ke - ks), ks);
				return -1;
			}
			cap = cap * 10 + (*c - '0');
			if (cap > PSSC_CAP_MAX)
			{
				GUC_check_errdetail("The cap of key \"%.*s\" is larger than %d.",
									(int) (ke - ks), ks, PSSC_CAP_MAX);
				return -1;
			}
		}
		if (n >= PSSC_CAP_MAX_OVERRIDES)
		{
			GUC_check_errdetail("The list has more than %d entries.",
								PSSC_CAP_MAX_OVERRIDES);
			return -1;
		}
		for (int i = 0; out != NULL && i < n; i++)
		{
			if (override_cmp(out[i].key, out[i].klen, ks, ke - ks) == 0)
			{
				GUC_check_errdetail("Key \"%.*s\" is listed more than once.",
									(int) (ke - ks), ks);
				return -1;
			}
		}
		if (out != NULL)
		{
			out[n].cap = (int32) cap;
			out[n].klen = (uint8) (ke - ks);
			memset(out[n].key, 0, sizeof(out[n].key));
			memcpy(out[n].key, ks, ke - ks);
		}
		n++;
		if (*p != ',')
			break;
		p++;
	}
	return n;
}

static bool
check_overrides(char **newval, void **extra, GucSource source)
{
	const char *value = *newval ? *newval : "";
	int			n = overrides_pass(value, NULL);
	CapOverride *tmp;
	CapOverrides *ov;
	Size		size;

	if (n < 0)
		return false;
	/* second pass into a scratch array (the duplicate check needs it) */
	tmp = palloc(sizeof(CapOverride) * Max(n, 1));
	if (overrides_pass(value, tmp) != n)
	{
		pfree(tmp);
		return false;
	}
	size = offsetof(CapOverrides, e) + sizeof(CapOverride) * n;
	ov = pssc_guc_extra_alloc(size);
	if (ov == NULL)
	{
		pfree(tmp);
		GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
		GUC_check_errdetail("Out of memory.");
		return false;
	}
	memset(ov, 0, size);
	ov->n = n;
	if (n > 0)
		memcpy(ov->e, tmp, sizeof(CapOverride) * n);
	pfree(tmp);
	qsort(ov->e, n, sizeof(CapOverride), override_qsort_cmp);
	for (int i = 0; i < n; i++)
		if (ov->e[i].cap > 0)
			ov->any_cap = true;
	*extra = ov;
	return true;
}

static void
assign_overrides(const char *newval, void *extra)
{
	cur_overrides = extra;
}

/* The cap of key: its override, else the default (0: none). */
static int
key_cap(const char *key, size_t klen)
{
	const CapOverrides *ov = cur_overrides;

	if (ov != NULL && ov->n > 0)
	{
		int			lo = 0,
					hi = ov->n - 1;

		while (lo <= hi)
		{
			int			mid = lo + (hi - lo) / 2;
			int			c = override_cmp(ov->e[mid].key, ov->e[mid].klen, key, klen);

			if (c == 0)
				return ov->e[mid].cap;
			if (c < 0)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
	}
	return pssc_cardinality_cap;
}

void
pssc_cap_define_gucs(void)
{
	DefineCustomIntVariable(PSSC_GUC_PREFIX ".cardinality_cap",
							"Sets the maximum number of distinct values of each tag key.",
							"Values beyond a key's cap are recorded as JSON null. "
							"0 means no cap. cardinality_cap_overrides sets per-key caps.",
							&pssc_cardinality_cap,
							0,
							0, PSSC_CAP_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomStringVariable(PSSC_GUC_PREFIX ".cardinality_cap_overrides",
							   "Sets per-key caps on the number of distinct tag values.",
							   "Comma-separated key:N entries; they take precedence "
							   "over cardinality_cap. N = 0 means no cap for that key.",
							   &pssc_cardinality_cap_overrides,
							   "",
							   PGC_SIGHUP,
							   0,
							   check_overrides,
							   assign_overrides,
							   NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".cardinality_cap_slots",
							"Sets the number of distinct (key, value) pairs the cardinality caps can track.",
							"Values beyond it are recorded as JSON null until a reset.",
							&pssc_cardinality_cap_slots,
							PSSC_CAP_SLOTS_DEFAULT,
							PSSC_CAP_SLOTS_MIN, PSSC_CAP_SLOTS_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable(PSSC_GUC_PREFIX ".cardinality_cap_scope",
							 "Selects what the cardinality caps count distinct values per.",
							 "role: per role and database (as pg_stat_statements entries); "
							 "database: per database; server: across the whole server.",
							 &pssc_cardinality_cap_scope,
							 PSSC_CAP_SCOPE_ROLE,
							 cap_scope_options,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);
}

/* ---------------- shared table ---------------- */

static uint32
nkeys_for(int nvalues)
{
	return (uint32) Max(64, nvalues / 16);
}

static Size
cap_shmem_size(int nvalues)
{
	return add_size(offsetof(CapShared, words),
					mul_size((Size) nvalues + 2 * (Size) nkeys_for(nvalues),
							 sizeof(pg_atomic_uint64)));
}

static void
cap_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(cap_shmem_size(pssc_cardinality_cap_slots));
	RequestNamedLWLockTranche(PSSC_CAP_LWLOCK_TRANCHE, 1);
}

/*
 * A fresh random hash key for scoped caps (scope_seed()), drawn at startup
 * and by every _reset().
 */
static uint64
new_secret(void)
{
	uint64		secret;

	if (!pg_strong_random(&secret, sizeof(secret)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate a random key for the cardinality caps")));
	return secret;
}

static void
cap_shmem_startup(void)
{
	bool		found;
	CapShared  *sh;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	cap_shared = NULL;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	sh = ShmemInitStruct(PSSC_CAP_SHMEM_NAME,
						 cap_shmem_size(pssc_cardinality_cap_slots), &found);
	if (!found)
	{
		Size		nwords;

		sh->nvalues = (uint32) pssc_cardinality_cap_slots;
		sh->nkeys = nkeys_for(pssc_cardinality_cap_slots);
		pg_atomic_init_u64(&sh->seq, 1);
		pg_atomic_init_u64(&sh->secret, new_secret());
		sh->lock = &(GetNamedLWLockTranche(PSSC_CAP_LWLOCK_TRANCHE))->lock;
		nwords = (Size) sh->nvalues + 2 * (Size) sh->nkeys;
		for (Size i = 0; i < nwords; i++)
			pg_atomic_init_u64(&sh->words[i], 0);
	}
	cap_shared = sh;
	LWLockRelease(AddinShmemInitLock);
}

void
pssc_cap_init(void)
{
	PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev_shmem_request_hook, cap_shmem_request);
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = cap_shmem_startup;
}

#ifdef PSSC_TESTING
static void (*reset_test_hook) (void *) = NULL;
static void *reset_test_hook_arg = NULL;

void
pssc_cap_set_reset_test_hook(void (*fn) (void *), void *arg)
{
	reset_test_hook = fn;
	reset_test_hook_arg = arg;
}
#endif

static inline uint32
gen_of(uint64 seq)
{
	return (uint32) ((seq - 1) % GEN_MAX) + 1;
}

#ifdef PSSC_TESTING
void
pssc_cap_test_near_wrap(void)
{
	CapShared  *sh = cap_shared;
	uint64		seq;

	if (sh == NULL)
		return;
	LWLockAcquire(sh->lock, LW_EXCLUSIVE);
	seq = pg_atomic_read_u64(&sh->seq);
	/* the next seq whose generation is GEN_MAX (skipped ones are unused) */
	pg_atomic_write_u64(&sh->seq, ((seq - 1) / GEN_MAX + 1) * GEN_MAX);
	LWLockRelease(sh->lock);
}
#endif

Size
pssc_cap_shmem_bytes(void)
{
	return cap_shmem_size(pssc_cardinality_cap_slots);
}

void
pssc_cap_reset(void)
{
	CapShared  *sh = cap_shared;
	uint64		next;
	uint64		secret;

	if (sh == NULL)
		return;
	secret = new_secret();
	LWLockAcquire(sh->lock, LW_EXCLUSIVE);
	next = pg_atomic_read_u64(&sh->seq) + 1;
	if (gen_of(next) == 1)
	{
		/*
		 * The generation wraps around (once every GEN_MAX resets): words of
		 * the earlier generation 1 may still be there, so clear the table.
		 * Meanwhile admissions see SEQ_CLEARING and write nothing; those that
		 * started earlier see seq change before any write.
		 */
		Size		nwords = (Size) sh->nvalues + 2 * (Size) sh->nkeys;

		pg_atomic_write_u64(&sh->seq, next | SEQ_CLEARING);
		pg_memory_barrier();
		for (Size i = 0; i < sh->nvalues; i++)
			pg_atomic_write_u64(&sh->words[i], 0);
#ifdef PSSC_TESTING
		if (reset_test_hook)
		{
			void		(*fn) (void *) = reset_test_hook;

			reset_test_hook = NULL;
			fn(reset_test_hook_arg);
		}
#endif
		for (Size i = sh->nvalues; i < nwords; i++)
			pg_atomic_write_u64(&sh->words[i], 0);
		pg_memory_barrier();
	}

	/*
	 * Rekey before publishing the new generation (pairs with cap_check()'s
	 * barrier after reading seq): an admission that sees the new generation
	 * hashes with the new key. One that still sees the old generation may
	 * hash with either key, but it finds seq changed before any write.
	 */
	pg_atomic_write_u64(&sh->secret, secret);
	pg_write_barrier();
	pg_atomic_write_u64(&sh->seq, next);
	LWLockRelease(sh->lock);
}

static inline uint64
slot_word(uint32 gen, uint64 hash)
{
	uint64		fp = (hash >> GEN_BITS) & FP_MASK;

	return ((uint64) gen << FP_BITS) | (fp != 0 ? fp : 1);
}

static inline bool
slot_free(uint64 w, uint32 gen)
{
	return w == 0 || (uint32) (w >> FP_BITS) != gen;
}

static inline uint32
count_of(uint64 c, uint32 gen)
{
	return (uint32) (c >> 32) == gen ? (uint32) c : 0;
}

/*
 * True if a _reset() ran (or is clearing the table) since an admission read
 * seq. Such an admission must not write: a word it sees as free may be in
 * use in the new generation, and its own words would be stale anyway. It is
 * then restarted in the new generation. The barrier orders the admission's
 * earlier reads of the table before this read of seq, so a word it read as
 * cleared by a wrapping reset implies it sees that reset here.
 */
static inline bool
gen_stale(CapShared *sh, uint64 seq)
{
	pg_read_barrier();
	return pg_atomic_read_u64(&sh->seq) != seq;
}

/* Gives back a reservation of seq's generation (unless reset since). */
static void
count_release(CapShared *sh, pg_atomic_uint64 *cnt, uint64 seq)
{
	uint32		gen = gen_of(seq);
	uint64		c = pg_atomic_read_u64(cnt);

	while (count_of(c, gen) > 0)
	{
		if (gen_stale(sh, seq))
			break;
		if (pg_atomic_compare_exchange_u64(cnt, &c, c - 1))
			break;
	}
}

/* Is the value (want, probe start vh) admitted, from probe i on? */
static bool
value_present(CapShared *sh, uint32 gen, uint64 vh, uint64 want,
			  uint32 i, uint32 probes)
{
	for (; i < probes; i++)
	{
		uint64		w = pg_atomic_read_u64(&sh->words[(vh + i) % sh->nvalues]);

		if (w == want)
			return true;
		if (slot_free(w, gen))
			return false;
	}
	return false;
}

typedef enum
{
	KEY_FOUND,					/* *cnt is the key's count word */
	KEY_ABSENT,					/* not claimed (peek): count 0 */
	KEY_FULL,					/* no free key slot within the probes */
	KEY_STALE					/* generation changed: restart */
} KeyLookup;

/* Finds key's count word, claiming a key slot for it when claim. */
static KeyLookup
key_count_word(CapShared *sh, uint64 seq, uint64 kh, bool claim,
			   pg_atomic_uint64 **cnt)
{
	uint32		gen = gen_of(seq);
	uint64		want = slot_word(gen, kh);
	pg_atomic_uint64 *keys = &sh->words[sh->nvalues];
	uint32		probes = Min((uint32) KEY_PROBES, sh->nkeys);

	for (uint32 j = 0; j < probes; j++)
	{
		uint32		idx = (uint32) ((kh + j) % sh->nkeys);
		pg_atomic_uint64 *ident = &keys[2 * (Size) idx];
		uint64		w = pg_atomic_read_u64(ident);

		for (;;)
		{
			if (w == want)
			{
				*cnt = ident + 1;
				return KEY_FOUND;
			}
			if (!slot_free(w, gen))
				break;			/* another key's: keep probing */
			if (!claim)
				return KEY_ABSENT;
			if (gen_stale(sh, seq))
				return KEY_STALE;
			if (pg_atomic_compare_exchange_u64(ident, &w, want))
			{
				*cnt = ident + 1;
				return KEY_FOUND;
			}
			/* w now holds what took the slot: look at it again */
		}
	}
	return KEY_FULL;
}

/*
 * One attempt of cap_check() at reset sequence seq (not clearing);
 * KEY_STALE-like restarts are reported through *stale.
 */
static PsscCapResult
cap_check_gen(CapShared *sh, uint64 seq, int cap, uint64 kh, uint64 vh,
			  bool admit, bool *stale)
{
	uint32		gen = gen_of(seq);
	uint64		want = slot_word(gen, vh);
	uint32		probes = Min((uint32) VALUE_PROBES, sh->nvalues);
	uint32		free_at = UINT32_MAX;
	pg_atomic_uint64 *cnt = NULL;
	uint64		c;

	*stale = false;

	/* Admitted already? Values are never removed, so stop at a free slot. */
	for (uint32 i = 0; i < probes; i++)
	{
		uint64		w = pg_atomic_read_u64(&sh->words[(vh + i) % sh->nvalues]);

		if (w == want)
			return PSSC_CAP_KEEP;
		if (slot_free(w, gen))
		{
			free_at = i;
			break;
		}
	}

	switch (key_count_word(sh, seq, kh, admit, &cnt))
	{
		case KEY_FOUND:
			break;
		case KEY_ABSENT:
			/* peek, key never seen: count 0 */
			return free_at == UINT32_MAX ? PSSC_CAP_NULL_FULL : PSSC_CAP_KEEP;
		case KEY_FULL:
			return PSSC_CAP_NULL_FULL;
		case KEY_STALE:
			*stale = true;
			return PSSC_CAP_KEEP;
	}
	c = pg_atomic_read_u64(cnt);

	/*
	 * At the cap: unless another backend has just admitted this very value
	 * (between the lookup above and the count read), it collapses.
	 */
	if (count_of(c, gen) >= (uint32) cap)
	{
		if (free_at != UINT32_MAX &&
			value_present(sh, gen, vh, want, free_at, probes))
			return PSSC_CAP_KEEP;
		return PSSC_CAP_NULL;
	}
	if (free_at == UINT32_MAX)
		return PSSC_CAP_NULL_FULL;
	if (!admit)
		return PSSC_CAP_KEEP;

	/* Reserve one of the key's values, then publish the value. */
	for (;;)
	{
		uint32		n = count_of(c, gen);

		if (n >= (uint32) cap)
			return value_present(sh, gen, vh, want, free_at, probes) ?
				PSSC_CAP_KEEP : PSSC_CAP_NULL;
		/*
		 * Never write into a later generation (a count it may already be
		 * using; after a wrap even one with this very word)
		 */
		if (gen_stale(sh, seq))
		{
			*stale = true;
			return PSSC_CAP_KEEP;
		}
		if (pg_atomic_compare_exchange_u64(cnt, &c, ((uint64) gen << 32) | (n + 1)))
			break;
	}
	for (uint32 i = free_at; i < probes; i++)
	{
		pg_atomic_uint64 *slot = &sh->words[(vh + i) % sh->nvalues];
		uint64		w = pg_atomic_read_u64(slot);

		for (;;)
		{
			if (w == want)
			{
				/* a concurrent admission of the same value won */
				count_release(sh, cnt, seq);
				return PSSC_CAP_KEEP;
			}
			if (!slot_free(w, gen))
				break;
			if (gen_stale(sh, seq))
			{
				count_release(sh, cnt, seq);
				*stale = true;
				return PSSC_CAP_KEEP;
			}
			if (pg_atomic_compare_exchange_u64(slot, &w, want))
				return PSSC_CAP_KEEP;
			/* w now holds what took the slot: look at it again */
		}
	}
	count_release(sh, cnt, seq);
	return PSSC_CAP_NULL_FULL;
}

/*
 * The key-hash seed of the scope of (userid, dbid): 0 under the server scope
 * (the unkeyed hash of a server-wide cap), else a nonzero hash of the
 * scope's ids keyed with the table's secret. The value hash is seeded with
 * the key hash, so a key's count, its admitted values and their slots and
 * fingerprints are all per scope, and the slots of a scoped value can't be
 * computed from its (public) role and database OIDs: otherwise a role could
 * fill the probe window after another scope's candidate value with values
 * of its own, and tell from one more value whether that slot is taken.
 */
static uint64
scope_seed(CapShared *sh, Oid userid, Oid dbid)
{
	Oid			ids[2];
	uint64		seed;

	switch (pssc_cardinality_cap_scope)
	{
		case PSSC_CAP_SCOPE_SERVER:
			return 0;
		case PSSC_CAP_SCOPE_DATABASE:
			ids[0] = InvalidOid;
			break;
		default:
			ids[0] = userid;
			break;
	}
	ids[1] = dbid;
	seed = hash_bytes_extended((const unsigned char *) ids, sizeof(ids),
							   pg_atomic_read_u64(&sh->secret));
	return seed != 0 ? seed : 1;
}

static void
cap_hashes(CapShared *sh, const char *key, size_t klen, const char *val,
		   size_t vlen, Oid userid, Oid dbid, uint64 *kh, uint64 *vh)
{
	*kh = hash_bytes_extended((const unsigned char *) key, (int) klen,
							  scope_seed(sh, userid, dbid));
	*vh = hash_bytes_extended((const unsigned char *) val, (int) vlen, *kh);
}

#ifdef PSSC_TESTING
int32
pssc_cap_test_slot(const char *key, size_t klen, const char *val,
				   size_t vlen, Oid userid, Oid dbid)
{
	CapShared  *sh = cap_shared;
	uint64		kh,
				vh;

	if (sh == NULL)
		return -1;
	cap_hashes(sh, key, klen, val, vlen, userid, dbid, &kh, &vh);
	return (int32) (vh % sh->nvalues);
}
#endif

/* Bound on restarts after concurrent _reset()s (each one is a reset). */
#define STALE_RETRIES	4

static PsscCapResult
cap_check(const char *key, size_t klen, const char *val, size_t vlen,
		  bool admit)
{
	CapShared  *sh = cap_shared;
	int			cap;
	uint64		kh,
				vh;

	if (sh == NULL)
		return PSSC_CAP_KEEP;
	cap = key_cap(key, klen);
	if (cap <= 0)
		return PSSC_CAP_KEEP;

	for (int attempt = 0; attempt < STALE_RETRIES; attempt++)
	{
		bool		stale;
		uint64		seq = pg_atomic_read_u64(&sh->seq);
		PsscCapResult r;

		/* a _reset() is clearing the table: nothing can be admitted */
		if (seq & SEQ_CLEARING)
			return PSSC_CAP_NULL;
		/*
		 * Read the table only after seq (pairs with the reset's barrier before
		 * publishing it): otherwise a weakly ordered CPU could combine the new
		 * seq with a slot's pre-clear contents.
		 */
		pg_read_barrier();
		cap_hashes(sh, key, klen, val, vlen, cap_scope_userid, cap_scope_dbid,
				   &kh, &vh);
		r = cap_check_gen(sh, seq, cap, kh, vh, admit, &stale);
		if (!stale)
			return r;
	}
	/* resets keep racing with us: fail closed */
	return PSSC_CAP_NULL;
}

static PsscCapResult
cap_admit_fn(void *arg, const char *key, size_t klen, const char *val,
			 size_t vlen, bool admit)
{
	return cap_check(key, klen, val, vlen, admit);
}

static PsscCapResult
cap_peek_fn(void *arg, const char *key, size_t klen, const char *val,
			size_t vlen, bool admit)
{
	return cap_check(key, klen, val, vlen, false);
}

PsscCapFn
pssc_cap_hook(bool peek, Oid userid, Oid dbid)
{
	if (cap_shared == NULL)
		return NULL;
	if (pssc_cardinality_cap <= 0 &&
		(cur_overrides == NULL || !cur_overrides->any_cap))
		return NULL;
	cap_scope_userid = userid;
	cap_scope_dbid = dbid;
	return peek ? cap_peek_fn : cap_admit_fn;
}
