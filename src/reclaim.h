/*
 * reclaim.h
 *		Optional background worker that reclaims dead entries on idle
 *		systems (DESIGN.md §5.3, §8): pg_stat_statement_context.reclaim_worker.
 */
#ifndef PSSC_RECLAIM_H
#define PSSC_RECLAIM_H

#include "fmgr.h"

/* Name and type of the worker (process title, server log). */
#define PSSC_RECLAIM_WORKER_NAME "pg_stat_statement_context reclaim worker"

/*
 * Registers the worker if reclaim_worker is on; from _PG_init, after the
 * GUCs are defined. With it off nothing is registered at all.
 */
extern void pssc_reclaim_init(void);

/* The worker's entry point (bgw_function_name). */
extern PGDLLEXPORT void pssc_reclaim_worker_main(Datum main_arg);

#endif							/* PSSC_RECLAIM_H */
