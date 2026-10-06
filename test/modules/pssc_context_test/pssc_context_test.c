/*
 * pssc_context_test.c
 *		TEST-ONLY driver for the execution frames of pg_stat_statement_context
 *		(src/context.h). See test/t/010_context.pl.
 *
 * When preloaded after pg_stat_statement_context, it installs ExecutorStart,
 * ExecutorRun, ExecutorFinish, ExecutorEnd and ProcessUtility hooks that
 * create, look up, activate and snapshot frames exactly as DESIGN.md §3.2
 * and §3.3 describe, but record nothing; the recording hooks of the main
 * library (backlog items -17 and -18) follow the same pattern. SQL
 * functions expose the active frame, the frame registry and the frames
 * found at ExecutorEnd.
 *
 * The main library is reached through load_external_function() (its
 * symbols are PGDLLEXPORT), like the other test modules.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/parsenodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "compat.h"
#include "context.h"

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"
#define LOG_MAX 1000

void		_PG_init(void);

typedef PsscFrame *(*create_fn) (QueryDesc *);
typedef PsscFrame *(*lookup_fn) (const QueryDesc *);
typedef void (*uinit_fn) (PsscUtilityFrame *, const PlannedStmt *, const char *);
typedef void (*enter_fn) (PsscFrameSave *, PsscFrame *, bool);
typedef void (*leave_fn) (const PsscFrameSave *);
typedef void (*refresh_fn) (PsscFrame *);
typedef int (*count_fn) (void);
typedef void (*xstats_fn) (PsscFrameXactStats *);

static create_fn f_create;
static lookup_fn f_lookup;
static uinit_fn f_uinit;
static enter_fn f_enter;
static leave_fn f_leave;
static refresh_fn f_refresh;
static count_fn f_count;
static xstats_fn f_xstats;
static PsscFrame **p_active;
static int *p_nesting;
static bool *p_enabled;
static bool hooks_installed = false;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

typedef struct EndedEntry
{
	char	   *query;
	char	   *tags;
	uint32		tags_len;
	bool		toplevel;
	bool		start_toplevel;
	bool		recordable;
	Oid			userid;
	Oid			start_userid;
} EndedEntry;

static EndedEntry *ended = NULL;
static int	nended = 0;
static int64 end_misses = 0;
static MemoryContext leak_cxt = NULL;

static void *
main_sym(const char *name)
{
	return (void *) load_external_function(MAIN_LIB, name, true, NULL);
}

static void
resolve_main(void)
{
	if (f_create != NULL)
		return;
	f_lookup = (lookup_fn) main_sym("pssc_frame_lookup");
	f_uinit = (uinit_fn) main_sym("pssc_utility_frame_init");
	f_enter = (enter_fn) main_sym("pssc_frame_enter");
	f_leave = (leave_fn) main_sym("pssc_frame_leave");
	f_refresh = (refresh_fn) main_sym("pssc_frame_refresh");
	f_count = (count_fn) main_sym("pssc_frame_count");
	f_xstats = (xstats_fn) main_sym("pssc_frame_xact_stats");
	p_active = (PsscFrame **) main_sym("pssc_active_frame");
	p_nesting = (int *) main_sym("pssc_nesting_level");
	p_enabled = (bool *) main_sym("pssc_enabled");
	f_create = (create_fn) main_sym("pssc_frame_create");
}

static bool
wants_frame(QueryDesc *queryDesc)
{
	return *p_enabled && !IsParallelWorker() &&
		queryDesc->plannedstmt->queryId != 0;
}

static void
t_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (wants_frame(queryDesc))
		f_create(queryDesc);
}

static void
t_ExecutorRun(PSSC_EXECUTOR_RUN_PARAMS)
{
	PsscFrameSave save;

	f_enter(&save, f_lookup(queryDesc), true);
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
		else
			standard_ExecutorRun(PSSC_EXECUTOR_RUN_ARGS);
	}
	PG_FINALLY();
	{
		f_leave(&save);
	}
	PG_END_TRY();
}

static void
t_ExecutorFinish(QueryDesc *queryDesc)
{
	PsscFrameSave save;

	f_enter(&save, f_lookup(queryDesc), true);
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		f_leave(&save);
	}
	PG_END_TRY();
}

static void
log_ended(QueryDesc *queryDesc, PsscFrame *frame)
{
	EndedEntry *e;
	const char *src = queryDesc->sourceText;
	int			loc = queryDesc->plannedstmt->stmt_location;
	int			len = queryDesc->plannedstmt->stmt_len;
	bool		start_toplevel = frame->toplevel;
	Oid			start_userid = frame->userid;

	f_refresh(frame);
	if (ended == NULL)
		ended = MemoryContextAllocZero(TopMemoryContext,
									   sizeof(EndedEntry) * LOG_MAX);
	if (nended >= LOG_MAX)
		elog(ERROR, "pssc_context_test: ExecutorEnd log full");
	e = &ended[nended++];
	if (src == NULL)
		src = "";
	if (loc < 0)
	{
		loc = 0;
		len = 0;
	}
	if (len <= 0)
		len = strlen(src + loc);
	len = Min(len, 200);
	e->query = MemoryContextAlloc(TopMemoryContext, len + 1);
	memcpy(e->query, src + loc, len);
	e->query[len] = '\0';
	e->tags = MemoryContextAlloc(TopMemoryContext, frame->tags_len + 1);
	memcpy(e->tags, frame->tags, frame->tags_len);
	e->tags_len = frame->tags_len;
	e->toplevel = frame->toplevel;
	e->start_toplevel = start_toplevel;
	e->recordable = frame->recordable;
	e->userid = frame->userid;
	e->start_userid = start_userid;
}

static void
t_ExecutorEnd(QueryDesc *queryDesc)
{
	PsscFrame  *frame = f_lookup(queryDesc);

	if (frame != NULL)
		log_ended(queryDesc, frame);
	else if (wants_frame(queryDesc))
		end_misses++;

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * As DESIGN.md §6.7: EXECUTE and PREPARE neither bump the nesting level
 * nor activate a frame (the executor of the prepared plan gets its own
 * frame from the saved source, §6.3); every other utility does, before
 * chaining, from a snapshot that never references pstmt afterwards.
 */
