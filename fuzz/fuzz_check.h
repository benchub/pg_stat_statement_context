/*
 * fuzz_check.h
 *		The invariant macro and libFuzzer entry point shared by all fuzz
 *		targets. A violated invariant aborts, which libFuzzer and the
 *		standalone driver (standalone_main.c) report as a crash.
 */
#ifndef PSSC_FUZZ_CHECK_H
#define PSSC_FUZZ_CHECK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * The bytes the standalone driver builds its random inputs from (each target
 * lists the bytes its code under test treats specially); *len is set to the
 * count, which may include NUL. Unused by libFuzzer.
 */
const char *fuzz_alphabet(size_t *len);

#define FUZZ_ALPHABET(lit) \
	const char * \
	fuzz_alphabet(size_t *len) \
	{ \
		*len = sizeof(lit) - 1; \
		return lit; \
	}

#define FUZZ_CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "%s:%d: invariant failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

#endif							/* PSSC_FUZZ_CHECK_H */
