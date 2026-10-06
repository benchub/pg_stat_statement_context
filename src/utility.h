/*
 * utility.h
 *		ProcessUtility hook: utility frames and recording (DESIGN.md §3.2,
 *		§6.6, §6.7, §6.9).
 */
#ifndef PSSC_UTILITY_H
#define PSSC_UTILITY_H

/* Installs the ProcessUtility hook; from _PG_init, while preloading. */
extern void pssc_utility_init(void);

/*
 * Whether the shared_preload_libraries value spl lists pg_stat_statements
 * after this extension (DESIGN.md §3.2). Exported for the TEST-ONLY
 * pssc_guc_test module.
 */
extern PGDLLEXPORT bool pssc_load_order_wrong(const char *spl);

/* Warns if shared_preload_libraries has the wrong order; from _PG_init. */
extern void pssc_utility_check_load_order(void);

#endif							/* PSSC_UTILITY_H */
