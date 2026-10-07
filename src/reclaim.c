/*
 * reclaim.c
 *		Optional background worker that reclaims dead entries (DESIGN.md
 *		§5.3, §8). See reclaim.h.
 *
 * It is not needed for correctness: readers hide expired slots and an
 * eviction pass reclaims dead entries first. On an idle system it frees
 * their space before the next insert needs it, and keeps entries in
 * _info() close to the number of entries with data.
 *
 * The worker touches only the extension's shared memory, so it needs no
 * database connection (BGWORKER_SHMEM_ACCESS only) and does not appear in
 * pg_stat_activity; its process title is PSSC_RECLAIM_WORKER_NAME. Every
 * reclaim_worker_interval it calls pssc_store_reclaim_dead(), which takes
 * the exclusive lock only when current_bucket has moved since its last
 * pass, so at most once per bucket.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"

#include "counters.h"
#include "guc.h"
#include "reclaim.h"
#include "store.h"

void
pssc_reclaim_init(void)
{
	BackgroundWorker worker;

	if (!pssc_reclaim_worker)
		return;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	/* entries are only recorded once queries can run */
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 10;	/* seconds, after an ERROR exit */
	strlcpy(worker.bgw_library_name, "pg_stat_statement_context",
			sizeof(worker.bgw_library_name));
	strlcpy(worker.bgw_function_name, "pssc_reclaim_worker_main",
			sizeof(worker.bgw_function_name));
	strlcpy(worker.bgw_name, PSSC_RECLAIM_WORKER_NAME, sizeof(worker.bgw_name));
	strlcpy(worker.bgw_type, PSSC_RECLAIM_WORKER_NAME, sizeof(worker.bgw_type));
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
}

void
pssc_reclaim_worker_main(Datum main_arg)
{
	int64		last_watermark = PSSC_BUCKET_NONE;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	if (!pssc_store_available())
		proc_exit(0);			/* not preloaded: cannot happen */

	ereport(LOG,
			(errmsg("%s started", PSSC_RECLAIM_WORKER_NAME),
			 errdetail("Wakes up every %d ms.", pssc_reclaim_worker_interval)));

	while (!ShutdownRequestPending)
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		(void) pssc_store_reclaim_dead(&last_watermark);

		/* a reload (SIGHUP) or shutdown request sets the latch */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 pssc_reclaim_worker_interval,
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	proc_exit(0);
}
