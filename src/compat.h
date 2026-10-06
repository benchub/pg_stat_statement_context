/*
 * compat.h
 *		PostgreSQL 14-18 version shims for pg_stat_statement_context.
 *
 * Version-dependent code belongs here rather than in scattered
 * PG_VERSION_NUM checks (DESIGN.md §6.10). scripts/check-version-guards.sh
 * rejects PG_VERSION_NUM anywhere else unless a "version-guard-ok:" comment
 * justifies it. Every shim is exercised on each supported version by the
 * TEST-ONLY module test/modules/pssc_compat_test (test/t/002_compat.pl).
 */
#ifndef PSSC_COMPAT_H
#define PSSC_COMPAT_H

#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "regex/regex.h"
#include "storage/ipc.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

/* PG16+: queryjumble.h moved from utils/ to nodes/ (PG14/15: utils/). */
#if PG_VERSION_NUM >= 160000
#include "nodes/queryjumble.h"
#else
#include "utils/queryjumble.h"
#endif

/*
 * ExecutorRun hook signature. Declare a hook as
 *		static void my_ExecutorRun(PSSC_EXECUTOR_RUN_PARAMS)
 * and chain with prev(PSSC_EXECUTOR_RUN_ARGS) or
 * standard_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS).
 */
/* PG14-17: trailing bool execute_once; PG18: removed. */
#if PG_VERSION_NUM >= 180000
#define PSSC_EXECUTOR_RUN_PARAMS \
	QueryDesc *queryDesc, ScanDirection direction, uint64 count
#define PSSC_EXECUTOR_RUN_ARGS \
	queryDesc, direction, count
#else
#define PSSC_EXECUTOR_RUN_PARAMS \
	QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once
#define PSSC_EXECUTOR_RUN_ARGS \
	queryDesc, direction, count, execute_once
#endif

/*
 * ProcessUtility hook signature, used like PSSC_EXECUTOR_RUN_*. Re-check
 * tcop/utility.h whenever a new major version is added.
 */
/* PG14-18: identical (readOnlyTree since PG14, QueryCompletion since PG13). */
#define PSSC_PROCESS_UTILITY_PARAMS \
	PlannedStmt *pstmt, const char *queryString, bool readOnlyTree, \
	ProcessUtilityContext context, ParamListInfo params, \
	QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc
#define PSSC_PROCESS_UTILITY_ARGS \
	pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc

/*
 * planner_hook signature, used like PSSC_EXECUTOR_RUN_*. Re-check
 * optimizer/planner.h whenever a new major version is added.
 */
/* PG14-18: identical (query_string since PG13). */
#define PSSC_PLANNER_PARAMS \
	Query *parse, const char *query_string, int cursorOptions, \
	ParamListInfo boundParams
#define PSSC_PLANNER_ARGS \
	parse, query_string, cursorOptions, boundParams

/*
 * Whether pg_stat_statements counts planning as a nesting level for
 * toplevel and track (statements run by functions evaluated while
 * planning, e.g. constant folding, are then not top level).
 */
/* PG17+: always (pgss_planner increments nesting_level even when not tracking planning); PG14-16: never (toplevel and track use exec_nested_level only). */
#if PG_VERSION_NUM >= 170000
#define PSSC_HAS_PLANNER_NESTING 1
#else
#define PSSC_HAS_PLANNER_NESTING 0
#endif

/*
 * Whether pg_stat_statements records DEALLOCATE as a utility statement
 * (EXECUTE and PREPARE are never recorded, on any version).
 */
/* PG17+: yes (PGSS_HANDLED_UTILITY dropped DeallocateStmt); PG14-16: no, excluded like EXECUTE/PREPARE. */
#if PG_VERSION_NUM >= 170000
#define PSSC_PGSS_RECORDS_DEALLOCATE 1
#else
#define PSSC_PGSS_RECORDS_DEALLOCATE 0
#endif

