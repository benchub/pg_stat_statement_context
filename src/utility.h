/*
 * utility.h
 *		ProcessUtility hook: utility frames and recording (DESIGN.md §3.2,
 *		§6.6, §6.7, §6.9).
 */
#ifndef PSSC_UTILITY_H
#define PSSC_UTILITY_H

#include "export.h"

/* Installs the ProcessUtility hook; from _PG_init, while preloading. */
extern void pssc_utility_init(void);

/*
 * Libraries that zero utility queryIds before chaining, so must be listed
 * before this extension in shared_preload_libraries (DESIGN.md §3.2).
 */
#define PSSC_LOAD_ORDER_PGSS	0x01	/* pg_stat_statements */
#define PSSC_LOAD_ORDER_PGSM	0x02	/* pg_stat_monitor */

/*
 * The PSSC_LOAD_ORDER_* flags of the libraries the shared_preload_libraries
 * value spl lists after this extension; *listed (may be NULL) gets those it
 * lists at all. Exported for the TEST-ONLY pssc_guc_test module.
 */
extern PSSC_TEST_API int pssc_load_order_wrong(const char *spl, int *listed);

/* Warns if shared_preload_libraries has the wrong order; from _PG_init. */
extern void pssc_utility_check_load_order(void);

#endif							/* PSSC_UTILITY_H */
