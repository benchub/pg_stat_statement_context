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
 * an interrupted compile is retried next time.
 *
 * Compile time. Each lazy compile runs under the compile time limit
 * (pssc_regex_compile()): in a client backend a compile still running at
 * the limit is aborted through the engine's cancel check and counts as a
 * compile failure, without an error for the statement; a genuine cancel or
 * timeout during the compile still propagates. A compile that hit the limit
 * because it was descheduled (little CPU time used) is retried, a few
 * times. Every attempt compiles into a new context, deleted if the attempt
 * fails: on PG16+ an attempt stopped by the limit throws out of
 * pg_regcomp() before its cleanup, leaving its allocations behind (PG14/15
 * return REG_CANCEL after freeing their own). The slot keeps the context of
 * the successful attempt. With interrupts held off the compile is put off
 * (retried next time), since it could not be bounded.
 *
 * A failed match yields no (further) pairs for that comment and is not
 * counted.
 */
#include "postgres.h"

#include <signal.h>
#include <time.h>

#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "portability/instr_time.h"
#include "regex/regex.h"
#include "utils/memutils.h"
#include "utils/timeout.h"

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
int			pssc_regex_compile_limit_ms = PSSC_REGEX_COMPILE_LIMIT_MS;

static Slot slots[PSSC_MAX_EXTRACTORS];
static Slot norm_slots[PSSC_MAX_NORMALIZE_RULES];
static uint64 slots_generation;
static bool slots_valid = false;
static MemoryContext regex_cxt = NULL;	/* parent of the per-regex contexts */
static MemoryContext exec_cxt = NULL;	/* per-call scratch, reset after use */
static PsscRegexDebugStats debug_stats;

/* See pssc_regex_transient_failures(). */
static uint64 transient_failures = 0;

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
	for (i = 0; i < PSSC_MAX_NORMALIZE_RULES; i++)
		release_slot(&norm_slots[i]);
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
 * Compile time limit (pssc_regex_compile()). In a client backend the limit
 * is a USER_TIMEOUT whose handler raises a query cancel, so the engine
 * aborts the compile at its next interrupt check: PG14/15's engine returns
 * REG_CANCEL, PG16+'s throws the usual "canceling statement" ERROR from
 * CHECK_FOR_INTERRUPTS(). While the timeout is armed, SIGINT goes through
 * compile_sigint_handler(), which notes that a genuine cancel (a client
 * cancel request, or statement_timeout / lock_timeout, which signal the
 * backend itself) arrived and then runs the regular handler. The only other
 * source of QueryCancelPending is a recovery conflict on PG14-16, set by
 * the SIGUSR1 (procsignal) handler; compile_sigusr1_handler() detects it.
 * On disarm the cancel is ours, and is consumed, only if the deadline fired
 * and nothing else requested a cancel; otherwise it is left to propagate as
 * usual. Consuming it re-arms InterruptPending: ProcessInterrupts() clears
 * it before raising the cancel, and other interrupts processed after the
 * cancel (recovery conflicts on PG17+, transaction_timeout, ...) may still
 * be pending.
 */
typedef enum DeadlineResult
{
	DEADLINE_NOT_FIRED,			/* the limit did not expire */
	DEADLINE_OWN,				/* expired; its cancel was the only one */
	DEADLINE_SHARED				/* expired, and a genuine cancel came too */
} DeadlineResult;

#ifndef WIN32
static TimeoutId compile_timeout_id;
static bool compile_timeout_registered = false;
static volatile sig_atomic_t deadline_armed = false;
static volatile sig_atomic_t deadline_fired = false;
static volatile sig_atomic_t foreign_cancel = false;
static struct sigaction saved_sigint;
static struct sigaction saved_sigusr1;

static void
compile_deadline_handler(void)
{
	if (!deadline_armed)
		return;
	deadline_fired = true;
	if (QueryCancelPending)
		foreign_cancel = true;
	else
		QueryCancelPending = true;
	InterruptPending = true;
}

static void
compile_sigint_handler(int signo)
{
	foreign_cancel = true;
	saved_sigint.sa_handler(signo);
}