static void
t_ProcessUtility(PSSC_PROCESS_UTILITY_PARAMS)
{
	Node	   *parsetree = pstmt->utilityStmt;
	PsscUtilityFrame uf;
	PsscFrameSave save;

	if (!*p_enabled || IsA(parsetree, ExecuteStmt) ||
		IsA(parsetree, PrepareStmt))
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
		else
			standard_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
		return;
	}

	f_uinit(&uf, pstmt, queryString);
	f_enter(&save, &uf.frame, true);
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
		else
			standard_ProcessUtility(PSSC_PROCESS_UTILITY_ARGS);
	}
	PG_FINALLY();
	{
		f_leave(&save);
	}
	PG_END_TRY();
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;
	resolve_main();

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = t_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = t_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = t_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = t_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = t_ProcessUtility;
	hooks_installed = true;
}

static void
check_installed(void)
{
	if (!hooks_installed)
		elog(ERROR, "pssc_context_test must be in shared_preload_libraries after pg_stat_statement_context");
}

static Datum
tags_array(const char *buf, uint32 len)
{
	Datum	   *elems = palloc(sizeof(Datum) * (len / 2 + 1));
	int			n = 0;
	uint32		off = 0;

	while (off < len)
	{
		const char *k = buf + off;
		size_t		klen = strnlen(k, len - off);
		const char *v = k + klen + 1;
		size_t		vlen;

		if (off + klen >= len)
			elog(ERROR, "malformed tag set");
		vlen = strnlen(v, len - off - klen - 1);
		if (off + klen + 1 + vlen >= len)
			elog(ERROR, "malformed tag set");
		elems[n++] = PointerGetDatum(cstring_to_text(psprintf("%s=%s", k, v)));
		off += klen + vlen + 2;
	}
	return PointerGetDatum(construct_array(elems, n, TEXTOID, -1, false,
										   TYPALIGN_INT));
}

