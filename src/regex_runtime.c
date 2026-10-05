/*
 * regex_runtime.c
 *		Regex extractor runtime: see regex_runtime.h.
 *
 * Memory. Each backend compiles a regex extractor's pattern the first time
 * the pipeline needs it after a config generation change
 * (pssc_guc_config_generation()), with the core engine (REG_ADVANCED, C
 * collation), into its own child of a long-lived context. The compiled
 * regexes of an older generation are released at the next pipeline run
 * (pssc_regex_release_stale(), called by pssc_extract_tags()) or hook call:
 * pg_regfree() first, then the child context is deleted. Both are needed:
 * PG14/15's engine allocates with malloc, so only pg_regfree() frees it,
 * while PG16+'s uses palloc in the current context (the child), where
 * pg_regfree() pfrees and deleting the child reclaims anything left over.
 *
 * Errors. The hook must not fail the user's statement, but must still honor
 * query cancel and other interrupts. Compilation (including creating the
 * pattern's memory context) and matching run in PG_TRY, and matching
 * allocates only with MCXT_ALLOC_NO_OOM: errors
 * whose SQLSTATE stands for an interrupt or a condition that must abort the
 * transaction (QUERY_CANCELED: cancel and statement/lock timeouts; shutdown
 * and similar) are re-thrown; any other ERROR (e.g. out of memory) is
 * swallowed with FlushErrorState() after restoring the interrupt holdoff
 * counts that errfinish() zeroed. No subtransaction is needed: the engine
 * holds no locks, buffers or other resources, only memory in our contexts.
 * A non-OK return code (PG14/15 report a pending interrupt as REG_CANCEL,
 * PG16+ report OOM as REG_ESPACE) is followed by CHECK_FOR_INTERRUPTS(), so
 * a pending cancel is raised as the usual ERROR. A failed compile that is
 * not an interrupt disables the extractor for this backend until the next
 * generation change and counts in PsscTagsetStats.regex_compile_failures;
 * an interrupted compile is retried next time. A failed match yields no
 * (further) pairs for that comment and is not counted.
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "regex/regex.h"
#include "utils/memutils.h"

#include "compat.h"
#include "extract.h"
#include "guc.h"
#include "regex_runtime.h"

typedef enum SlotState
{
	SLOT_EMPTY,					/* not compiled (yet) for this generation */
	SLOT_READY,					/* re is a compiled regex in cxt */
	SLOT_FAILED					/* compile failed: disabled until next gen */
} SlotState;

typedef struct Slot
{
	SlotState	state;
	MemoryContext cxt;			/* owns re on PG16+; NULL unless READY */
	regex_t		re;
} Slot;

PsscRegexTestHook pssc_regex_test_hook = NULL;

static Slot slots[PSSC_MAX_EXTRACTORS];
static uint64 slots_generation;
static bool slots_valid = false;
static MemoryContext regex_cxt = NULL;	/* parent of the per-regex contexts */
static MemoryContext exec_cxt = NULL;	/* per-call scratch, reset after use */
static PsscRegexDebugStats debug_stats;

static void
release_slot(Slot *slot)
{
	if (slot->state == SLOT_READY)
	{
		pssc_regfree(&slot->re);
		debug_stats.frees++;
		debug_stats.live--;
	}
	else if (slot->state == SLOT_FAILED)
		debug_stats.failed--;
	if (slot->cxt != NULL)
		MemoryContextDelete(slot->cxt);
	slot->cxt = NULL;
	slot->state = SLOT_EMPTY;
}

/*
 * Releases the compiled regexes of an older config generation. Never
 * throws.
 */
void
pssc_regex_release_stale(void)
{
	uint64		gen = pssc_guc_config_generation();
	int			i;

	if (slots_valid && slots_generation == gen)
		return;
	for (i = 0; i < PSSC_MAX_EXTRACTORS; i++)
		release_slot(&slots[i]);
	slots_generation = gen;
	slots_valid = true;
}

/*
 * True if the error being handled must propagate: interrupts (cancel,
 * statement/lock/idle timeouts, termination) and conditions the
 * transaction must not survive.
 */
static bool
must_rethrow(int code)
{
	switch (code)
	{
		case ERRCODE_QUERY_CANCELED:
		case ERRCODE_LOCK_NOT_AVAILABLE:
		case ERRCODE_T_R_SERIALIZATION_FAILURE:
		case ERRCODE_T_R_DEADLOCK_DETECTED:
		case ERRCODE_ADMIN_SHUTDOWN:
		case ERRCODE_CRASH_SHUTDOWN:
		case ERRCODE_CANNOT_CONNECT_NOW:
		case ERRCODE_DATABASE_DROPPED:
		case ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT:
		case ERRCODE_IDLE_SESSION_TIMEOUT:
			return true;
		default:
			return false;
	}
}

