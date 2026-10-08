/*
 * pssc_extract_test.c
 *		TEST-ONLY module that runs pg_stat_statement_context's tag-set
 *		pipeline (src/extract.h) on given text for test/t/005_extract.pl.
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It reaches the main library's exported
 * functions and variables through load_external_function(), so
 * pg_stat_statement_context must be in shared_preload_libraries.
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <time.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "parser/parser.h"
#include "portability/instr_time.h"
#include "miscadmin.h"
#include "regex/regex.h"
#include "storage/procsignal.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "extract.h"
#include "guc.h"
#include "regex_runtime.h"
#include "scan.h"

/*
 * Sources of this backend's malloc'd bytes in use: mallinfo2() (arrived in
 * glibc 2.33; features.h came in via postgres.h) or, on macOS,
 * malloc_zone_statistics() over all zones.
 */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
#define PSSC_HAVE_MALLINFO2 1
#include <malloc.h>
#endif
#elif defined(__APPLE__)
#define PSSC_HAVE_MALLOC_ZONE 1
#include <malloc/malloc.h>
#endif

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"
#define GUARD_BYTES 64
#define GUARD 0xA5

typedef void (*extract_fn) (const char *, size_t, size_t, char *, size_t,
							PsscExtractResult *);
typedef uint32 (*hash_fn) (const char *, size_t);
typedef void (*take_fn) (PsscTagsetStats *);
typedef void (*set_regex_fn) (PsscRegexExtractFn, void *);

static void *
main_sym(const char *name)
{
	return (void *) load_external_function(MAIN_LIB, name, true, NULL);
}

static int
key_cmp(const char *a, size_t alen, const char *b, size_t blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	if (c != 0)
		return c;
	return alen < blen ? -1 : alen > blen ? 1 : 0;
}