/*
 * Whether pg_stat_statements counts a utility (other than EXECUTE/PREPARE)
 * as a nesting level only when it tracks that utility under its own
 * settings (pg_stat_statements.track_utility, pgss_enabled(level) and
 * PGSS_HANDLED_UTILITY), rather than always.
 */
/* PG14-16: only when tracked (exec_nested_level++ inside the tracking branch); PG17+: always (nesting_level++ in both branches). */
#if PG_VERSION_NUM >= 170000
#define PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES 0
#else
#define PSSC_PGSS_NESTS_ONLY_TRACKED_UTILITIES 1
#endif

/*
 * Shared-memory requests (RequestAddinShmemSpace, RequestNamedLWLockTranche).
 * Write a request function that first calls the saved previous hook (if any),
 * then makes the requests, and register it from _PG_init (while
 * process_shared_preload_libraries_in_progress) with
 *		PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev_shmem_request_hook, my_request);
 */
/* PG15+: install as shmem_request_hook; PG14: no such hook, call it now from _PG_init. */
#if PG_VERSION_NUM >= 150000
typedef shmem_request_hook_type pssc_shmem_request_hook_type;
#define PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev, fn) \
	do { \
		(prev) = shmem_request_hook; \
		shmem_request_hook = (fn); \
	} while (0)
#else
typedef void (*pssc_shmem_request_hook_type) (void);
#define PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev, fn) \
	do { \
		(prev) = NULL; \
		(fn) (); \
	} while (0)
#endif

/*
 * Allocate a GUC check_hook "extra" blob the way guc.c will free it.
 * Returns NULL on out-of-memory; the check_hook should then return false.
 * guc.c frees only this top-level block, so keep it pointer-free.
 */
/* PG14/15: malloc (guc.c frees with free()); PG16+: guc_malloc in the GUC memory context. */
static inline void *
pssc_guc_extra_alloc(Size size)
{
#if PG_VERSION_NUM >= 160000
	return guc_malloc(LOG, size);
#else
	return malloc(size);
#endif
}

/*
 * Regex allocation. pg_regcomp allocates the compiled regex with whatever
 * the core regex library uses; pssc_regcomp compiles with cxt as the current
 * memory context so that, where the library uses palloc, the regex lives in
 * a context the caller owns. Always release with pssc_regfree (pg_regfree):
 * on PG14/15 the memory is malloc'd and deleting cxt does not free it.
 */
/* PG14/15: regex library uses malloc (cxt unused); PG16+: palloc in CurrentMemoryContext. */
#if PG_VERSION_NUM >= 160000
#define PSSC_REGEX_USES_PALLOC 1
#else
#define PSSC_REGEX_USES_PALLOC 0
#endif

static inline int
pssc_regcomp(MemoryContext cxt, regex_t *re, const pg_wchar *pattern,
			 size_t len, int flags, Oid collation)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
	int			rc = pg_regcomp(re, pattern, len, flags, collation);

	MemoryContextSwitchTo(oldcxt);
	return rc;
}

#define pssc_regfree(re) pg_regfree(re)

/* Rows for a finished statement, matching pg_stat_statements. */
/* PG14/15: es_processed (last ExecutorRun only); PG16+: es_total_processed. */
#if PG_VERSION_NUM >= 160000
#define PSSC_QUERYDESC_ROWS(qd) ((qd)->estate->es_total_processed)
#else
#define PSSC_QUERYDESC_ROWS(qd) ((qd)->estate->es_processed)
#endif

/*
 * Buffer, WAL, I/O-timing and JIT counter availability. Fields present on
 * every supported version (shared/local/temp blks_*, wal_records, wal_fpi,
 * wal_bytes, JIT created_functions and *_counter except deform_counter) need
 * no macro. Test the PSSC_HAS_* macros with #if; they are always defined.
 */
/* PG15+: BufferUsage.temp_blk_read_time/temp_blk_write_time (absent on PG14). */
#if PG_VERSION_NUM >= 150000
#define PSSC_HAS_TEMP_BLK_IO_TIME 1
#else
#define PSSC_HAS_TEMP_BLK_IO_TIME 0
#endif

