/*
 * utility.h
 *		ProcessUtility hook: utility frames and recording (DESIGN.md §3.2,
 *		§6.6, §6.7, §6.9).
 */
#ifndef PSSC_UTILITY_H
#define PSSC_UTILITY_H

/* Installs the ProcessUtility hook; from _PG_init, while preloading. */
extern void pssc_utility_init(void);

#endif							/* PSSC_UTILITY_H */