PG_FUNCTION_INFO_V1(pssc_extract_test);
Datum
pssc_extract_test(PG_FUNCTION_ARGS)
{
	bytea	   *q = PG_GETARG_BYTEA_PP(0);
	int			stmt_location = PG_GETARG_INT32(1);
	int			stmt_len = PG_GETARG_INT32(2);
	int			bufsize = PG_GETARG_INT32(3);
	size_t		qlen = VARSIZE_ANY_EXHDR(q);
	char	   *s;
	char	   *buf;
	PsscStmtRange r;
	PsscExtractResult res;
	PsscTagsetStats st = {0};
	extract_fn extract = (extract_fn) main_sym("pssc_extract_tags");
	hash_fn hash = (hash_fn) main_sym("pssc_tagset_hash");
	take_fn take = (take_fn) main_sym("pssc_extract_take_stats");
	int			max_bytes = *(int *) main_sym("pssc_max_tagset_bytes");
	int			max_tags = *(int *) main_sym("pssc_max_tags");
	int			max_value = *(int *) main_sym("pssc_max_tag_value_len");
	int			scan_window = *(int *) main_sym("pssc_scan_window");
	TupleDesc	tupdesc;
	Datum		values[11];
	bool		nulls[11] = {0};
	Datum	   *elems;
	size_t		off;
	int			n;
	const char *prevk = NULL;
	size_t		prevklen = 0;

	if (bufsize < 0)
		elog(ERROR, "bufsize must be >= 0");
	if (stmt_location > (int) qlen || (stmt_location >= 0 && stmt_len > (int) qlen - stmt_location))
		elog(ERROR, "statement out of range");
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	s = palloc(qlen + 1);
	memcpy(s, VARDATA_ANY(q), qlen);
	s[qlen] = '\0';
	r = pssc_stmt_range(s, stmt_location, stmt_len);
	/* pssc_stmt_range uses strlen for "to the end": keep embedded NULs */
	if (stmt_location < 0 || stmt_len <= 0)
		r.end = qlen;
	r.start = pssc_stmt_owned_start(s, 0, r.start, (size_t) scan_window,
									standard_conforming_strings);

	buf = palloc(bufsize + GUARD_BYTES);
	memset(buf, GUARD, bufsize + GUARD_BYTES);

	take(&st);					/* discard earlier counters */
	memset(&st, 0, sizeof(st));
	memset(&res, 0x5A, sizeof(res));
	extract(s, r.start, r.end, buf, (size_t) bufsize, &res);
	take(&st);

	/* invariants */
	for (int i = 0; i < GUARD_BYTES; i++)
		if ((unsigned char) buf[bufsize + i] != GUARD)
			elog(ERROR, "buffer overrun");
	if (res.len > (size_t) Min(bufsize, max_bytes))
		elog(ERROR, "tag set of %zu bytes exceeds limit", res.len);
	if (res.ntags < 0 || res.ntags > max_tags)
		elog(ERROR, "bad ntags %d", res.ntags);
	if (res.oom && (res.len != 0 || res.ntags != 0))
		elog(ERROR, "oom with tags");
	if (res.hash != hash(buf, res.len))
		elog(ERROR, "hash mismatch");

	elems = palloc(sizeof(Datum) * (res.ntags + 1));
	n = 0;
	for (off = 0; off < res.len;)
	{
		const char *k = buf + off;
		size_t		klen = strnlen(k, res.len - off);
		const char *v;
		size_t		vlen;
		char	   *kv;

		if (off + klen >= res.len)
			elog(ERROR, "unterminated key");
		v = k + klen + 1;
		vlen = strnlen(v, res.len - off - klen - 1);
		if (off + klen + 1 + vlen >= res.len)
			elog(ERROR, "unterminated value");
		if (klen == 0 || klen > PSSC_MAX_KEY_LEN)
			elog(ERROR, "bad key length %zu", klen);
		if ((int) vlen > max_value)
			elog(ERROR, "value of %zu bytes exceeds max_tag_value_len", vlen);
		if (!pg_verify_mbstr(GetDatabaseEncoding(), k, (int) klen, true) ||
			!pg_verify_mbstr(GetDatabaseEncoding(), v, (int) vlen, true))
			elog(ERROR, "invalid encoding in tag set");
		if (prevk != NULL && key_cmp(prevk, prevklen, k, klen) >= 0)
			elog(ERROR, "keys not strictly sorted");
		prevk = k;
		prevklen = klen;
		if (n >= res.ntags)
			elog(ERROR, "more tags than ntags");
		kv = psprintf("%s=%s", k, v);
		elems[n++] = PointerGetDatum(cstring_to_text(kv));
		off += klen + vlen + 2;
	}
	if (n != res.ntags)
		elog(ERROR, "ntags %d but %d tags serialized", res.ntags, n);

	values[0] = PointerGetDatum(construct_array(elems, n, TEXTOID, -1, false,
												TYPALIGN_INT));
	{
		bytea	   *b = palloc(VARHDRSZ + res.len);

		SET_VARSIZE(b, VARHDRSZ + res.len);
		memcpy(VARDATA(b), buf, res.len);
		values[1] = PointerGetDatum(b);
	}
	values[2] = Int32GetDatum((int) res.len);
	values[3] = Int64GetDatum((int64) res.hash);
	values[4] = Int32GetDatum(res.ntags);
	values[5] = BoolGetDatum(res.footer);
	values[6] = BoolGetDatum(res.oom);
	values[7] = Int64GetDatum((int64) st.invalid_tags);
	values[8] = Int64GetDatum((int64) st.dropped_tags);
	values[9] = Int64GetDatum((int64) st.heuristic_scans);
	values[10] = Int64GetDatum((int64) st.regex_compile_failures);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_extract_test_hash);
Datum
pssc_extract_test_hash(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	hash_fn		hash = (hash_fn) main_sym("pssc_tagset_hash");

	PG_RETURN_INT64((int64) hash(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)));
}

static int	fake_regex_calls;

static bool
is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void
fake_regex(void *arg, int index, const PsscExtractorList *list,
		   const char *body, size_t len, const PsscPairOut *out,
		   PsscPairResult *result)
{
	const PsscExtractor *e = &list->extractors[index];
	const PsscBlobStr *keys = pssc_extractor_keys(list, e);
	size_t		p = 0;

	Assert(arg == &fake_regex_calls);
	fake_regex_calls++;
	memset(result, 0, sizeof(*result));
	for (uint32 i = 0; i < e->nkeys && result->npairs < out->max_pairs; i++)
	{
		size_t		w;
		PsscPair   *pr;

		while (p < len && is_space(body[p]))
			p++;
		if (p == len)
			break;
		for (w = p; w < len && !is_space(body[w]); w++)
			;
		pr = &out->pairs[result->npairs++];
		pr->key = pssc_blob_str(list, keys[i]);
		pr->keylen = keys[i].len;
		pr->value = body + p;
		pr->valuelen = w - p;
		pr->flags = 0;
		p = w;
	}
}