/* PG17+: BufferUsage.local_blk_read_time/local_blk_write_time (absent on PG14-16). */
#if PG_VERSION_NUM >= 170000
#define PSSC_HAS_LOCAL_BLK_IO_TIME 1
#else
#define PSSC_HAS_LOCAL_BLK_IO_TIME 0
#endif

/* PG14-16: BufferUsage.blk_read_time/blk_write_time; PG17+: renamed shared_blk_*_time. */
#if PG_VERSION_NUM >= 170000
#define PSSC_SHARED_BLK_READ_TIME(bu) ((bu).shared_blk_read_time)
#define PSSC_SHARED_BLK_WRITE_TIME(bu) ((bu).shared_blk_write_time)
#else
#define PSSC_SHARED_BLK_READ_TIME(bu) ((bu).blk_read_time)
#define PSSC_SHARED_BLK_WRITE_TIME(bu) ((bu).blk_write_time)
#endif

/* PG18+: WalUsage.wal_buffers_full (absent on PG14-17). */
#if PG_VERSION_NUM >= 180000
#define PSSC_HAS_WAL_BUFFERS_FULL 1
#else
#define PSSC_HAS_WAL_BUFFERS_FULL 0
#endif

/* PG17+: JitInstrumentation.deform_counter (absent on PG14-16). */
#if PG_VERSION_NUM >= 170000
#define PSSC_HAS_JIT_DEFORM_COUNTER 1
#else
#define PSSC_HAS_JIT_DEFORM_COUNTER 0
#endif

/*
 * Reserve the GUC prefix after defining custom GUCs, so misspelled
 * placeholders are reported.
 */
/* PG15+: MarkGUCPrefixReserved (removes and rejects placeholders); PG14: EmitWarningsOnPlaceholders (warns only). */
#if PG_VERSION_NUM >= 150000
#define PSSC_MARK_GUC_PREFIX_RESERVED(prefix) MarkGUCPrefixReserved(prefix)
#else
#define PSSC_MARK_GUC_PREFIX_RESERVED(prefix) EmitWarningsOnPlaceholders(prefix)
#endif

/*
 * Set up a materialize-mode SRF: after the call, fill
 * ((ReturnSetInfo *) fcinfo->resultinfo)->setResult using ->setDesc.
 */
/* PG15.1+: core InitMaterializedSRF; PG15.0: same function named SetSingleFuncCall; PG14: hand-rolled below. */
#if PG_VERSION_NUM >= 150001
#define PSSC_MAT_SRF_USE_EXPECTED_DESC MAT_SRF_USE_EXPECTED_DESC
#define PSSC_MAT_SRF_BLESS MAT_SRF_BLESS
#define pssc_init_materialized_srf(fcinfo, flags) InitMaterializedSRF(fcinfo, flags)
#elif PG_VERSION_NUM >= 150000
#define PSSC_MAT_SRF_USE_EXPECTED_DESC SRF_SINGLE_USE_EXPECTED
#define PSSC_MAT_SRF_BLESS SRF_SINGLE_BLESS
#define pssc_init_materialized_srf(fcinfo, flags) SetSingleFuncCall(fcinfo, flags)
#else
#define PSSC_MAT_SRF_USE_EXPECTED_DESC 0x01
#define PSSC_MAT_SRF_BLESS 0x02

/* Mirrors InitMaterializedSRF() from PostgreSQL 15 funcapi.c. */
static inline void
pssc_init_materialized_srf(FunctionCallInfo fcinfo, bits32 flags)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	bool		random_access;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & PSSC_MAT_SRF_USE_EXPECTED_DESC) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	if ((flags & PSSC_MAT_SRF_USE_EXPECTED_DESC) != 0)
		tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if ((flags & PSSC_MAT_SRF_BLESS) != 0)
		BlessTupleDesc(tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcxt);
}
#endif

#endif							/* PSSC_COMPAT_H */
