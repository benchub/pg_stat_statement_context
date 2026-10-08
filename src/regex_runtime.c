/*
 * regex_runtime.c
 *		Regex extractor runtime: see regex_runtime.h.
 *
 * Memory. Each backend compiles a regex extractor's pattern the first time
 * the pipeline needs it after a regex generation change
 * (pssc_guc_regex_generation(): extractors or normalize changed), with the
 * core engine (REG_ADVANCED, C collation), into its own child of a long-lived context. The compiled
 * regexes of an older generation are released at the next pipeline run
 * (pssc_regex_release_stale(), called by pssc_extract_tags()) or hook call:
 * pg_regfree() first, then the child context is deleted. Both are needed:
 * PG14/15's engine allocates with malloc, so only pg_regfree() frees it,
 * while PG16+'s uses palloc in the current context (the child), where
 * pg_regfree() pfrees and deleting the child reclaims anything left over.
 *
 * Errors. The hook must not fail the user's statement because of a
 * pattern, but must still honor query cancel and other interrupts.
 * Compilation (including creating the pattern's memory context) and
 * matching run in PG_TRY, and matching allocates only with
 * MCXT_ALLOC_NO_OOM. The protected code touches only memory in our own
 * contexts and holds no locks, buffers or other resources, so no
 * subtransaction is needed. Only the errors a compile or match is expected
 * to raise (out of memory, program limit exceeded, invalid regular
 * expression: may_swallow()) are swallowed with FlushErrorState() after
 * restoring the interrupt holdoff counts that errfinish() zeroed; any other
 * ERROR, interrupts included, is re-thrown.
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
 * timeout during the compile still propagates, at the latest when the
 * limit expires (SIGINT is blocked meanwhile; see "Compile time limit"
 * below). A compile that hit the limit because it was descheduled (little
 * CPU time used) is retried, a few times. Every attempt compiles into a new context, deleted if the attempt
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

#include "access/parallel.h"
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

int			pssc_regex_compile_limit_ms = PSSC_REGEX_COMPILE_LIMIT_MS;

#ifdef PSSC_TESTING
PsscRegexTestHook pssc_regex_test_hook = NULL;
PsscRegexTestEngineHook pssc_regex_test_engine_hook = NULL;
static PsscRegexDebugStats debug_stats;

/* The fault-injection hook's result (regex_runtime.h); REG_OKAY without it. */
#define TEST_HOOK(phase, index) \
	(pssc_regex_test_hook != NULL ? pssc_regex_test_hook((phase), (index)) : REG_OKAY)
#define DEBUG_STAT_ADD(field, n) (debug_stats.field += (n))
#else
/* The release build has no test hooks (export.h): the engine always runs. */
#define TEST_HOOK(phase, index) REG_OKAY
#define DEBUG_STAT_ADD(field, n) ((void) 0)
#endif

static Slot slots[PSSC_MAX_EXTRACTORS];
static Slot norm_slots[PSSC_MAX_NORMALIZE_RULES];
static uint64 slots_generation;
static bool slots_valid = false;
static MemoryContext regex_cxt = NULL;	/* parent of the per-regex contexts */
static MemoryContext exec_cxt = NULL;	/* per-call scratch, reset after use */

/* See pssc_regex_transient_failures(). */
static uint64 transient_failures = 0;

static void
release_slot(Slot *slot)
{
	if (slot->state == SLOT_READY)
	{
		pssc_regfree(&slot->re);
		DEBUG_STAT_ADD(frees, 1);
		DEBUG_STAT_ADD(live, -1);
	}
	else if (slot->state == SLOT_FAILED)
		DEBUG_STAT_ADD(failed, -1);
	if (slot->cxt != NULL)
		MemoryContextDelete(slot->cxt);
	slot->cxt = NULL;
	slot->state = SLOT_EMPTY;
}

/*
 * Releases the compiled regexes of an older regex generation. Never
 * throws.
 */