PG_FUNCTION_INFO_V1(pssc_extract_test_fake_regex);
Datum
pssc_extract_test_fake_regex(PG_FUNCTION_ARGS)
{
	set_regex_fn set = (set_regex_fn) main_sym("pssc_extract_set_regex_hook");

	if (PG_GETARG_BOOL(0))
		set(fake_regex, &fake_regex_calls);
	else
		set((PsscRegexExtractFn) main_sym("pssc_regex_extract"), NULL);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_extract_test_no_regex);
Datum
pssc_extract_test_no_regex(PG_FUNCTION_ARGS)
{
	set_regex_fn set = (set_regex_fn) main_sym("pssc_extract_set_regex_hook");

	set(NULL, NULL);
	PG_RETURN_VOID();
}

/* --- regex runtime fault injection (src/regex_runtime.h) --- */

typedef enum InjectAction
{
	INJ_NONE,
	INJ_ESPACE,					/* return REG_ESPACE (engine out of memory) */
	INJ_ETOOBIG,				/* return REG_ETOOBIG */
	INJ_OOM,					/* throw ERRCODE_OUT_OF_MEMORY */
	INJ_ERROR,					/* elog(ERROR) (internal error) */
	INJ_CANCEL,					/* throw ERRCODE_QUERY_CANCELED */
	INJ_REGCANCEL,				/* PG14/15 engine: cancel pending, REG_CANCEL */
	INJ_SLEEP,					/* busy past the limit in CPU time, then CFI loop */
	INJ_REGSLEEP,				/* as SLEEP, then poll for cancel, REG_CANCEL (PG14/15) */
	INJ_RACESLEEP,				/* SLEEP, the deadline due as it is put off */
	INJ_RACEREGSLEEP,			/* REGSLEEP, the deadline due as it is put off */
	INJ_STALL,					/* CFI loop, sleeping (descheduled, no CPU) */
	INJ_REGSTALL,				/* cancel poll, sleeping; REG_CANCEL */
	INJ_LATEINT,				/* deadline, then a real SIGINT, then CFI */
	INJ_LATEREGINT,				/* deadline, then a real SIGINT, REG_CANCEL */
	INJ_LATEWAIT,				/* deadline, then wait 1 s without CFI, then CFI */
	INJ_LATECONFLICT,			/* deadline, then a recovery conflict, then CFI */
	INJ_LATEREGCONFLICT,		/* deadline, then a recovery conflict, REG_CANCEL */
	INJ_EXPIRE,					/* let the engine run; the limit expires mid-compile,
								 * after inj_expire_ms of CPU time */
	INJ_EXPIREAFTER,			/* the limit expires once the engine has returned */
	INJ_LIMIT,					/* throw ERRCODE_PROGRAM_LIMIT_EXCEEDED */
	INJ_REGERR,					/* throw ERRCODE_INVALID_REGULAR_EXPRESSION */
	INJ_SELFINT,				/* a real SIGINT, then let the engine run */
	INJ_LATEERROR,				/* deadline, then an internal error, no CFI */
	INJ_LATEINTERROR			/* deadline, a real SIGINT, an internal error */
} InjectAction;

static int	inj_phase = -1;
static int	inj_index = -1;
static InjectAction inj_action = INJ_NONE;
static int	inj_remaining = 0;	/* -1: unlimited */
static int	inj_fired = 0;
static int	inj_attempts = 0;	/* hook calls for the phase/index, fired or not */
static int	inj_engine_entered = 0; /* of those, engine calls let run */
static int	inj_engine_ok = 0;	/* engine calls that returned REG_OKAY */
static bool inj_expire_after = false;	/* INJ_EXPIREAFTER fired, engine running */
static instr_time inj_engine_start;	/* the engine call let run last */
static double inj_engine_cpu_start;
static double inj_engine_ms = -1;	/* wall time of the last that completed */
static double inj_engine_cpu_ms = -1;	/* and its CPU time */

/* REG_CANCEL of the PG14/15 engine (21); PG16+ throws instead. */
#define PSSC_TEST_REG_CANCEL 21

/*
 * INJ_EXPIRE: the compile time limit expires once the engine has used this
 * much CPU time (pssc_extract_test_regex_expire_ms()).
 */
static int	inj_expire_ms = 300;

static double
cpu_ms(void)
{
#ifdef CLOCK_PROCESS_CPUTIME_ID
	struct timespec ts;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
		return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
	return GetCurrentTimestamp() / 1000.0;
}

