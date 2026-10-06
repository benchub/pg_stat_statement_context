/*
 * pssc_compat_test.c
 *		TEST-ONLY module that exercises every shim in src/compat.h (and the
 *		version-sensitive ms conversions of src/counters.h).
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It exists so test/t/002_compat.pl can check,
 * on each supported PostgreSQL major version, that each compat.h shim
 * compiles warning-free *and* behaves as the version requires (for example,
 * that the GUC extra allocator matches how guc.c frees it, or that the regex
 * shim compiles into the caller's context where it can). It must be preloaded via
 * shared_preload_libraries.
 *
 * It deliberately contains no version checks: all version knowledge
 * lives in compat.h, and the TAP test derives its expectations from
 * server_version_num independently.
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "compat.h"
#include "counters.h"

PG_MODULE_MAGIC;

#define TEST_PREFIX "pssc_compat_test"

/* Larger than the ~100kB slack the server adds, so a missing request fails. */
#define TEST_SHMEM_SIZE ((Size) 1024 * 1024)

/* Big enough to show up clearly in pg_backend_memory_contexts. */
#define TEST_GUC_EXTRA_SIZE ((Size) 256 * 1024)

typedef struct TestShared
{
	LWLock	   *lock;
	int64		counter;
} TestShared;

void		_PG_init(void);

static TestShared *test_shared = NULL;

static pssc_shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static int64 executor_runs = 0;
static int64 utility_calls = 0;

/* pssc_max_backends_for_shmem() in the shared memory request function */
static int	backends_at_request = -1;

static char *guc_extra_value = NULL;
static const char *guc_extra_current = NULL;

/* ---- shared memory request shim ---- */

static void
test_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(TEST_SHMEM_SIZE);
	RequestNamedLWLockTranche(TEST_PREFIX, 1);
	backends_at_request = pssc_max_backends_for_shmem();
}

static void
test_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	test_shared = ShmemInitStruct(TEST_PREFIX, TEST_SHMEM_SIZE, &found);
	if (!found)
	{
		test_shared->lock = &(GetNamedLWLockTranche(TEST_PREFIX))->lock;
		test_shared->counter = 0;
	}
	LWLockRelease(AddinShmemInitLock);
}

/* ---- ExecutorRun / ProcessUtility shims ---- */

static void
test_ExecutorRun(PSSC_EXECUTOR_RUN_PARAMS)
{
	executor_runs++;
	if (prev_ExecutorRun)
		prev_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
	else
		standard_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
}

static void
test_ProcessUtility(PSSC_PROCESS_UTILITY_PARAMS)
{
	utility_calls++;
	if (prev_ProcessUtility)
		prev_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
	else
		standard_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
}

/* ---- GUC extra allocator shim ---- */

/*
 * Stores an upper-cased copy of the value at the start of a large extra blob,
 * so the test can tell the value came through extra and can see where the
 * blob was allocated.
 */
static bool
test_check_extra(char **newval, void **extra, GucSource source)
{
	const char *val = *newval ? *newval : "";
	size_t		len = strlen(val);
	char	   *blob;
	size_t		i;

	if (len >= TEST_GUC_EXTRA_SIZE)
	{
		GUC_check_errdetail("Value is too long.");
		return false;
	}

	blob = pssc_guc_extra_alloc(TEST_GUC_EXTRA_SIZE);
	if (blob == NULL)
		return false;

	for (i = 0; i < len; i++)
		blob[i] = pg_toupper((unsigned char) val[i]);
	blob[len] = '\0';

	*extra = blob;
	return true;
}

static void
test_assign_extra(const char *newval, void *extra)
{
	guc_extra_current = (const char *) extra;
}

/* ---- module init ---- */

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomStringVariable(TEST_PREFIX ".extra",
							   "TEST-ONLY: exercises the GUC extra allocator shim.",
							   NULL,
							   &guc_extra_value,
							   "",
							   PGC_USERSET,
							   0,
							   test_check_extra,
							   test_assign_extra,
							   NULL);

	PSSC_MARK_GUC_PREFIX_RESERVED(TEST_PREFIX);

	PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev_shmem_request_hook, test_shmem_request);
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_shmem_startup;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = test_ExecutorRun;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = test_ProcessUtility;
}

/* ---- SQL-callable functions ---- */