/*
 * Hides QueryCancelPending from the regular handler, so that one it sets
 * (a PG14-16 recovery conflict) is seen even after the deadline set it.
 */
static void
compile_sigusr1_handler(int signo)
{
	bool		was_pending = QueryCancelPending;

	QueryCancelPending = false;
	saved_sigusr1.sa_handler(signo);
	if (QueryCancelPending)
		foreign_cancel = true;
	else if (was_pending)
		QueryCancelPending = true;
}

static bool
plain_handler(int signo, struct sigaction *sa)
{
	return sigaction(signo, NULL, sa) == 0 && !(sa->sa_flags & SA_SIGINFO) &&
		sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL;
}

/*
 * Arms the limit, if this process can: a regular client backend past its
 * startup (InitializeTimeouts() would forget the registration) with
 * interrupts not held off and plain SIGINT and SIGUSR1 handlers. Returns
 * false if not.
 */
static bool
compile_deadline_arm(int limit_ms)
{
	struct sigaction act;

	if (!IsUnderPostmaster || MyBackendType != B_BACKEND ||
		!IsNormalProcessingMode())
		return false;
	if (!plain_handler(SIGINT, &saved_sigint) ||
		!plain_handler(SIGUSR1, &saved_sigusr1))
		return false;
	if (!compile_timeout_registered)
	{
		compile_timeout_id = RegisterTimeout(USER_TIMEOUT, compile_deadline_handler);
		compile_timeout_registered = true;
	}
	deadline_fired = false;
	foreign_cancel = false;
	act = saved_sigint;
	act.sa_handler = compile_sigint_handler;
	if (sigaction(SIGINT, &act, NULL) != 0)
		return false;
	act = saved_sigusr1;
	act.sa_handler = compile_sigusr1_handler;
	/*
	 * The deadline (SIGALRM) must not fire while the wrapper has hidden
	 * QueryCancelPending, or its cancel would be taken for a foreign one.
	 */
	sigaddset(&act.sa_mask, SIGALRM);
	if (sigaction(SIGUSR1, &act, NULL) != 0)
	{
		sigaction(SIGINT, &saved_sigint, NULL);
		return false;
	}
	deadline_armed = true;
	enable_timeout_after(compile_timeout_id, limit_ms);
	return true;
}

/*
 * Disarms the limit. With DEADLINE_OWN its cancel is no longer pending.
 * SIGINT and SIGUSR1 are blocked while deciding and restoring their
 * handlers, so a genuine cancel is never lost.
 */
static DeadlineResult
compile_deadline_disarm(void)
{
	sigset_t	block;
	sigset_t	old;
	DeadlineResult res;

	disable_timeout(compile_timeout_id, false);
	deadline_armed = false;
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGUSR1);
	sigprocmask(SIG_BLOCK, &block, &old);
	res = !deadline_fired ? DEADLINE_NOT_FIRED
		: foreign_cancel ? DEADLINE_SHARED : DEADLINE_OWN;
	if (res == DEADLINE_OWN)
	{
		QueryCancelPending = false;
		InterruptPending = true;
	}
	sigaction(SIGINT, &saved_sigint, NULL);
	sigaction(SIGUSR1, &saved_sigusr1, NULL);
	sigprocmask(SIG_SETMASK, &old, NULL);
	return res;
}

void
pssc_regex_test_expire_in(int ms)
{
	if (deadline_armed)
		enable_timeout_after(compile_timeout_id, ms);
}
#else
void
pssc_regex_test_expire_in(int ms)
{
}

static bool
compile_deadline_arm(int limit_ms)
{
	return false;
}

static DeadlineResult
compile_deadline_disarm(void)
{
	return DEADLINE_NOT_FIRED;
}
#endif

/* The test hook for test_phase (if >= 0), then the engine. May throw. */
static int
run_compile(MemoryContext cxt, regex_t *re, const pg_wchar *pat, size_t len,
			int test_phase, int test_index)
{
	int			rc = REG_OKAY;

	if (test_phase >= 0 && pssc_regex_test_hook != NULL)
		rc = pssc_regex_test_hook(test_phase, test_index);
	if (rc == REG_OKAY)
		rc = pssc_regcomp(cxt, re, pat, len, REG_ADVANCED, C_COLLATION_OID);
	return rc;
}

