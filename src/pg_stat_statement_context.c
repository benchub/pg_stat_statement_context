/*
 * pg_stat_statement_context.c
 *		Module entry point: _PG_init and hook installation.
 *
 * See DESIGN.md for the overall architecture.
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"

#include "compat.h"
#include "extract.h"
#include "guc.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/*
 * Module load callback.
 *
 * The extension only works when listed in shared_preload_libraries
 * (DESIGN.md §6.12); a plain LOAD or CREATE EXTENSION without preloading is
 * a no-op.
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* GUCs exist only when preloaded; postmaster GUCs need this anyway. */
	pssc_guc_define();
	pssc_extract_init();

	/*
	 * Make compute_query_id = auto behave as on, so query IDs match
	 * pg_stat_statements (DESIGN.md §2).
	 */
	EnableQueryId();
}