/*
 * INJ_EXPIRE's timer. Interrupting the engine a given wall-clock time into
 * the compile, calibrated from a clean compile's wall time, is not
 * reproducible on a loaded host: a calibration slowed by other processes
 * puts the expiry past the end of a later, faster compile (and the reverse
 * puts it near the start). So the expiry is in the backend's CPU time
 * (ITIMER_PROF: user and system time of the process), i.e. after a given
 * share of the engine's work, whatever else runs on the host. Its SIGPROF
 * handler makes the armed limit expire right away, as the limit's own
 * timer would (pssc_regex_test_expire_now() only sets flags). Armed right
 * before the engine runs, disarmed when it returns or at the next
 * injection.
 */
static void (*cpu_expire_now) (void) = NULL;

static void
cpu_expiry_handler(SIGNAL_ARGS)
{
	if (cpu_expire_now != NULL)
		cpu_expire_now();
}

static void
cpu_expiry_set(int ms)
{
#ifndef WIN32
	struct itimerval it;

	memset(&it, 0, sizeof(it));
	if (ms > 0)
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = cpu_expiry_handler;
		sa.sa_flags = SA_RESTART;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGPROF, &sa, NULL) != 0)
			elog(ERROR, "could not set the SIGPROF handler: %m");
		cpu_expire_now = (void (*) (void)) main_sym("pssc_regex_test_expire_now");
		it.it_value.tv_sec = ms / 1000;
		it.it_value.tv_usec = (ms % 1000) * 1000;
	}
	if (setitimer(ITIMER_PROF, &it, NULL) != 0)
		elog(ERROR, "could not set the CPU-time timer: %m");
#endif
}

/*
 * INJ_SLEEP / INJ_REGSLEEP stand for a compile that is busy past the limit,
 * which must not be retried as a stall. On a loaded host a busy loop can get
 * less than half of the 100 ms wall-clock limit in CPU time and so would be
 * retried. Instead, the deadline is put off until this attempt has used the
 * whole limit in CPU time, and is then made to expire. A cancel already
 * pending (the deadline fired before this hook ran) is left alone; it is
 * raised once the CPU time is spent. With no limit (0) nothing is put off.
 *
 * While the deadline is put off, any cancel that arrives is a genuine one
 * (client cancel, statement_timeout), so it, or a termination, ends the
 * spinning at once and the caller's loop raises it. The runtime holds a
 * genuine SIGINT pending in the signal mask while the limit is armed; then
 * the deadline is let expire at once, as it would for a real compile, and
 * the runtime raises the genuine cancel. If the CPU time hasn't
 * been used after BUSY_WALL_LIMIT_MS of wall-clock time (a starved host),
 * the injection gives up with a WARNING (the test then fails visibly) and
 * lets the deadline expire.
 *
 * With race (racesleep / raceregsleep) the deadline is made due right
 * between seeing no cancel pending and putting it off, which SIGALRM being
 * blocked there must survive.
 */
#define BUSY_WALL_LIMIT_MS 5000

/*
 * Test hook for the racesleep / raceregsleep injections: makes the deadline
 * due right after busy_past_limit() saw no cancel pending, before it puts
 * the deadline off. Returns once the deadline's SIGALRM has been handled
 * (QueryCancelPending) or is held pending by the signal mask, or after
 * BUSY_WALL_LIMIT_MS.
 */
static void
deadline_due_now(void (*expire) (int))
{
	expire(1);
	for (int i = 0; i < BUSY_WALL_LIMIT_MS && !QueryCancelPending; i++)
	{
#ifndef WIN32
		sigset_t	pending;

		if (sigpending(&pending) == 0 && sigismember(&pending, SIGALRM))
			return;
#endif
		pg_usleep(1000L);
	}
}

/*
 * A genuine cancel (SIGINT: a client cancel, statement_timeout) that the
 * regex runtime holds pending in the signal mask while the compile time
 * limit is armed.
 */
static bool
sigint_held(void)
{
#ifndef WIN32
	sigset_t	pending;

	return sigpending(&pending) == 0 && sigismember(&pending, SIGINT);
#else
	return false;
#endif
}

