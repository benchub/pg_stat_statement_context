/*
 * activity_fn.c
 *		pg_stat_statement_context_activity(): one row per backend slot that
 *		holds a top-level statement (activity.h), with the visibility rules of
 *		the stats views (DESIGN.md §6.11): rows of the calling role
 *		(GetUserId()) are complete; for other roles' rows queryid, state and
 *		tags are NULL unless the caller has the privileges of
 *		pg_read_all_stats. pid, userid and dbid are always shown, as in
 *		pg_stat_activity.
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "activity.h"
#include "compat.h"
#include "tagout.h"

#define ACTIVITY_COLS 6

PG_FUNCTION_INFO_V1(pg_stat_statement_context_activity_1_0);

Datum
pg_stat_statement_context_activity_1_0(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			caller;
	bool		see_all;
	int			nslots;
	int			i;
	PsscActivityRow row;
	MemoryContext rowcxt;
	MemoryContext oldcxt;

	if (!pssc_activity_available())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statement_context must be loaded via \"shared_preload_libraries\"")));

	pssc_init_materialized_srf(fcinfo, 0);
	if (rsinfo->setDesc->natts != ACTIVITY_COLS)
		elog(ERROR, "incorrect number of output arguments");

	caller = GetUserId();
	see_all = has_privs_of_role(caller, ROLE_PG_READ_ALL_STATS);
	nslots = pssc_activity_nslots();
	row.tags = palloc(Max(pssc_activity_tags_max(), 1));

	rowcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "pg_stat_statement_context activity row",
								   ALLOCSET_DEFAULT_SIZES);
	for (i = 0; i < nslots; i++)
	{
		Datum		values[ACTIVITY_COLS];
		bool		nulls[ACTIVITY_COLS];

		pssc_activity_read(i, &row);
		if (row.pid == 0)
			continue;

		memset(nulls, true, sizeof(nulls));
		values[0] = Int32GetDatum(row.pid);
		nulls[0] = false;
		values[1] = ObjectIdGetDatum(row.userid);
		nulls[1] = false;
		values[2] = ObjectIdGetDatum(row.dbid);
		nulls[2] = false;

		/* tuplestore_putvalues copies: build each row in rowcxt */
		oldcxt = MemoryContextSwitchTo(rowcxt);
		if (see_all || row.userid == caller)
		{
			if (row.queryid != 0)
			{
				values[3] = Int64GetDatum(row.queryid);
				nulls[3] = false;
			}
			values[4] = CStringGetTextDatum(row.active ? "active" : "idle");
			nulls[4] = false;
			values[5] = JsonbPGetDatum(pssc_tags_jsonb_noerror(row.tags, row.tags_len,
															   row.encoding, NULL));
			nulls[5] = false;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(rowcxt);
	}
	MemoryContextDelete(rowcxt);
	pfree(row.tags);

	return (Datum) 0;
}
