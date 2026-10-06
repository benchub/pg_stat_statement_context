/*
 * executor.h
 *		ExecutorStart/Run/Finish/End hooks: executor frames and recording
 *		(DESIGN.md §3.2, §3.3, §6.8).
 */
#ifndef PSSC_EXECUTOR_H
#define PSSC_EXECUTOR_H

/* Installs the executor hooks; from _PG_init, while preloading. */
extern void pssc_executor_init(void);

/*
 * Adds this backend's extraction counters (pssc_extract_take_stats()) to
 * the shared header; after every recording opportunity (ExecutorEnd, a
 * utility's return).
 */
extern void pssc_flush_extract_stats(void);

#endif							/* PSSC_EXECUTOR_H */