/*
 * CPU time this process has used, in ms; without a CPU clock, wall time
 * (then a stall can't be told from a slow compile).
 */
static double
cpu_time_ms(void)
{
#ifdef CLOCK_PROCESS_CPUTIME_ID
	struct timespec ts;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
		return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
	{
		instr_time	now;

		INSTR_TIME_SET_CURRENT(now);
		return INSTR_TIME_GET_MILLISEC(now);
	}
}

/*
 * One attempt of pssc_regex_compile(): the same contract, with the limit
 * on wall-clock time when a timer can be armed, else on CPU time.
 */
static int
compile_attempt(MemoryContext cxt, regex_t *re, const pg_wchar *pat,
				size_t len, bool strict, int test_phase, int test_index,
				int limit, bool *timed)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	uint32		save_holdoff = InterruptHoldoffCount;
	uint32		save_cancel_holdoff = QueryCancelHoldoffCount;
	volatile int rc = REG_OKAY;
	volatile bool aborted = false;
	bool		held_off;
	double		start;

	*timed = false;
	held_off = InterruptHoldoffCount != 0 || QueryCancelHoldoffCount != 0 ||
		CritSectionCount != 0;
	if (!held_off && compile_deadline_arm(limit))
	{
		*timed = true;
		PG_TRY();
		{
			rc = run_compile(cxt, re, pat, len, test_phase, test_index);
		}
		PG_CATCH();
		{
			DeadlineResult res;

			MemoryContextSwitchTo(oldcxt);
			res = compile_deadline_disarm();

			/*
			 * The cancel raised is ours if only the deadline asked for one.
			 * If a genuine cancel came too, but after ProcessInterrupts()
			 * consumed the flag, it is still pending and is raised at the
			 * next CHECK_FOR_INTERRUPTS(); otherwise this is the genuine
			 * one.
			 */
			if (geterrcode() != ERRCODE_QUERY_CANCELED ||
				!(res == DEADLINE_OWN ||
				  (res == DEADLINE_SHARED && QueryCancelPending)))
				PG_RE_THROW();
			FlushErrorState();
			InterruptHoldoffCount = save_holdoff;
			QueryCancelHoldoffCount = save_cancel_holdoff;
			InterruptPending = true;
			aborted = true;
		}
		PG_END_TRY();
		if (aborted)
			return PSSC_REGEX_COMPILE_TOO_SLOW;
		if (compile_deadline_disarm() == DEADLINE_OWN)
		{
			/* REG_CANCEL from PG14/15, or finished before noticing */
			if (rc != REG_OKAY)
				return PSSC_REGEX_COMPILE_TOO_SLOW;
			if (strict)
			{
				pssc_regfree(re);
				return PSSC_REGEX_COMPILE_TOO_SLOW;
			}
		}
		return rc;
	}

	/*
	 * No timer: interrupts are held off (a client backend puts the lazy
	 * compile off), or not a client backend (e.g. the postmaster checking
	 * postgresql.conf, a background worker): unbounded.
	 */
	if (!strict && held_off && IsUnderPostmaster && MyBackendType == B_BACKEND)
		return PSSC_REGEX_COMPILE_DEFERRED;
	start = cpu_time_ms();
	rc = run_compile(cxt, re, pat, len, test_phase, test_index);
	if (strict && rc == REG_OKAY && cpu_time_ms() - start > limit)
	{
		pssc_regfree(re);
		return PSSC_REGEX_COMPILE_TOO_SLOW;
	}
	return rc;
}

/*
 * The limit is wall-clock time where a timer bounds it, so a compile that
 * was descheduled (a loaded host or VM) could hit it with a normal pattern.
 * An attempt over the limit is retried, up to PSSC_REGEX_COMPILE_ATTEMPTS
 * attempts, if it used less than half the limit in CPU time: then most of
 * the time went to a stall, not to the pattern. Interrupts pending from
 * the attempt are processed before the next one.
 *
 * Each attempt compiles into a new child of parent, stored in *cxtp before
 * the attempt starts (so that a caller catching an error can delete it).
 * An attempt that fails is not trusted to have freed what it allocated (on
 * PG16+ a compile stopped by the limit throws out of pg_regcomp() before
 * its cleanup), so its context is deleted, and *cxtp reset to NULL, before
 * the next attempt or returning. On REG_OKAY *cxtp holds the regex.
 */