static void
busy_past_limit(bool race)
{
	int			limit = *(int *) main_sym("pssc_regex_compile_limit_ms");
	void		(*expire) (int) = (void (*) (int)) main_sym("pssc_regex_test_expire_in");
	bool		was_pending;
	double		start = cpu_ms();
	instr_time	wstart;
	instr_time	now;
#ifndef WIN32
	sigset_t	block;
	sigset_t	old;
#endif

	if (limit <= 0)
		return;
	INSTR_TIME_SET_CURRENT(wstart);

	/*
	 * The deadline (SIGALRM) must not fire between seeing no cancel pending
	 * and putting it off, or its cancel would be taken for a genuine one. If
	 * it falls due meanwhile, the signal stays pending until it is put off;
	 * the timeout handler then finds nothing due.
	 */
#ifndef WIN32
	sigemptyset(&block);
	sigaddset(&block, SIGALRM);
	sigprocmask(SIG_BLOCK, &block, &old);
#endif
	was_pending = QueryCancelPending;
	if (race && !was_pending)
		deadline_due_now(expire);
	if (!was_pending)
		expire(60000);
#ifndef WIN32
	sigprocmask(SIG_SETMASK, &old, NULL);
#endif
	while (cpu_ms() - start < limit)
	{
		if (ProcDiePending || (!was_pending && QueryCancelPending))
			return;
		if (sigint_held())
			break;				/* the deadline then lets the runtime see it */
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, wstart);
		if (INSTR_TIME_GET_MILLISEC(now) >= BUSY_WALL_LIMIT_MS)
		{
			ereport(WARNING,
					(errmsg("pssc_extract_test: busy injection gave up after %d s with %.0f ms of CPU time, short of the %d ms limit",
							BUSY_WALL_LIMIT_MS / 1000, cpu_ms() - start, limit)));
			break;
		}
	}
	if (!QueryCancelPending)
		expire(1);
}

static int	inject_action(void);

/*
 * PSSC_REGEX_TEST_COMPILE_CATCH: throw out of memory this many more times
 * (-1: always); pssc_extract_test_regex_catch_oom().
 */
static int	inj_catch_oom = 0;
static int	inj_catch_fired = 0;

static int
inject_hook(int phase, int index)
{
	int			rc = REG_OKAY;

	if (phase == PSSC_REGEX_TEST_COMPILE_CATCH)
	{
		if (inj_catch_oom != 0)
		{
			if (inj_catch_oom > 0)
				inj_catch_oom--;
			inj_catch_fired++;
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("pssc_extract_test injected failure while handling an error.")));
		}
		return REG_OKAY;
	}
	if (phase != inj_phase || (inj_index >= 0 && index != inj_index))
		return REG_OKAY;
	inj_attempts++;
	if (inj_remaining != 0)
		rc = inject_action();
	if (rc == REG_OKAY)
	{
		inj_engine_entered++;
		inj_engine_cpu_start = cpu_ms();
		INSTR_TIME_SET_CURRENT(inj_engine_start);
	}
	return rc;
}

/*
 * The engine returned (it didn't throw). INJ_EXPIREAFTER: the compile time
 * limit expires now, after the engine completed but before it is disarmed,
 * as when the backend is descheduled right after the engine returns.
 */
static void
inject_engine_hook(int phase, int index, int rc)
{
	if (phase != inj_phase || (inj_index >= 0 && index != inj_index))
		return;
	if (rc == REG_OKAY)
	{
		instr_time	now;

		INSTR_TIME_SET_CURRENT(now);
		inj_engine_cpu_ms = cpu_ms() - inj_engine_cpu_start;
		INSTR_TIME_SUBTRACT(now, inj_engine_start);
		inj_engine_ms = INSTR_TIME_GET_MILLISEC(now);
		inj_engine_ok++;
	}
	if (inj_action == INJ_EXPIRE)
		cpu_expiry_set(0);
	if (inj_expire_after)
	{
		void		(*expire) (int) = (void (*) (int)) main_sym("pssc_regex_test_expire_in");

		inj_expire_after = false;
		expire(1);
		for (int i = 0; i < 5000 && !QueryCancelPending; i++)
			pg_usleep(1000L);
		if (!QueryCancelPending)
			ereport(WARNING,
					(errmsg("pssc_extract_test: the compile time limit did not expire after the engine returned")));
	}
}