PG_FUNCTION_INFO_V1(pssc_compat_test_shmem_bump);
Datum
pssc_compat_test_shmem_bump(PG_FUNCTION_ARGS)
{
	int64		val;

	if (test_shared == NULL)
		ereport(ERROR,
				(errmsg("pssc_compat_test must be loaded via shared_preload_libraries")));

	LWLockAcquire(test_shared->lock, LW_EXCLUSIVE);
	val = ++test_shared->counter;
	LWLockRelease(test_shared->lock);

	PG_RETURN_INT64(val);
}

PG_FUNCTION_INFO_V1(pssc_compat_test_reset);
Datum
pssc_compat_test_reset(PG_FUNCTION_ARGS)
{
	executor_runs = 0;
	utility_calls = 0;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pssc_compat_test_stats);
Datum
pssc_compat_test_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values[0] = Int64GetDatum(executor_runs);
	values[1] = Int64GetDatum(utility_calls);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_compat_test_guc_extra);
Datum
pssc_compat_test_guc_extra(PG_FUNCTION_ARGS)
{
	if (guc_extra_current == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(guc_extra_current));
}

/*
 * Compiles the pattern with the regex shim into a private context and reports
 * whether the compiled regex consumed memory from that context.
 */
PG_FUNCTION_INFO_V1(pssc_compat_test_regex);
Datum
pssc_compat_test_regex(PG_FUNCTION_ARGS)
{
	text	   *pattern = PG_GETARG_TEXT_PP(0);
	text	   *subject = PG_GETARG_TEXT_PP(1);
	MemoryContext regex_cxt;
	Size		before,
				after;
	regex_t		re;
	pg_wchar   *wpat;
	pg_wchar   *wsub;
	int			wpat_len;
	int			wsub_len;
	int			rc;
	regmatch_t	match[2];
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	wpat = palloc((VARSIZE_ANY_EXHDR(pattern) + 1) * sizeof(pg_wchar));
	wpat_len = pg_mb2wchar_with_len(VARDATA_ANY(pattern), wpat,
									VARSIZE_ANY_EXHDR(pattern));
	wsub = palloc((VARSIZE_ANY_EXHDR(subject) + 1) * sizeof(pg_wchar));
	wsub_len = pg_mb2wchar_with_len(VARDATA_ANY(subject), wsub,
									VARSIZE_ANY_EXHDR(subject));

	regex_cxt = AllocSetContextCreate(CurrentMemoryContext,
									  "pssc_compat_test regex",
									  ALLOCSET_SMALL_SIZES);
	before = MemoryContextMemAllocated(regex_cxt, true);

	rc = pssc_regcomp(regex_cxt, &re, wpat, wpat_len,
					  REG_ADVANCED, C_COLLATION_OID);
	if (rc != REG_OKAY)
	{
		char		errstr[100];

		pg_regerror(rc, &re, errstr, sizeof(errstr));
		MemoryContextDelete(regex_cxt);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression: %s", errstr)));
	}
	after = MemoryContextMemAllocated(regex_cxt, true);

	rc = pg_regexec(&re, wsub, wsub_len, 0, NULL, lengthof(match), match, 0);
	values[0] = BoolGetDatum(rc == REG_OKAY);
	if (rc == REG_OKAY && re.re_nsub >= 1 && match[1].rm_so >= 0)
	{
		char	   *buf = palloc(pg_database_encoding_max_length() *
								 (match[1].rm_eo - match[1].rm_so) + 1);

		pg_wchar2mb_with_len(wsub + match[1].rm_so, buf,
							 match[1].rm_eo - match[1].rm_so);
		values[1] = CStringGetTextDatum(buf);
	}
	else
		nulls[1] = true;
	values[2] = BoolGetDatum(after > before);

	if (re.re_magic == 0 || re.re_guts == NULL)
		elog(ERROR, "compiled regex is not valid before pssc_regfree");
	pssc_regfree(&re);
	/* pg_regfree invalidates the handle and drops its internals. */
	values[3] = BoolGetDatum(re.re_magic == 0 && re.re_guts == NULL && re.re_fns == NULL);
	MemoryContextDelete(regex_cxt);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* Composite (OUT-parameter) SRF. */
PG_FUNCTION_INFO_V1(pssc_compat_test_srf);
Datum
pssc_compat_test_srf(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	bool		use_expected_desc = PG_GETARG_BOOL(1);
	ReturnSetInfo *rsinfo;
	int32		i;

	pssc_init_materialized_srf(fcinfo,
							   use_expected_desc ? PSSC_MAT_SRF_USE_EXPECTED_DESC : 0);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	for (i = 1; i <= n; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = Int32GetDatum(i);
		values[1] = CStringGetTextDatum(psprintf("row %d", i));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * Scalar SRF: get_call_result_type cannot describe it, so it only works with
 * the USE_EXPECTED_DESC flag. The caller's expected descriptor is unblessed,
 * so the BLESS flag is observable: each value is i * 10, plus 1 if setDesc
 * has a registered record typmod.
 */
PG_FUNCTION_INFO_V1(pssc_compat_test_srf_scalar);
Datum
pssc_compat_test_srf_scalar(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	bool		bless = PG_GETARG_BOOL(1);
	ReturnSetInfo *rsinfo;
	int32		blessed;
	int32		i;

	pssc_init_materialized_srf(fcinfo, PSSC_MAT_SRF_USE_EXPECTED_DESC |
							   (bless ? PSSC_MAT_SRF_BLESS : 0));
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	blessed = rsinfo->setDesc->tdtypmod >= 0 ? 1 : 0;

	for (i = 1; i <= n; i++)
	{
		Datum		value = Int32GetDatum(i * 10 + blessed);
		bool		isnull = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &value, &isnull);
	}

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pssc_compat_test_srf_direct);
Datum
pssc_compat_test_srf_direct(PG_FUNCTION_ARGS)
{
	DirectFunctionCall2(pssc_compat_test_srf, Int32GetDatum(1), BoolGetDatum(false));
	PG_RETURN_VOID();
}

/*
 * src/counters.h backend conversions (not compat.h shims, but
 * version-sensitive: instr_time changed representation in PG16). Times a
 * sleep the way the executor and pgss do and returns, in ms:
 *	pre_total	Instrumentation.total before InstrEndLoop (0: the loop's time
 *				is still in the counter, so the helper must end the loop)
 *	exec_ms		pssc_exec_ms_from_totaltime()
 *	exec_again	the same, called again (a later hook ending the loop again)
 *	exec_pgss	pgss_ExecutorEnd's expression, totaltime->total * 1000.0
 *	util_ms		pssc_ms_from_instr_time() of a measured duration
 *	util_pgss	pgss_ProcessUtility's INSTR_TIME_GET_MILLISEC(duration)
 */
PG_FUNCTION_INFO_V1(pssc_compat_test_counters_ms);
Datum
pssc_compat_test_counters_ms(PG_FUNCTION_ARGS)
{
	int32		sleep_ms = PG_GETARG_INT32(0);
	Instrumentation *instr = InstrAlloc(1, INSTRUMENT_TIMER, false);
	instr_time	start,
				duration;
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	InstrStartNode(instr);
	pg_usleep(sleep_ms * 1000L);
	InstrStopNode(instr, 1);
	values[0] = Float8GetDatum(instr->total * 1000.0);
	values[1] = Float8GetDatum(pssc_exec_ms_from_totaltime(instr));
	values[2] = Float8GetDatum(pssc_exec_ms_from_totaltime(instr));
	values[3] = Float8GetDatum(instr->total * 1000.0);

	INSTR_TIME_SET_CURRENT(start);
	pg_usleep(sleep_ms * 1000L);
	INSTR_TIME_SET_CURRENT(duration);
	INSTR_TIME_SUBTRACT(duration, start);
	values[4] = Float8GetDatum(pssc_ms_from_instr_time(duration));
	values[5] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(duration));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* ---- per-backend shared memory: backend count and slot index ---- */

/*
 * at_request	pssc_max_backends_for_shmem() in the shared memory request
 *				function (PG14: _PG_init, before MaxBackends is set)
 * max_backends	MaxBackends in this backend
 * slot			PSSC_MY_BACKEND_SLOT()
 */
PG_FUNCTION_INFO_V1(pssc_compat_test_backends);
Datum
pssc_compat_test_backends(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int32GetDatum(backends_at_request);
	values[1] = Int32GetDatum(MaxBackends);
	values[2] = Int32GetDatum(PSSC_MY_BACKEND_SLOT());
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
