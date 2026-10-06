/*
 * fuzz_common.h
 *		Shared invariant checks for the pair-parser fuzz targets
 *		(fuzz_sqlcommenter.c, fuzz_marginalia.c). Any violation aborts, which
 *		libFuzzer and the standalone driver report as a crash.
 */
#ifndef PSSC_FUZZ_COMMON_H
#define PSSC_FUZZ_COMMON_H

#include <string.h>

#include "fuzz_check.h"
#include "pairs.h"

typedef void (*FuzzParse) (const char *body, size_t len, const void *arg,
						   const PsscPairOut *out, PsscPairResult *r);

/*
 * Parse once. In the PSSC_PAIRS_CHECKED build (every fuzz build: pairs.c
 * then aborts on any read outside the body) also check the parse is linear,
 * with the bound test/unit/test_pairs.c uses: 3 reads per byte + 8.
 */
static void
fuzz_parse(FuzzParse parse, const char *body, size_t len, const void *arg,
		   const PsscPairOut *out, PsscPairResult *r)
{
#ifdef PSSC_PAIRS_CHECKED
	pssc_pairs_reads = 0;
#endif
	parse(body, len, arg, out, r);
#ifdef PSSC_PAIRS_CHECKED
	if (pssc_pairs_reads > 3 * (unsigned long) len + 8)
		fprintf(stderr, "%lu reads for %zu bytes\n", pssc_pairs_reads, len);
	FUZZ_CHECK(pssc_pairs_reads <= 3 * (unsigned long) len + 8);
#endif
}

static void
fuzz_check_storage(const PsscPairOut *out, const PsscPairResult *r)
{
	size_t		i;

	FUZZ_CHECK(r->npairs <= out->max_pairs);
	FUZZ_CHECK(r->bufused <= out->bufsize);
	for (i = 0; i < r->npairs; i++)
	{
		const PsscPair *p = &out->pairs[i];

		FUZZ_CHECK(p->keylen > 0);
		FUZZ_CHECK(p->key >= out->buf && p->key + p->keylen <= out->buf + r->bufused);
		FUZZ_CHECK(p->valuelen == 0 ||
				   (p->value >= out->buf && p->value + p->valuelen <= out->buf + r->bufused));
		FUZZ_CHECK(!(p->flags & PSSC_PAIR_KEY_NUL) == !memchr(p->key, 0, p->keylen));
		FUZZ_CHECK(!(p->flags & PSSC_PAIR_VALUE_NUL) ==
				   !(p->valuelen && memchr(p->value, 0, p->valuelen)));
	}
}

static bool
fuzz_pair_equal(const PsscPair *a, const PsscPair *b)
{
	return a->keylen == b->keylen && memcmp(a->key, b->key, a->keylen) == 0 &&
		a->valuelen == b->valuelen &&
		(a->valuelen == 0 || memcmp(a->value, b->value, a->valuelen) == 0) &&
		a->flags == b->flags;
}

/*
 * Parse with the documented "always enough" storage (len bytes and
 * (len + 1) / 2 + 1 pairs), which must drop nothing, then with little
 * storage, which must keep a subsequence and count the rest as dropped.
 */
static void
fuzz_run(FuzzParse parse, const void *arg, const char *body, size_t len)
{
	size_t		maxp = (len + 1) / 2 + 1;
	PsscPair   *pairs = malloc(maxp * sizeof(PsscPair));
	PsscPair	small_pairs[3];
	char	   *buf = malloc(len + 1);
	char	   *small_buf = malloc(len / 3 + 1);
	PsscPairOut out = {pairs, maxp, buf, len};
	PsscPairOut small = {small_pairs, 3, small_buf, len / 3};
	PsscPairResult r,
				rs;
	size_t		i,
				j = 0;

	FUZZ_CHECK(pairs && buf && small_buf);
	fuzz_parse(parse, body, len, arg, &out, &r);
	fuzz_check_storage(&out, &r);
	FUZZ_CHECK(r.ndropped == 0);

	fuzz_parse(parse, body, len, arg, &small, &rs);
	fuzz_check_storage(&small, &rs);
	FUZZ_CHECK(rs.nmalformed == r.nmalformed);
	FUZZ_CHECK(rs.npairs + rs.ndropped == r.npairs);
	for (i = 0; i < rs.npairs; i++)
	{
		while (j < r.npairs && !fuzz_pair_equal(&small_pairs[i], &pairs[j]))
			j++;
		FUZZ_CHECK(j < r.npairs);
		j++;
	}

	/* no room at all: everything is dropped, nothing is written */
	small.pairs = NULL;
	small.max_pairs = 0;
	small.buf = NULL;
	small.bufsize = 0;
	fuzz_parse(parse, body, len, arg, &small, &rs);
	FUZZ_CHECK(rs.npairs == 0 && rs.bufused == 0 && rs.ndropped == r.npairs);

	free(pairs);
	free(buf);
	free(small_buf);
}

#endif							/* PSSC_FUZZ_COMMON_H */
