/*
 * fuzz_marginalia.c
 *		libFuzzer entry point for pssc_parse_marginalia() (DESIGN.md §9).
 *		The input is one comment body, parsed with the default separators
 *		and a few custom ones (multi-byte, overlapping, equal, NUL, invalid,
 *		starting with whitespace), and checked against a naive
 *		split-on-pair_sep-first reference.
 */
#include "fuzz_common.h"

static void
parse_mg(const char *body, size_t len, const void *arg,
		 const PsscPairOut *out, PsscPairResult *r)
{
	pssc_parse_marginalia(body, len, (const PsscMarginaliaOpts *) arg, out, r);
}

static bool
ref_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static size_t
ref_find(const char *s, size_t from, size_t to, const char *sep, size_t seplen)
{
	size_t		i;

	for (i = from; i + seplen <= to; i++)
		if (memcmp(s + i, sep, seplen) == 0)
			return i;
	return to;
}

/*
 * Naive reference for the documented semantics: split on pair_sep first,
 * then on the first kv_sep inside each segment. Compares the pair list and
 * the malformed count with the real parser's (ample-storage) result.
 */
static void
ref_compare(const char *s, size_t len, const char *kv, size_t kvlen,
			const char *ps, size_t pslen)
{
	size_t		maxp = (len + 1) / 2 + 1;
	PsscPair   *pairs = malloc(maxp * sizeof(PsscPair));
	char	   *buf = malloc(len + 1);
	PsscPairOut out = {pairs, maxp, buf, len};
	PsscMarginaliaOpts o = {kv, kvlen, ps, pslen};
	PsscPairResult r;
	size_t		start = 0,
				np = 0,
				nmal = 0;

	FUZZ_CHECK(pairs && buf);
	pssc_parse_marginalia(s, len, &o, &out, &r);
	for (;;)
	{
		size_t		end = ref_find(s, start, len, ps, pslen);
		size_t		k = ref_find(s, start, end, kv, kvlen);
		size_t		ks = start,
					ke = k,
					vs = k + kvlen,
					ve = end,
					i;
		bool		blank = true,
					ok = true;

		for (i = start; i < end; i++)
			if (!ref_space((unsigned char) s[i]))
				blank = false;
		if (!blank)
		{
			if (k == end)
				ok = false;
			else
			{
				while (ks < ke && ref_space((unsigned char) s[ks]))
					ks++;
				while (ke > ks && ref_space((unsigned char) s[ke - 1]))
					ke--;
				while (vs < ve && ref_space((unsigned char) s[vs]))
					vs++;
				while (ve > vs && ref_space((unsigned char) s[ve - 1]))
					ve--;
				if (ks == ke)
					ok = false;
				for (i = ks; i < ke; i++)
					if (ref_space((unsigned char) s[i]))
						ok = false;
			}
			if (!ok)
				nmal++;
			else
			{
				FUZZ_CHECK(np < r.npairs);
				FUZZ_CHECK(pairs[np].keylen == ke - ks &&
						   memcmp(pairs[np].key, s + ks, ke - ks) == 0);
				FUZZ_CHECK(pairs[np].valuelen == ve - vs &&
						   (ve == vs || memcmp(pairs[np].value, s + vs, ve - vs) == 0));
				np++;
			}
		}
		if (end >= len)
			break;
		start = end + pslen;
	}
	FUZZ_CHECK(np == r.npairs);
	FUZZ_CHECK(nmal == r.nmalformed);
	free(pairs);
	free(buf);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const PsscMarginaliaOpts opts[] = {
		{":", 1, ",", 1},
		{"=", 1, " ", 1},
		{"=>", 2, ";;", 2},
		{"=>", 2, ">", 1},		/* pair_sep inside kv_sep */
		{"=>", 2, ">,", 2},		/* pair_sep straddles kv_sep's end */
		{">", 1, "=>", 2},		/* kv_sep inside pair_sep */
		{"::", 2, ":", 1},
		{":", 1, ":", 1},
		{"\0", 1, ",", 1},
		{" :", 2, ",", 1},		/* kv_sep starting with whitespace */
		{" \t:", 3, ",", 1},
		{"\t=>", 3, ";", 1},
		{"  ", 2, ",", 1},		/* whitespace-only kv_sep */
	};
	static const PsscMarginaliaOpts invalid = {"", 0, "123456789", 9};
	const char *s = (const char *) data;
	size_t		i;

	fuzz_run(parse_mg, NULL, s, size);
	fuzz_run(parse_mg, &invalid, s, size);	/* falls back to the defaults */
	for (i = 0; i < sizeof(opts) / sizeof(opts[0]); i++)
	{
		fuzz_run(parse_mg, &opts[i], s, size);
		ref_compare(s, size, opts[i].kv_sep, opts[i].kv_sep_len,
					opts[i].pair_sep, opts[i].pair_sep_len);
	}
	return 0;
}