static int
inject_action(void)
{
	if (inj_remaining > 0)
		inj_remaining--;
	inj_fired++;
	switch (inj_action)
	{
		case INJ_NONE:
			return REG_OKAY;
		case INJ_ESPACE:
			return REG_ESPACE;
		case INJ_ETOOBIG:
			return REG_ETOOBIG;
		case INJ_OOM:
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("pssc_extract_test injected failure.")));
			break;
		case INJ_ERROR:
			elog(ERROR, "pssc_extract_test injected internal error");
			break;
		case INJ_LIMIT:
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pssc_extract_test injected program limit exceeded")));
			break;
		case INJ_REGERR:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("pssc_extract_test injected invalid regular expression")));
			break;
		case INJ_CANCEL:
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
			break;
		case INJ_REGCANCEL:
			QueryCancelPending = true;
			InterruptPending = true;
			return PSSC_TEST_REG_CANCEL;
		case INJ_SLEEP:
		case INJ_REGSLEEP:
		case INJ_RACESLEEP:
		case INJ_RACEREGSLEEP:
			busy_past_limit(inj_action == INJ_RACESLEEP ||
							inj_action == INJ_RACEREGSLEEP);
			/* FALLTHROUGH */
		case INJ_STALL:
		case INJ_REGSTALL:
			{
				/* up to 60 s, using CPU (as a slow compile) or not */
				TimestampTz end = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 60000);
				bool		reg = inj_action == INJ_REGSLEEP || inj_action == INJ_RACEREGSLEEP ||
					inj_action == INJ_REGSTALL;
				bool		busy = inj_action != INJ_STALL && inj_action != INJ_REGSTALL;

				while (GetCurrentTimestamp() < end)
				{
					/* like the PG14/15 engine's rcancelrequested() polling */
					if (reg && InterruptPending && (QueryCancelPending || ProcDiePending))
						return PSSC_TEST_REG_CANCEL;
					if (!reg)
						CHECK_FOR_INTERRUPTS();
					if (!busy)
						pg_usleep(10000L);
				}
				return REG_OKAY;
			}
		case INJ_LATEINT:
		case INJ_LATEREGINT:
		case INJ_LATEWAIT:
		case INJ_LATECONFLICT:
		case INJ_LATEREGCONFLICT:

			/*
			 * Wait, without processing interrupts, until a cancel is pending
			 * (the compile time limit sets one), then let a genuine cancel
			 * arrive: a real SIGINT (as from pg_cancel_backend), or for
			 * LATEWAIT whatever comes within 1 s (a statement_timeout).
			 */
			for (int i = 0; i < 6000 && !QueryCancelPending; i++)
				pg_usleep(10000L);
			if (inj_action == INJ_LATEWAIT)
			{
				for (int i = 0; i < 100; i++)
					pg_usleep(10000L);
			}
			else if (inj_action == INJ_LATECONFLICT ||
					 inj_action == INJ_LATEREGCONFLICT)
			{
				/*
				 * A real recovery conflict (SIGUSR1, not SIGINT). It also
				 * cancels the statement of a primary in a transaction. -1:
				 * InvalidBackendId / INVALID_PROC_NUMBER (search by pid).
				 */
				if (SendProcSignal(MyProcPid, PROCSIG_RECOVERY_CONFLICT_SNAPSHOT, -1) != 0)
					elog(ERROR, "could not send a recovery conflict signal");
			}
			else
				kill(MyProcPid, SIGINT);
			if (inj_action == INJ_LATEREGINT || inj_action == INJ_LATEREGCONFLICT)
				return PSSC_TEST_REG_CANCEL;
			CHECK_FOR_INTERRUPTS();
			return REG_OKAY;
		case INJ_EXPIRE:
			cpu_expiry_set(inj_expire_ms);
			return REG_OKAY;
		case INJ_EXPIREAFTER:
			inj_expire_after = true;
			return REG_OKAY;
		case INJ_SELFINT:
			kill(MyProcPid, SIGINT);
			return REG_OKAY;
		case INJ_LATEERROR:
		case INJ_LATEINTERROR:
			for (int i = 0; i < 6000 && !QueryCancelPending; i++)
				pg_usleep(10000L);
			if (inj_action == INJ_LATEINTERROR)
				kill(MyProcPid, SIGINT);
			elog(ERROR, "pssc_extract_test injected internal error");
			break;
	}
	return REG_OKAY;
}

