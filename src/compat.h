/*
 * compat.h
 *		PostgreSQL 14-18 version shims for pg_stat_statement_context.
 *
 * Version-dependent code belongs here rather than in scattered
 * PG_VERSION_NUM checks (DESIGN.md §6.10).
 */
#ifndef PSSC_COMPAT_H
#define PSSC_COMPAT_H

/* PG16+: queryjumble.h moved from utils/ to nodes/ (PG14/15: utils/). */
#if PG_VERSION_NUM >= 160000
#include "nodes/queryjumble.h"
#else
#include "utils/queryjumble.h"
#endif

#endif							/* PSSC_COMPAT_H */
