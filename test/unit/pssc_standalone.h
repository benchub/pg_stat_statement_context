/*
 * pssc_standalone.h
 *		The few c.h definitions that src/guc.h and src/counters.h need, for
 *		building the backend-independent src/tagset.c and src/counters.c
 *		without a server (test/unit, fuzz/). They include this instead of
 *		postgres.h when PSSC_STANDALONE is defined.
 */
#ifndef PSSC_STANDALONE_H
#define PSSC_STANDALONE_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int64_t int64;

#define PGDLLEXPORT
#define FLEXIBLE_ARRAY_MEMBER
#define Assert(x) assert(x)

#endif							/* PSSC_STANDALONE_H */