static int
compile_retrying(MemoryContext parent, MemoryContext *cxtp, regex_t *re,
				 const pg_wchar *pat, size_t len, bool strict,
				 int test_phase, int test_index)
{
	int			limit = pssc_regex_compile_limit_ms;
	int			rc;

	*cxtp = NULL;
	for (int attempt = 1;; attempt++)
	{
		double		start = cpu_time_ms();
		bool		timed = false;

		*cxtp = AllocSetContextCreate(parent,
									  "pg_stat_statement_context regex pattern",
									  ALLOCSET_SMALL_SIZES);
		if (limit <= 0)
			rc = run_compile(*cxtp, re, pat, len, test_phase, test_index);
		else
			rc = compile_attempt(*cxtp, re, pat, len, strict, test_phase,
								 test_index, limit, &timed);
		if (rc != REG_OKAY)
		{
			MemoryContextDelete(*cxtp);
			*cxtp = NULL;
		}
		if (rc != PSSC_REGEX_COMPILE_TOO_SLOW || !timed ||
			attempt >= PSSC_REGEX_COMPILE_ATTEMPTS ||
			cpu_time_ms() - start >= limit / 2.0)
			return rc;
		CHECK_FOR_INTERRUPTS();
	}
}

int
pssc_regex_compile(MemoryContext cxt, regex_t *re, const pg_wchar *pat,
				   size_t len, bool strict, int test_phase, int test_index)
{
	MemoryContext acxt;

	return compile_retrying(cxt, &acxt, re, pat, len, strict, test_phase,
							test_index);
}

/*
 * Compiles pat[0, patlen) into slot, for extractor or rule index (test hook
 * phases ctx_phase and comp_phase). The compiled regex must have between
 * nsub_min and nsub_max capture groups and no back-references. On return
 * the slot is READY or FAILED (also when over the compile time limit), or
 * still EMPTY if an interrupt is pending or interrupts are held off.
 * Interrupts propagate as ERROR (the slot is then EMPTY and its context
 * gone).
 */