void
pssc_regex_release_stale(void)
{
	uint64		gen = pssc_guc_regex_generation();
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
 * Catch policy. The PG_TRY sites of this file (compile_slot(),
 * pssc_regex_extract(), pssc_regex_normalize()) protect code that touches
 * only private memory: the pattern's or the scratch context and the
 * engine's own allocations. It takes no locks, pins no buffers and acquires
 * no resource-owner resources, so an error there leaves nothing that a
 * subtransaction abort would have to release, and none is used (one per
 * match would be far too costly). Even so, only the errors the engine and
 * its allocations are expected to raise are swallowed; anything else (an
 * interrupt, a bug, an unexpected condition) is re-thrown, so the statement
 * fails as it would without this extension.
 */
static bool
may_swallow(int code)
{
	switch (code)
	{
		case ERRCODE_OUT_OF_MEMORY:
		case ERRCODE_PROGRAM_LIMIT_EXCEEDED:
		case ERRCODE_INVALID_REGULAR_EXPRESSION:
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
 * is a USER_TIMEOUT whose handler raises a query cancel the way core's
 * SIGINT handler does (QueryCancelPending and InterruptPending), so the
 * engine aborts the compile at its next interrupt check: PG14/15's engine
 * returns REG_CANCEL (its rcancelrequested() tests InterruptPending &&
 * (QueryCancelPending || ProcDiePending)), PG16+'s throws the usual
 * "canceling statement" ERROR from CHECK_FOR_INTERRUPTS(). Setting only
 * InterruptPending does not stop either: PG14/15 ignore it, and on PG16+
 * ProcessInterrupts() clears it and returns, and of the flags it handles
 * only QueryCancelPending (and recovery conflicts) raise an ERROR rather
 * than FATAL. So the handler sets QueryCancelPending; nothing here ever
 * clears it: ProcessInterrupts() consumes it.
 *
 * To tell the deadline's cancel from a genuine one, SIGINT is blocked
 * (sigprocmask(), core's handlers stay in place) while the limit is armed.
 * A client cancel and statement_timeout / lock_timeout (whose handlers
 * signal the backend itself) then stay pending in the signal mask until the
 * compile ends or the deadline fires, i.e. they wait at most the limit. A
 * cancel error raised with the deadline fired and no SIGINT pending is the
 * deadline's and is swallowed; with one pending, it also reports the
 * genuine request and propagates. Once the limit is disarmed, SIGINT is
 * unblocked, so a pending one is handled by core's handler as usual. The
 * only other source of QueryCancelPending, a PG14-16 recovery conflict
 * (SIGUSR1), raises its own SQLSTATE and so is never taken for the
 * deadline's. A swallowed cancel re-arms InterruptPending: ProcessInterrupts()
 * clears it before raising the cancel, and other interrupts processed after
 * the cancel (recovery conflicts on PG17+, transaction_timeout, ...) may
 * still be pending; with nothing pending ProcessInterrupts() just returns.
 */
#ifndef WIN32
static TimeoutId compile_timeout_id;
static bool compile_timeout_registered = false;
static volatile sig_atomic_t deadline_armed = false;
static volatile sig_atomic_t deadline_fired = false;
static sigset_t deadline_saved_mask;

static void
compile_deadline_handler(void)
{
	if (!deadline_armed)
		return;
	deadline_fired = true;
	QueryCancelPending = true;
	InterruptPending = true;
}

static bool
sigint_held(void)
{
	sigset_t	pending;

	return sigpending(&pending) == 0 && sigismember(&pending, SIGINT);
}

static void
unblock_sigint(void)
{
	sigprocmask(SIG_SETMASK, &deadline_saved_mask, NULL);
}

/*
 * Arms the limit, if this process can: a regular client backend past its
 * startup (InitializeTimeouts() would forget the registration), with
 * interrupts not held off (checked by the caller). Blocks SIGINT; a cancel
 * pending before is raised first. Returns false if not armed.
 */
static bool
compile_deadline_arm(int limit_ms)
{
	sigset_t	block;

	if (!IsUnderPostmaster || MyBackendType != B_BACKEND ||
		!IsNormalProcessingMode())
		return false;
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (sigprocmask(SIG_BLOCK, &block, &deadline_saved_mask) != 0)
			return false;
		if (!QueryCancelPending)
			break;
		/* a cancel came in meanwhile: raise it */
		unblock_sigint();
	}
	if (!compile_timeout_registered)
	{
		compile_timeout_id = RegisterTimeout(USER_TIMEOUT, compile_deadline_handler);
		compile_timeout_registered = true;
	}
	deadline_fired = false;
	deadline_armed = true;
	enable_timeout_after(compile_timeout_id, limit_ms);
	return true;
}

/* Stops the limit's timer; SIGINT stays blocked. */
static void
compile_deadline_stop(void)
{
	disable_timeout(compile_timeout_id, false);
	deadline_armed = false;
}

/*
 * With the limit stopped after it fired, SIGINT still blocked and no error
 * being handled: raises any interrupt pending, except the deadline's own
 * cancel, which is consumed. Unblocks SIGINT.
 */
static void
compile_deadline_settle(uint32 save_holdoff, uint32 save_cancel_holdoff)
{
	MemoryContext oldcxt = CurrentMemoryContext;

	if (sigint_held())
	{
		/* a genuine cancel: core raises it, with the deadline's merged */
		unblock_sigint();
		CHECK_FOR_INTERRUPTS();
		return;
	}
	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		if (geterrcode() != ERRCODE_QUERY_CANCELED || sigint_held())
		{
			unblock_sigint();
			PG_RE_THROW();
		}
		FlushErrorState();
		InterruptHoldoffCount = save_holdoff;
		QueryCancelHoldoffCount = save_cancel_holdoff;
		InterruptPending = true;
	}
	PG_END_TRY();
	unblock_sigint();
}

/*
 * The cancel error being handled was raised with a genuine SIGINT pending,
 * so it reports that request too; the pending signal is the same request
 * and is dropped rather than cancelling a second time. Unless a statement
 * or lock timeout fell due after the error was raised: then the error does
 * not name it, and false is returned.
 */
static bool
drop_coalesced_sigint(void)
{
	sigset_t	set;
	int			sig;

	if (get_timeout_indicator(STATEMENT_TIMEOUT, false) ||
		get_timeout_indicator(LOCK_TIMEOUT, false))
		return false;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	return sigwait(&set, &sig) == 0;
}

/*
 * CopyErrorData() of the error being handled, or NULL if that fails (out of
 * memory): then both errors are still on the error stack, to be flushed.
 */
static ErrorData *
copy_error_guarded(MemoryContext cxt, int test_phase, int test_index)
{
	ErrorData  *volatile edata = NULL;

	PG_TRY();
	{
		if (test_phase >= 0)
			(void) TEST_HOOK(PSSC_REGEX_TEST_COMPILE_CATCH, test_index);
		edata = CopyErrorData();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		edata = NULL;
	}
	PG_END_TRY();
	return edata;
}

#ifdef PSSC_TESTING
void
pssc_regex_test_expire_in(int ms)
{
	if (deadline_armed)
		enable_timeout_after(compile_timeout_id, ms);
}

void
pssc_regex_test_expire_now(void)
{
	compile_deadline_handler();
}
#endif
#else
#ifdef PSSC_TESTING
void
pssc_regex_test_expire_in(int ms)
{
}

void
pssc_regex_test_expire_now(void)
{
}
#endif

static bool
compile_deadline_arm(int limit_ms)
{
	return false;
}
#endif

/*
 * The test hook for test_phase (if >= 0), then the engine, then the test
 * engine hook. May throw.
 */
static int
run_compile(MemoryContext cxt, regex_t *re, const pg_wchar *pat, size_t len,
			int test_phase, int test_index)
{
	int			rc = REG_OKAY;

	if (test_phase >= 0)
		rc = TEST_HOOK(test_phase, test_index);
	if (rc == REG_OKAY)
	{
		rc = pssc_regcomp(cxt, re, pat, len, REG_ADVANCED, C_COLLATION_OID);
#ifdef PSSC_TESTING
		if (test_phase >= 0 && pssc_regex_test_engine_hook != NULL)
			pssc_regex_test_engine_hook(test_phase, test_index, rc);
#endif
	}
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
	bool		held_off;
	double		start;

	*timed = false;
	held_off = InterruptHoldoffCount != 0 || QueryCancelHoldoffCount != 0 ||
		CritSectionCount != 0;
#ifndef WIN32
	if (!held_off && compile_deadline_arm(limit))
	{
		volatile bool aborted = false;
		volatile bool settled = false;	/* a swallowable error, settled */

		*timed = true;
		PG_TRY();
		{
			rc = run_compile(cxt, re, pat, len, test_phase, test_index);
		}
		PG_CATCH();
		{
			/*
			 * SIGINT is blocked until unblock_sigint(), which every way out
			 * of here takes first: nothing before it may throw (PG_TRY does
			 * not restore the signal mask). The one step that allocates,
			 * copying the error, is guarded itself.
			 */
			int			code;

			MemoryContextSwitchTo(oldcxt);
			compile_deadline_stop();
			if (!deadline_fired)
			{
				unblock_sigint();
				PG_RE_THROW();
			}
			code = geterrcode();
			if (!QueryCancelPending)
			{
				/*
				 * The deadline's cancel was consumed by ProcessInterrupts():
				 * raised as this error if it is a cancel, else merged into
				 * it (a PG14-16 recovery conflict).
				 */
				if (code != ERRCODE_QUERY_CANCELED)
				{
					unblock_sigint();
					PG_RE_THROW();
				}
				if (!sigint_held())
				{
					/* the deadline's cancel alone */
					FlushErrorState();
					InterruptHoldoffCount = save_holdoff;
					QueryCancelHoldoffCount = save_cancel_holdoff;
					InterruptPending = true;
					unblock_sigint();
					aborted = true;
				}
				else if (drop_coalesced_sigint())
				{
					unblock_sigint();
					PG_RE_THROW();
				}
				else
				{
					/* a timeout fell due since: core raises it instead */
					FlushErrorState();
					InterruptHoldoffCount = save_holdoff;
					QueryCancelHoldoffCount = save_cancel_holdoff;
					unblock_sigint();
					CHECK_FOR_INTERRUPTS();
					InterruptPending = true;
					aborted = true;
				}
			}
			else if (sigint_held())
			{
				/*
				 * A genuine cancel came too: it stays pending, merged with
				 * the deadline's, and is raised (or discarded with this
				 * error at top level) as usual.
				 */
				unblock_sigint();
				PG_RE_THROW();
			}
			else
			{
				/*
				 * The deadline's cancel is still pending (or, on PG14-16, a
				 * recovery conflict's), behind an error the engine raised
				 * meanwhile: settle it, so it neither cancels the statement
				 * later nor is lost, then go on with the error. One the
				 * callers swallow anyway is not kept.
				 */
				ErrorData  *edata = NULL;

				if (!may_swallow(code))
					edata = copy_error_guarded(oldcxt, test_phase, test_index);
				FlushErrorState();
				InterruptHoldoffCount = save_holdoff;
				QueryCancelHoldoffCount = save_cancel_holdoff;
				compile_deadline_settle(save_holdoff, save_cancel_holdoff);
				if (edata != NULL)
					ReThrowError(edata);
				if (!may_swallow(code))
					ereport(ERROR,
							(errcode(code),
							 errmsg("unexpected error while compiling a regular expression"),
							 errdetail("The error could not be reported: out of memory.")));
				settled = true;
			}
		}
		PG_END_TRY();
		if (aborted)
			return PSSC_REGEX_COMPILE_TOO_SLOW;
		if (settled)
			return REG_ESPACE;
		compile_deadline_stop();
		if (!deadline_fired)
		{
			unblock_sigint();
			return rc;
		}
		/* REG_CANCEL from PG14/15, or finished before noticing */
		PG_TRY();
		{
			compile_deadline_settle(save_holdoff, save_cancel_holdoff);
		}
		PG_CATCH();
		{
			if (rc == REG_OKAY)
				pssc_regfree(re);
			PG_RE_THROW();
		}
		PG_END_TRY();
		if (rc != REG_OKAY)
			return PSSC_REGEX_COMPILE_TOO_SLOW;
		if (strict)
		{
			pssc_regfree(re);
			return PSSC_REGEX_COMPILE_TOO_SLOW;
		}
		return rc;
	}
#endif

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

/* Set by the ProcessUtility hook while ALTER SYSTEM runs. */
static bool in_alter_system = false;

void
pssc_regex_note_alter_system(bool running)
{
	in_alter_system = running;
}

/*
 * The time limit only rejects values a statement sets: ALTER SYSTEM (which
 * validates with PGC_S_FILE, like a reload, hence the flag), or a source
 * from SET and the like (PGC_S_SESSION, PGC_S_TEST; these parameters are
 * PGC_SIGHUP, so such statements fail before the check hook today).
 * Reading the configuration file (the postmaster, every backend again after
 * a reload, pg_file_settings) must not reject a value for time alone:
 * processes would end up with different configurations depending on how
 * they were scheduled (a host preempting a VM is charged to the process as
 * CPU time, so it can't be told from a slow compile), and a backend's
 * rejection is only logged at DEBUG3. There the compile is non-strict: the
 * postmaster runs it to completion, as before, and logs if it took longer
 * than the limit; a backend still stops at the limit and then accepts the
 * value unchecked (its lazy compile checks the pattern again, under the
 * limit). A parallel worker restoring the leader's settings doesn't
 * compile at all: it never extracts, and the leader has the value already.
 */
int
pssc_regex_check_compile(MemoryContext cxt, regex_t *re, const pg_wchar *pat,
						 size_t len, GucSource source, const char *what)
{
	double		start;
	double		used;
	int			rc;

	if (IsParallelWorker())
		return PSSC_REGEX_COMPILE_UNCHECKED;
	if (in_alter_system || source >= PGC_S_INTERACTIVE)
		return pssc_regex_compile(cxt, re, pat, len, true,
								  PSSC_REGEX_TEST_CHECK, -1);
	start = cpu_time_ms();
	rc = pssc_regex_compile(cxt, re, pat, len, false, PSSC_REGEX_TEST_CHECK, -1);
	if (rc == PSSC_REGEX_COMPILE_TOO_SLOW || rc == PSSC_REGEX_COMPILE_DEFERRED)
		return PSSC_REGEX_COMPILE_UNCHECKED;
	used = cpu_time_ms() - start;
	if (rc == REG_OKAY && !IsUnderPostmaster && pssc_regex_compile_limit_ms > 0 &&
		used > pssc_regex_compile_limit_ms)
		ereport(LOG,
				(errmsg("compiling the pattern of %s took %.0f ms, longer than the %d ms limit",
						what, used, pssc_regex_compile_limit_ms),
				 errdetail("The configuration file's value is used; backends that cannot compile the pattern within the limit disable it.")));
	return rc;
}

/*
 * Compiles pat[0, patlen) into slot, for extractor or rule index (test hook
 * phases ctx_phase and comp_phase). The compiled regex must have between
 * nsub_min and nsub_max capture groups and no back-references. On return
 * the slot is READY or FAILED (also when over the compile time limit), or
 * still EMPTY if an interrupt is pending or interrupts are held off.
 * Interrupts and other errors outside the catch policy's allowlist
 * (may_swallow()) propagate (the slot is then EMPTY and its context gone).
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
		rc = TEST_HOOK(ctx_phase, index);
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
		if (!may_swallow(geterrcode()))
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
			DEBUG_STAT_ADD(compiles, 1);
			DEBUG_STAT_ADD(live, 1);
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
	DEBUG_STAT_ADD(failed, 1);
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
		int			rc = TEST_HOOK(PSSC_REGEX_TEST_EXEC, index);
		size_t		g;
		regoff_t	so;
		regoff_t	eo;

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
		if (!may_swallow(geterrcode()))
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
		int			rc = TEST_HOOK(PSSC_REGEX_TEST_NORM_EXEC, index);
		regoff_t	so;
		regoff_t	eo;

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
		DEBUG_STAT_ADD(failed, 1);
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
			if (!may_swallow(geterrcode()))
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

#ifdef PSSC_TESTING
void
pssc_regex_debug_stats(PsscRegexDebugStats *stats)
{
	*stats = debug_stats;
}
#endif

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