/*
 * After a non-OK engine return code: raise a pending interrupt (PG14/15
 * return REG_CANCEL for it). Returns true if one is still pending (held
 * off), so the caller should not treat the failure as the pattern's fault.
 */
static bool
interrupt_pending(void)
{
	CHECK_FOR_INTERRUPTS();
	return INTERRUPTS_PENDING_CONDITION();
}

/*
 * Compiles extractor index's pattern into slot. On return the slot is READY
 * or FAILED, or still EMPTY if an interrupt is pending. Interrupts
 * propagate as ERROR (the slot is then EMPTY and its context gone).
 */
static void
compile_slot(Slot *slot, int index, const PsscExtractor *e,
			 const PsscExtractorList *list)
{
	const char *pat = pssc_blob_str(list, e->pattern);
	int			patlen = (int) e->pattern.len;
	MemoryContext oldcxt = CurrentMemoryContext;
	uint32		save_holdoff = InterruptHoldoffCount;
	uint32		save_cancel_holdoff = QueryCancelHoldoffCount;
	volatile int rc = REG_OKAY;
	pg_wchar   *wpat;
	int			wlen;

	Assert(slot->state == SLOT_EMPTY && slot->cxt == NULL);

	/*
	 * The check hook validated the pattern, but possibly in another encoding
	 * (e.g. postgresql.conf is checked by the postmaster).
	 */
	if (e->pattern.len > PSSC_MAX_REGEX_PATTERN_LEN ||
		!pg_verify_mbstr(GetDatabaseEncoding(), pat, patlen, true))
		goto failed;

	/*
	 * Everything that can throw, including creating the pattern's context,
	 * runs inside PG_TRY: e.g. out of memory here disables the extractor
	 * instead of failing the statement.
	 */
	PG_TRY();
	{
		if (pssc_regex_test_hook != NULL)
			rc = pssc_regex_test_hook(PSSC_REGEX_TEST_CONTEXT, index);
		if (rc == REG_OKAY)
		{
			slot->cxt = AllocSetContextCreate(regex_cxt,
											  "pg_stat_statement_context regex pattern",
											  ALLOCSET_SMALL_SIZES);
			wpat = MemoryContextAlloc(exec_cxt, sizeof(pg_wchar) * (patlen + 1));
			wlen = pg_mb2wchar_with_len(pat, wpat, patlen);
			if (pssc_regex_test_hook != NULL)
				rc = pssc_regex_test_hook(PSSC_REGEX_TEST_COMPILE, index);
			if (rc == REG_OKAY)
				rc = pssc_regcomp(slot->cxt, &slot->re, wpat, wlen,
								  REG_ADVANCED, C_COLLATION_OID);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		if (slot->cxt != NULL)
			MemoryContextDelete(slot->cxt);
		slot->cxt = NULL;
		MemoryContextReset(exec_cxt);
		if (must_rethrow(geterrcode()))
			PG_RE_THROW();
		FlushErrorState();
		InterruptHoldoffCount = save_holdoff;
		QueryCancelHoldoffCount = save_cancel_holdoff;
		rc = REG_ESPACE;
	}
	PG_END_TRY();
	MemoryContextReset(exec_cxt);

	if (rc == REG_OKAY)
	{
		if (slot->re.re_nsub == e->nkeys && !(slot->re.re_info & REG_UBACKREF))
		{
			slot->state = SLOT_READY;
			debug_stats.compiles++;
			debug_stats.live++;
			return;
		}
		pssc_regfree(&slot->re);
	}
	if (slot->cxt != NULL)
		MemoryContextDelete(slot->cxt);
	slot->cxt = NULL;
	if (rc != REG_OKAY && interrupt_pending())
		return;					/* held off: retry next time */

failed:
	slot->state = SLOT_FAILED;
	debug_stats.failed++;
	pssc_extract_note_regex_compile_failure();
}

/*
 * Matches the READY slot against body; see regex_runtime.h for the
 * semantics. Allocates from exec_cxt (the caller resets it).
 */
static void
match_slot(Slot *slot, int index, const PsscExtractor *e,
		   const PsscExtractorList *list, const char *body, size_t len,
		   const PsscPairOut *out, PsscPairResult *result)
{
	int			enc = GetDatabaseEncoding();
	const PsscBlobStr *keys = pssc_extractor_keys(list, e);
	size_t		nkeys = e->nkeys;
	pg_wchar   *w;
	size_t	   *off;			/* char index -> byte offset, [wlen + 1] */
	regmatch_t *pmatch;
	bool		found[PSSC_MAX_TAG_LIST_ENTRIES + 1];
	size_t		nfound = 0;
	int			wlen;
	size_t		nchars;
	size_t		pos;
	size_t		start;

	if (nkeys == 0 || nkeys > PSSC_MAX_TAG_LIST_ENTRIES ||
		len > (MaxAllocSize / sizeof(size_t)) - 1 ||
		memchr(body, '\0', len) != NULL ||
		!pg_verify_mbstr(enc, body, (int) len, true))
		return;

	w = MemoryContextAllocExtended(exec_cxt, sizeof(pg_wchar) * (len + 1),
								   MCXT_ALLOC_NO_OOM);
	off = MemoryContextAllocExtended(exec_cxt, sizeof(size_t) * (len + 1),
									 MCXT_ALLOC_NO_OOM);
	pmatch = MemoryContextAllocExtended(exec_cxt,
										sizeof(regmatch_t) * (nkeys + 1),
										MCXT_ALLOC_NO_OOM);
	if (w == NULL || off == NULL || pmatch == NULL)
		return;

	wlen = pg_mb2wchar_with_len(body, w, (int) len);
	for (nchars = 0, pos = 0; pos < len; nchars++)
	{
		int			l = pg_encoding_mblen_bounded(enc, body + pos);

		off[nchars] = pos;
		pos += Min((size_t) Max(l, 1), len - pos);
	}
	off[nchars] = len;
	if ((size_t) wlen != nchars)
		return;					/* cannot map offsets: should not happen */

	memset(found, 0, sizeof(found));
	start = 0;
	while (start <= (size_t) wlen && nfound < nkeys)
	{
		int			rc = REG_OKAY;
		size_t		g;
		regoff_t	so;
		regoff_t	eo;

		if (pssc_regex_test_hook != NULL)
			rc = pssc_regex_test_hook(PSSC_REGEX_TEST_EXEC, index);
		if (rc == REG_OKAY)
			rc = pg_regexec(&slot->re, w, (size_t) wlen, start, NULL,
							nkeys + 1, pmatch, 0);
		if (rc == REG_NOMATCH)
			break;
		if (rc != REG_OKAY)
		{
			(void) interrupt_pending();
			break;
		}

		for (g = 1; g <= nkeys; g++)
		{
			PsscPair   *p;

			so = pmatch[g].rm_so;
			eo = pmatch[g].rm_eo;
			if (so < 0 || eo < so || eo > wlen || found[g])
				continue;
			found[g] = true;
			nfound++;
			if (result->npairs >= out->max_pairs)
			{
				result->ndropped++;
				continue;
			}
			p = &out->pairs[result->npairs++];
			p->key = pssc_blob_str(list, keys[g - 1]);
			p->keylen = keys[g - 1].len;
			p->value = body + off[so];
			p->valuelen = off[eo] - off[so];
			p->flags = 0;
		}

		so = pmatch[0].rm_so;
		eo = pmatch[0].rm_eo;
		if (so < 0 || eo < so || (size_t) eo < start)
			break;				/* defensive: no progress possible */
		start = eo > so ? (size_t) eo : (size_t) eo + 1;
	}
}

void
pssc_regex_extract(void *arg, int index, const PsscExtractorList *list,
				   const char *body, size_t len, const PsscPairOut *out,
				   PsscPairResult *result)
{
	const PsscExtractor *e;
	Slot	   *slot;
	MemoryContext oldcxt = CurrentMemoryContext;
	uint32		save_holdoff = InterruptHoldoffCount;
	uint32		save_cancel_holdoff = QueryCancelHoldoffCount;

	memset(result, 0, sizeof(*result));
	if (regex_cxt == NULL || list == NULL || index < 0 ||
		index >= PSSC_MAX_EXTRACTORS || (uint32) index >= list->nextractors)
		return;
	e = &list->extractors[index];
	if (e->kind != PSSC_EXTRACTOR_REGEX)
		return;

	pssc_regex_release_stale();
	slot = &slots[index];
	if (slot->state == SLOT_EMPTY)
		compile_slot(slot, index, e, list);
	if (slot->state != SLOT_READY)
		return;

	PG_TRY();
	{
		match_slot(slot, index, e, list, body, len, out, result);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(exec_cxt);
		if (must_rethrow(geterrcode()))
			PG_RE_THROW();
		FlushErrorState();
		InterruptHoldoffCount = save_holdoff;
		QueryCancelHoldoffCount = save_cancel_holdoff;
		/* keep the pairs reported before the error: they are valid */
	}
	PG_END_TRY();
	MemoryContextReset(exec_cxt);
}

void
pssc_regex_debug_stats(PsscRegexDebugStats *stats)
{
	*stats = debug_stats;
}

void
pssc_regex_init(void)
{
	if (regex_cxt == NULL)
		regex_cxt = AllocSetContextCreate(TopMemoryContext,
										  "pg_stat_statement_context regex",
										  ALLOCSET_SMALL_SIZES);
	if (exec_cxt == NULL)
		exec_cxt = AllocSetContextCreate(TopMemoryContext,
										 "pg_stat_statement_context regex exec",
										 ALLOCSET_DEFAULT_SIZES);
	pssc_extract_set_regex_hook(pssc_regex_extract, NULL);
}