PG_FUNCTION_INFO_V1(pssc_context_test_active);
Datum
pssc_context_test_active(PG_FUNCTION_ARGS)
{
	PsscFrame  *f;
	TupleDesc	tupdesc;
	Datum		values[8];
	bool		nulls[8] = {0};

	check_installed();
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	f = *p_active;
	if (f == NULL)
	{
		for (int i = 0; i < 7; i++)
			nulls[i] = true;
	}
	else
	{
		values[0] = tags_array(f->tags, f->tags_len);
		values[1] = BoolGetDatum(f->utility);
		values[2] = BoolGetDatum(f->nested);
		values[3] = BoolGetDatum(f->toplevel);
		values[4] = BoolGetDatum(f->recordable);
		values[5] = Int32GetDatum(f->nesting_level);
		values[6] = Int64GetDatum(f->queryId);
	}
	values[7] = Int32GetDatum(*p_nesting);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_context_test_tags);
Datum
pssc_context_test_tags(PG_FUNCTION_ARGS)
{
	check_installed();
	if (*p_active == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(tags_array((*p_active)->tags, (*p_active)->tags_len));
}

PG_FUNCTION_INFO_V1(pssc_context_test_registry);
Datum
pssc_context_test_registry(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {0};
	PsscFrameXactStats st;

	check_installed();
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	f_xstats(&st);
	values[0] = Int32GetDatum(f_count());
	values[1] = Int64GetDatum((int64) st.checks);
	values[2] = Int64GetDatum((int64) st.leaks);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_context_test_ended);
Datum
pssc_context_test_ended(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			n = nended;

	check_installed();
	pssc_init_materialized_srf(fcinfo, 0);
	/* entries added by this very statement's End come later */
	for (int i = 0; i < n; i++)
	{
		Datum		values[7];
		bool		nulls[7] = {0};

		values[0] = CStringGetTextDatum(ended[i].query);
		values[1] = tags_array(ended[i].tags, ended[i].tags_len);
		values[2] = BoolGetDatum(ended[i].toplevel);
		values[3] = BoolGetDatum(ended[i].start_toplevel);
		values[4] = BoolGetDatum(ended[i].recordable);
		values[5] = ObjectIdGetDatum(ended[i].userid);
		values[6] = ObjectIdGetDatum(ended[i].start_userid);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		pfree(ended[i].query);
		pfree(ended[i].tags);
	}
	nended = 0;
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pssc_context_test_end_misses);
Datum
pssc_context_test_end_misses(PG_FUNCTION_ARGS)
{
	check_installed();
	PG_RETURN_INT64(end_misses);
}

PG_FUNCTION_INFO_V1(pssc_context_test_leak);
Datum
pssc_context_test_leak(PG_FUNCTION_ARGS)
{
	check_installed();
	if (PG_GETARG_BOOL(0))
	{
		MemoryContext old;
		QueryDesc  *qd;
		EState	   *es;
		PlannedStmt *ps;

		if (leak_cxt != NULL)
			elog(ERROR, "leak already created");
		leak_cxt = AllocSetContextCreate(TopMemoryContext,
										 "pssc_context_test leak",
										 ALLOCSET_SMALL_SIZES);
		old = MemoryContextSwitchTo(leak_cxt);
		ps = makeNode(PlannedStmt);
		ps->queryId = 1;
		ps->stmt_location = -1;
		es = palloc0(sizeof(EState));
		es->es_query_cxt = leak_cxt;
		qd = palloc0(sizeof(QueryDesc));
		qd->plannedstmt = ps;
		qd->sourceText = "SELECT 1";
		qd->estate = es;
		MemoryContextSwitchTo(old);
		if (f_create(qd) == NULL)
			elog(ERROR, "could not create frame");
	}
	else if (leak_cxt != NULL)
	{
		MemoryContextDelete(leak_cxt);
		leak_cxt = NULL;
	}
	PG_RETURN_VOID();
}