PG_FUNCTION_INFO_V1(pssc_extract_test_regex_inject);
Datum
pssc_extract_test_regex_inject(PG_FUNCTION_ARGS)
{
	char	   *phase = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			index = PG_GETARG_INT32(1);
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int			count = PG_GETARG_INT32(3);
	PsscRegexTestHook *hook = (PsscRegexTestHook *) main_sym("pssc_regex_test_hook");
	PsscRegexTestEngineHook *ehook = (PsscRegexTestEngineHook *) main_sym("pssc_regex_test_engine_hook");
	static const char *const names[] = {
		[INJ_NONE] = "none", [INJ_ESPACE] = "espace", [INJ_ETOOBIG] = "etoobig",
		[INJ_OOM] = "oom", [INJ_ERROR] = "error", [INJ_CANCEL] = "cancel",
		[INJ_REGCANCEL] = "regcancel", [INJ_SLEEP] = "sleep",
		[INJ_REGSLEEP] = "regsleep", [INJ_RACESLEEP] = "racesleep",
		[INJ_RACEREGSLEEP] = "raceregsleep", [INJ_STALL] = "stall",
		[INJ_REGSTALL] = "regstall", [INJ_LATEINT] = "lateint",
		[INJ_LATEREGINT] = "lateregint", [INJ_LATEWAIT] = "latewait",
		[INJ_LATECONFLICT] = "lateconflict", [INJ_LATEREGCONFLICT] = "lateregconflict",
		[INJ_EXPIRE] = "expire", [INJ_EXPIREAFTER] = "expireafter",
		[INJ_LIMIT] = "limit", [INJ_REGERR] = "regerr", [INJ_SELFINT] = "selfint",
		[INJ_LATEERROR] = "lateerror", [INJ_LATEINTERROR] = "lateinterror",
	};
	int			a = -1;

	for (int i = 0; i < (int) lengthof(names); i++)
		if (strcmp(action, names[i]) == 0)
			a = i;
	if (a < 0)
		elog(ERROR, "unknown action \"%s\"", action);
	if (strcmp(phase, "compile") == 0)
		inj_phase = PSSC_REGEX_TEST_COMPILE;
	else if (strcmp(phase, "exec") == 0)
		inj_phase = PSSC_REGEX_TEST_EXEC;
	else if (strcmp(phase, "context") == 0)
		inj_phase = PSSC_REGEX_TEST_CONTEXT;
	else if (strcmp(phase, "norm_context") == 0)
		inj_phase = PSSC_REGEX_TEST_NORM_CONTEXT;
	else if (strcmp(phase, "norm_compile") == 0)
		inj_phase = PSSC_REGEX_TEST_NORM_COMPILE;
	else if (strcmp(phase, "norm_exec") == 0)
		inj_phase = PSSC_REGEX_TEST_NORM_EXEC;
	else if (strcmp(phase, "check") == 0)
		inj_phase = PSSC_REGEX_TEST_CHECK;
	else
		elog(ERROR, "unknown phase \"%s\"", phase);
	inj_index = index;
	inj_action = (InjectAction) a;
	inj_remaining = count;
	inj_fired = 0;
	inj_attempts = 0;
	inj_engine_entered = 0;
	inj_engine_ok = 0;
	inj_expire_after = false;
	inj_engine_ms = -1;
	inj_engine_cpu_ms = -1;
	cpu_expiry_set(0);
	*hook = a == INJ_NONE ? NULL : inject_hook;
	*ehook = a == INJ_NONE ? NULL : inject_engine_hook;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_extract_test_regex_injected);
Datum
pssc_extract_test_regex_injected(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(inj_fired);
}

/*
 * How many times the hook ran for the injected phase (and index) since the
 * injection was set, whether it fired or not. For a compile phase that is
 * one per attempt, so 2 after one firing means the attempt it fired in was
 * stopped and retried.
 */
PG_FUNCTION_INFO_V1(pssc_extract_test_regex_attempts);
Datum
pssc_extract_test_regex_attempts(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(inj_attempts);
}

/*
 * Of those attempts, how many engine calls were interrupted: let run, but
 * did not complete (they threw, as PG16+ on a cancel, or returned an error,
 * as REG_CANCEL from PG14/15).
 */
PG_FUNCTION_INFO_V1(pssc_extract_test_regex_interrupted);
Datum
pssc_extract_test_regex_interrupted(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(inj_engine_entered - inj_engine_ok);
}

/*
 * Wall-clock time, in ms, of the last engine call for the injected phase
 * (and index) that completed, measured in this backend from right before
 * the engine was entered to its return; NULL if none completed since the
 * injection was set. Unlike a client's round trip, it does not include
 * statement processing, IPC or the client being descheduled.
 */
PG_FUNCTION_INFO_V1(pssc_extract_test_regex_engine_ms);
Datum
pssc_extract_test_regex_engine_ms(PG_FUNCTION_ARGS)
{
	if (inj_engine_ms < 0)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(inj_engine_ms);
}

