/*
 * fuzz_sqlcommenter.c
 *		libFuzzer entry point for pssc_parse_sqlcommenter() (DESIGN.md §9).
 *		The input is one comment body, parsed with url_decode on and off.
 *		If the input is also a complete comment span, pssc_comment_body() is
 *		checked and its body parsed too.
 */
#include "fuzz_common.h"

static void
parse_sc(const char *body, size_t len, const void *arg,
		 const PsscPairOut *out, PsscPairResult *r)
{
	pssc_parse_sqlcommenter(body, len, *(const bool *) arg, out, r);
}

FUZZ_ALPHABET("'\\%0aF2,:=;> \t\nk\0")

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const bool on = true,
				off = false;
	const char *s = (const char *) data;
	size_t		off_body,
				blen;

	fuzz_run(parse_sc, &on, s, size);
	fuzz_run(parse_sc, &off, s, size);
	if (pssc_comment_body(s, size, &off_body, &blen))
	{
		FUZZ_CHECK(off_body + blen <= size);
		fuzz_run(parse_sc, &on, s + off_body, blen);
	}
	else
		FUZZ_CHECK(off_body == 0 && blen == 0);
	return 0;
}