static void
compile_slot(Slot *slot, int index, int ctx_phase, int comp_phase,
			 const char *pat, uint32 patlen32, size_t nsub_min, size_t nsub_max)
{
	int			patlen = (int) patlen32;
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
	if (patlen32 == 0 || patlen32 > PSSC_MAX_REGEX_PATTERN_LEN ||
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
			rc = pssc_regex_test_hook(ctx_phase, index);
		if (rc == REG_OKAY)
		{
			wpat = MemoryContextAlloc(exec_cxt, sizeof(pg_wchar) * (patlen + 1));
			wlen = pg_mb2wchar_with_len(pat, wpat, patlen);
			rc = compile_retrying(regex_cxt, &slot->cxt, &slot->re, wpat, wlen,
								  false, comp_phase, index);
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
		if (slot->re.re_nsub >= nsub_min && slot->re.re_nsub <= nsub_max &&
			!(slot->re.re_info & REG_UBACKREF))
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
	if (rc == PSSC_REGEX_COMPILE_DEFERRED ||
		(rc != REG_OKAY && rc != PSSC_REGEX_COMPILE_TOO_SLOW &&
		 interrupt_pending()))
	{
		transient_failures++;
		return;					/* held off: retry next time */
	}

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
	{
		transient_failures++;
		return;
	}

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
			transient_failures++;
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
		compile_slot(slot, index, PSSC_REGEX_TEST_CONTEXT, PSSC_REGEX_TEST_COMPILE,
					 pssc_blob_str(list, e->pattern), e->pattern.len,
					 e->nkeys, e->nkeys);
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
		transient_failures++;
		/* keep the pairs reported before the error: they are valid */
	}
	PG_END_TRY();
	MemoryContextReset(exec_cxt);
}

/* ---------------- value normalization ---------------- */

/* Output of one rule: at most limit bytes; full once something was cut. */
typedef struct NormBuf
{
	char	   *buf;
	size_t		len;
	size_t		limit;
	bool		full;
} NormBuf;

static void
normbuf_append(NormBuf *b, const char *s, size_t len)
{
	size_t		n;

	if (b->full)
		return;
	n = Min(len, b->limit - b->len);
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	if (n < len)
		b->full = true;
}

/*
 * Applies one READY rule to val[0, vlen) (valid, no NUL) like
 * regexp_replace(val COLLATE "C", pattern, replacement, 'g'), writing at
 * most limit bytes into b (cut on a character boundary). Returns false on an
 * engine error. Allocates from exec_cxt (the caller resets it).
 */
static bool
replace_slot(Slot *slot, int index, const PsscNormalizeRule *rule,
			 const PsscNormalizeList *list, const char *val, size_t vlen,
			 NormBuf *b)
{
	int			enc = GetDatabaseEncoding();
	const char *repl = pssc_normalize_str(list, rule->replacement);
	size_t		repllen = rule->replacement.len;
	size_t		nmatch = (size_t) rule->max_ref + 1;
	pg_wchar   *w;
	size_t	   *off;			/* char index -> byte offset, [wlen + 1] */
	regmatch_t *pmatch;
	int			wlen;
	size_t		nchars;
	size_t		pos;
	size_t		data_pos = 0;	/* chars of val consumed */
	size_t		search_start = 0;

	if (vlen > (MaxAllocSize / sizeof(size_t)) - 1)
		return false;
	w = MemoryContextAllocExtended(exec_cxt, sizeof(pg_wchar) * (vlen + 1),
								   MCXT_ALLOC_NO_OOM);
	off = MemoryContextAllocExtended(exec_cxt, sizeof(size_t) * (vlen + 1),
									 MCXT_ALLOC_NO_OOM);
	pmatch = MemoryContextAllocExtended(exec_cxt, sizeof(regmatch_t) * nmatch,
										MCXT_ALLOC_NO_OOM);
	if (w == NULL || off == NULL || pmatch == NULL)
		return false;

	wlen = pg_mb2wchar_with_len(val, w, (int) vlen);
	for (nchars = 0, pos = 0; pos < vlen; nchars++)
	{
		int			l = pg_encoding_mblen_bounded(enc, val + pos);

		off[nchars] = pos;
		pos += Min((size_t) Max(l, 1), vlen - pos);
	}
	off[nchars] = vlen;
	if ((size_t) wlen != nchars)
		return false;

	while (search_start <= (size_t) wlen && !b->full)
	{
		int			rc = REG_OKAY;
		regoff_t	so;
		regoff_t	eo;

		if (pssc_regex_test_hook != NULL)
			rc = pssc_regex_test_hook(PSSC_REGEX_TEST_NORM_EXEC, index);
		if (rc == REG_OKAY)
			rc = pg_regexec(&slot->re, w, (size_t) wlen, search_start, NULL,
							nmatch, pmatch, 0);
		if (rc == REG_NOMATCH)
			break;
		if (rc != REG_OKAY)
		{
			(void) interrupt_pending();
			return false;
		}
		so = pmatch[0].rm_so;
		eo = pmatch[0].rm_eo;
		if (so < 0 || (size_t) so < data_pos || eo < so || eo > wlen)
			return false;		/* defensive: no progress possible */

		normbuf_append(b, val + off[data_pos], off[so] - off[data_pos]);
		for (size_t i = 0; i < repllen && !b->full; i++)
		{
			char		c = repl[i];

			if (c == '\\' && i + 1 < repllen)
			{
				char		n = repl[++i];
				regoff_t	gso = -1;
				regoff_t	geo = -1;

				if (n >= '1' && n <= '9' && (size_t) (n - '0') < nmatch)
				{
					gso = pmatch[n - '0'].rm_so;
					geo = pmatch[n - '0'].rm_eo;
				}
				else if (n == '&')
				{
					gso = so;
					geo = eo;
				}
				else
				{
					normbuf_append(b, &n, 1);	/* \\ (the check hook allows no other) */
					continue;
				}
				if (gso >= 0 && geo >= gso && geo <= wlen)
					normbuf_append(b, val + off[gso], off[geo] - off[gso]);
				continue;
			}
			normbuf_append(b, &c, 1);
		}
		data_pos = (size_t) eo;
		search_start = (size_t) eo + (so == eo ? 1 : 0);
	}
	normbuf_append(b, val + off[data_pos], vlen - off[data_pos]);
	if (b->full)
		b->len = (size_t) pg_encoding_mbcliplen(enc, b->buf, (int) b->len, (int) b->len);
	return true;
}

/*
 * Compiles rule index, after checking its replacement in this database's
 * encoding (the check hook may have run in another one).
 */
static void
compile_norm_slot(Slot *slot, int index, const PsscNormalizeRule *rule,
				  const PsscNormalizeList *list)
{
	if (rule->replacement.len > PSSC_MAX_NORMALIZE_REPLACEMENT_LEN ||
		!pg_verify_mbstr(GetDatabaseEncoding(),
						 pssc_normalize_str(list, rule->replacement),
						 (int) rule->replacement.len, true))
	{
		slot->state = SLOT_FAILED;
		debug_stats.failed++;
		pssc_extract_note_regex_compile_failure();
		return;
	}
	compile_slot(slot, index, PSSC_REGEX_TEST_NORM_CONTEXT, PSSC_REGEX_TEST_NORM_COMPILE,
				 pssc_normalize_str(list, rule->pattern), rule->pattern.len,
				 rule->max_ref, SIZE_MAX);
}

PsscNormalizeResult
pssc_regex_normalize(const PsscNormalizeList *list,
					 const char *key, size_t klen,
					 const char *val, size_t vlen, size_t limit,
					 void *(*alloc) (void *arg, size_t size), void *alloc_arg,
					 const char **out, size_t *outlen)
{
	const char *cur = val;
	size_t		curlen = vlen;
	bool		any = false;

	if (list == NULL)
		return PSSC_NORMALIZE_NO_RULES;
	for (uint32 i = 0; i < list->nrules && i < PSSC_MAX_NORMALIZE_RULES; i++)
	{
		const PsscNormalizeRule *rule = &list->rules[i];
		Slot	   *slot = &norm_slots[i];
		MemoryContext oldcxt;
		uint32		save_holdoff;
		uint32		save_cancel_holdoff;
		NormBuf		b;
		volatile bool ok = false;

		if (rule->key.len != klen ||
			memcmp(pssc_normalize_str(list, rule->key), key, klen) != 0)
			continue;
		if (!any)
		{
			if (regex_cxt == NULL || limit < vlen)
				return PSSC_NORMALIZE_FAILED;
			pssc_regex_release_stale();
			any = true;
		}
		if (slot->state == SLOT_EMPTY)
			compile_norm_slot(slot, (int) i, rule, list);
		if (slot->state != SLOT_READY)
			return PSSC_NORMALIZE_FAILED;

		b.buf = alloc(alloc_arg, Max(limit, 1));
		b.len = 0;
		b.limit = limit;
		b.full = false;
		if (b.buf == NULL)
			return PSSC_NORMALIZE_FAILED;

		oldcxt = CurrentMemoryContext;
		save_holdoff = InterruptHoldoffCount;
		save_cancel_holdoff = QueryCancelHoldoffCount;
		PG_TRY();
		{
			ok = replace_slot(slot, (int) i, rule, list, cur, curlen, &b);
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
			ok = false;
		}
		PG_END_TRY();
		MemoryContextReset(exec_cxt);
		if (!ok)
			return PSSC_NORMALIZE_FAILED;
		cur = b.buf;
		curlen = b.len;
	}
	if (!any)
		return PSSC_NORMALIZE_NO_RULES;
	*out = cur;
	*outlen = curlen;
	return PSSC_NORMALIZE_DONE;
}

void
pssc_regex_debug_stats(PsscRegexDebugStats *stats)
{
	*stats = debug_stats;
}

uint64
pssc_regex_transient_failures(void)
{
	return transient_failures;
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