/* The same engine call's CPU time, in ms; NULL as above. */
PG_FUNCTION_INFO_V1(pssc_extract_test_regex_engine_cpu_ms);
Datum
pssc_extract_test_regex_engine_cpu_ms(PG_FUNCTION_ARGS)
{
	if (inj_engine_ms < 0)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(inj_engine_cpu_ms);
}

PG_FUNCTION_INFO_V1(pssc_extract_test_regex_expire_ms);
Datum
pssc_extract_test_regex_expire_ms(PG_FUNCTION_ARGS)
{
	int			old = inj_expire_ms;

	if (PG_GETARG_INT32(0) < 1)
		elog(ERROR, "the expiry must be at least 1 ms");
	inj_expire_ms = PG_GETARG_INT32(0);
	PG_RETURN_INT32(old);
}

/*
 * Makes the next count steps of a compile attempt's error handling that
 * allocate (PSSC_REGEX_TEST_COMPILE_CATCH) fail with out of memory, while
 * an injection is set. Returns how many failed since the previous call.
 */
PG_FUNCTION_INFO_V1(pssc_extract_test_regex_catch_oom);
Datum
pssc_extract_test_regex_catch_oom(PG_FUNCTION_ARGS)
{
	int			fired = inj_catch_fired;

	inj_catch_oom = PG_GETARG_INT32(0);
	inj_catch_fired = 0;
	PG_RETURN_INT32(fired);
}

PG_FUNCTION_INFO_V1(pssc_extract_test_regex_compile_limit);
Datum
pssc_extract_test_regex_compile_limit(PG_FUNCTION_ARGS)
{
	int		   *limit = (int *) main_sym("pssc_regex_compile_limit_ms");
	int			old = *limit;

	*limit = PG_GETARG_INT32(0);
	PG_RETURN_INT32(old);
}

PG_FUNCTION_INFO_V1(pssc_extract_test_set_local);
Datum
pssc_extract_test_set_local(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *value = text_to_cstring(PG_GETARG_TEXT_PP(1));

	(void) set_config_option(name, value, PGC_SIGHUP, PGC_S_SESSION,
							 GUC_ACTION_SET, true, 0, false);
	PG_RETURN_VOID();
}

typedef void (*debug_fn) (PsscRegexDebugStats *);

PG_FUNCTION_INFO_V1(pssc_extract_test_regex_stats);
Datum
pssc_extract_test_regex_stats(PG_FUNCTION_ARGS)
{
	debug_fn	get = (debug_fn) main_sym("pssc_regex_debug_stats");
	PsscRegexDebugStats st;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	memset(&st, 0, sizeof(st));
	get(&st);
	values[0] = Int64GetDatum((int64) st.compiles);
	values[1] = Int64GetDatum((int64) st.frees);
	values[2] = Int32GetDatum(st.live);
	values[3] = Int32GetDatum(st.failed);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Memory held by this backend: malloc'd bytes in use (glibc >= 2.33 and
 * macOS, NULL elsewhere; the regex engine mallocs on PG14/15) and bytes
 * allocated by all memory contexts (it pallocs on PG16+).
 */
PG_FUNCTION_INFO_V1(pssc_extract_test_mem);
Datum
pssc_extract_test_mem(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
#if defined(PSSC_HAVE_MALLINFO2)
	{
		struct mallinfo2 mi = mallinfo2();

		values[0] = Int64GetDatum((int64) (mi.uordblks + mi.hblkhd));
	}
#elif defined(PSSC_HAVE_MALLOC_ZONE)
	{
		malloc_statistics_t st;

		malloc_zone_statistics(NULL, &st);
		values[0] = Int64GetDatum((int64) st.size_in_use);
	}
#else
	nulls[0] = true;
#endif
	values[1] = Int64GetDatum((int64) MemoryContextMemAllocated(TopMemoryContext, true));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_extract_test_malloc_hold);
Datum
pssc_extract_test_malloc_hold(PG_FUNCTION_ARGS)
{
	static char *held;
	int64		bytes = PG_GETARG_INT64(0);

	free(held);
	held = NULL;
	if (bytes < 0 || bytes > (int64) 1 << 30)
		elog(ERROR, "malloc_hold: %lld bytes out of range", (long long) bytes);
	if (bytes > 0)
	{
		held = malloc((size_t) bytes);
		if (held == NULL)
			elog(ERROR, "malloc_hold: out of memory");
		memset(held, 0x5A, (size_t) bytes);
	}
	PG_RETURN_VOID();
}
