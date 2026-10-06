/*
 * fuzz_scan.c
 *		libFuzzer entry point for the comment scanner, src/scan.c (DESIGN.md
 *		§6.2, §6.5, §6.11, §9). The input is SQL text. Every entry point runs
 *		on every input, in every positional mode, with both
 *		standard_conforming_strings settings and a set of scan windows
 *		(including ones below and above the input length, so both the exact
 *		and the windowed/heuristic paths run). Sub-ranges, statement starts
 *		and windows that are not fixed are derived from a hash of the input,
 *		so they are reproducible.
 *
 * Built with -DPSSC_SCAN_CHECKED, so scan.c aborts on any read outside the
 * range it was given (not only outside the allocation, which ASan checks),
 * and counts reads, which are checked against the linear bounds of
 * test/unit (3 reads per byte for a full lex, 4 per byte of the range or
 * window for a positional scan, plus 8).
 *
 * Invariants checked (beyond "no crash, no sanitizer report"):
 *	- every span lies inside the range (or the tail window), spans are
 *	  ordered and disjoint, at most 16, and each is a whole comment: it
 *	  starts with slash-star and ends with star-slash, or starts with "--"
 *	  and ends at a newline or the range end; re-lexing a span on its own
 *	  yields exactly that span (idempotence);
 *	- a smaller comment budget gives a prefix of the unbounded result and
 *	  sets truncated when it drops comments; the reported bytes respect the
 *	  budget;
 *	- the result does not depend on where the range sits in memory
 *	  (translation invariance) and pssc_scan_only_trivia() agrees with the
 *	  full lex;
 *	- position any equals a full scan with the window as budget; append is
 *	  a whitespace-separated run of block comments followed only by ';' and
 *	  whitespace, maximal and ending at the last comment; prepend is the
 *	  leading run, maximal, and with a window it is the prefix of the exact
 *	  run whose comments end within the window; the heuristic flag is set
 *	  exactly on the tail path; on input without quotes, '$', '\' and '-'
 *	  the tail path reports a suffix of the exact run;
 *	- pssc_stmt_range(), pssc_stmt_owned_start() and pssc_scan_footer()
 *	  stay within the string and honor their documented contracts.
 */
#include <stdbool.h>
#include <string.h>

#include "fuzz_check.h"
#include "scan.h"

#ifndef PSSC_SCAN_CHECKED
#error "fuzz_scan.c must be built with -DPSSC_SCAN_CHECKED"
#endif

#define MAXC PSSC_SCAN_MAX_COMMENTS

static bool
is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool
only_bytes(const char *s, size_t p, size_t q, bool semi)
{
	for (; p < q; p++)
		if (!is_space((unsigned char) s[p]) && !(semi && s[p] == ';'))
			return false;
	return true;
}

static bool
is_block(const char *s, const PsscCommentSpan *c)
{
	return s[c->offset + 1] == '*';
}

static bool
same_span(const PsscCommentSpan *a, const PsscCommentSpan *b, size_t shift)
{
	return a->offset == b->offset + shift && a->len == b->len;
}

static uint64_t
hash64(const uint8_t *d, size_t n)
{
	uint64_t	h = 1469598103934665603ULL;
	size_t		i;

	for (i = 0; i < n; i++)
		h = (h ^ d[i]) * 1099511628211ULL;
	return h ^ (h >> 29);
}

static void
check_reads(unsigned long limit)
{
	if (pssc_scan_reads > limit)
		fprintf(stderr, "%lu byte reads, limit %lu\n", pssc_scan_reads, limit);
	FUZZ_CHECK(pssc_scan_reads <= limit);
}

/*
 * The generic span checks: inside [lo, hi), ordered, disjoint, whole
 * comments, re-lexable on their own.
 */
static void
check_spans(const char *s, size_t lo, size_t hi, const PsscScanResult *r)
{
	int			i;
	size_t		prev_end = lo;

	FUZZ_CHECK(r->ncomments >= 0 && r->ncomments <= MAXC);
	FUZZ_CHECK(!(r->truncated && r->unterminated));
	for (i = 0; i < r->ncomments; i++)
	{
		const PsscCommentSpan *c = &r->comments[i];
		size_t		e = c->offset + c->len;
		PsscScanResult again;
		unsigned long saved = pssc_scan_reads;

		FUZZ_CHECK(c->offset >= prev_end && c->len >= 2 && e <= hi && e > c->offset);
		prev_end = e;
		if (s[c->offset] == '/')
		{
			FUZZ_CHECK(s[c->offset + 1] == '*' && c->len >= 4);
			FUZZ_CHECK(s[e - 2] == '*' && s[e - 1] == '/');
		}
		else
		{
			FUZZ_CHECK(s[c->offset] == '-' && s[c->offset + 1] == '-');
			FUZZ_CHECK(!memchr(s + c->offset, '\n', c->len) &&
					   !memchr(s + c->offset, '\r', c->len));
			FUZZ_CHECK(e == hi || s[e] == '\n' || s[e] == '\r');
		}
		/* idempotence: the span alone lexes to exactly itself */
		pssc_scan_comments(s, c->offset, e, true, SIZE_MAX, &again);
		FUZZ_CHECK(again.ncomments == 1 && !again.truncated && !again.unterminated);
		FUZZ_CHECK(same_span(&again.comments[0], c, 0));
		pssc_scan_reads = saved;
	}
}

static unsigned long
span_bytes(const PsscScanResult *r)
{
	unsigned long n = 0;
	int			i;

	for (i = 0; i < r->ncomments; i++)
		n += r->comments[i].len;
	return n;
}

static void
scan_all(const char *s, size_t lo, size_t hi, bool scs, size_t budget,
		 PsscScanResult *r)
{
	memset(r, 0x5a, sizeof(*r));
	pssc_scan_reads = 0;
	pssc_scan_comments(s, lo, hi, scs, budget, r);
	check_reads(3 * (unsigned long) (hi - lo) + 8);
	check_spans(s, lo, hi, r);
	FUZZ_CHECK(!r->heuristic);
	FUZZ_CHECK(span_bytes(r) <= budget);
}

/* full lex of [lo, hi) plus budget, translation and trivia checks */
static void
check_full(const char *s, size_t lo, size_t hi, bool scs, size_t budget,
		   PsscScanResult *full)
{
	PsscScanResult b,
				moved;
	char	   *copy = malloc(hi - lo + 1);
	int			i;
	bool		trivia,
				gaps_trivial = true;
	size_t		p = lo;

	FUZZ_CHECK(copy);
	scan_all(s, lo, hi, scs, SIZE_MAX, full);

	/* a budget keeps a prefix and flags what it dropped */
	scan_all(s, lo, hi, scs, budget, &b);
	FUZZ_CHECK(b.ncomments <= full->ncomments);
	for (i = 0; i < b.ncomments; i++)
		FUZZ_CHECK(same_span(&b.comments[i], &full->comments[i], 0));
	if (b.ncomments < full->ncomments)
		FUZZ_CHECK(b.truncated);
	else if (b.truncated)
		FUZZ_CHECK(full->truncated);
	else
		FUZZ_CHECK(b.unterminated == full->unterminated && !full->truncated);

	/* translation invariance: the same bytes in an exactly sized buffer */
	memcpy(copy, s + lo, hi - lo);
	scan_all(copy, 0, hi - lo, scs, SIZE_MAX, &moved);
	FUZZ_CHECK(moved.ncomments == full->ncomments &&
			   moved.truncated == full->truncated &&
			   moved.unterminated == full->unterminated);
	for (i = 0; i < moved.ncomments; i++)
		FUZZ_CHECK(same_span(&full->comments[i], &moved.comments[i], lo));
	free(copy);

	/* only_trivia agrees with the lex (exact unless the lex was truncated) */
	pssc_scan_reads = 0;
	trivia = pssc_scan_only_trivia(s, lo, hi);
	check_reads(3 * (unsigned long) (hi - lo) + 8);
	if (!full->truncated)
	{
		for (i = 0; i < full->ncomments; i++)
		{
			if (!only_bytes(s, p, full->comments[i].offset, true))
				gaps_trivial = false;
			p = full->comments[i].offset + full->comments[i].len;
		}
		if (!only_bytes(s, p, hi, true))
			gaps_trivial = false;
		FUZZ_CHECK(trivia == (gaps_trivial && !full->unterminated));
	}
}

static void
scan_pos(const char *s, size_t lo, size_t hi, PsscPosition pos, size_t w,
		 bool scs, PsscScanResult *r)
{
	size_t		len = hi - lo;
	bool		windowed = len > w && pos != PSSC_POS_ANY;

	memset(r, 0x5a, sizeof(*r));
	pssc_scan_reads = 0;
	pssc_scan_statement(s, lo, hi, pos, w, scs, r);
	check_reads(4 * (unsigned long) (windowed ? w : len) + 8);
	check_spans(s, lo, hi, r);
	FUZZ_CHECK(r->heuristic == (pos == PSSC_POS_APPEND && len > w));
	if (pos != PSSC_POS_ANY)
		FUZZ_CHECK(!r->unterminated && (!r->truncated || r->ncomments == MAXC));
}

/* index of span c in full, or -1 */
static int
find_span(const PsscScanResult *full, const PsscCommentSpan *c)
{
	int			i;

	for (i = 0; i < full->ncomments; i++)
		if (same_span(&full->comments[i], c, 0))
			return i;
	return -1;
}

/* r's spans are separated only by whitespace (and ';' if semi) */
static void
check_run_gaps(const char *s, const PsscScanResult *r, bool semi)
{
	int			i;

	for (i = 1; i < r->ncomments; i++)
		FUZZ_CHECK(only_bytes(s, r->comments[i - 1].offset + r->comments[i - 1].len,
							  r->comments[i].offset, semi));
}

/* append run checks against the exact full lex of [lo, hi) */
static void
check_append(const char *s, size_t hi, const PsscScanResult *r,
			 const PsscScanResult *full, bool exact)
{
	int			i,
				first;
	const PsscCommentSpan *last;

	for (i = 0; i < r->ncomments; i++)
		FUZZ_CHECK(is_block(s, &r->comments[i]));
	check_run_gaps(s, r, false);
	if (r->ncomments > 0)
	{
		last = &r->comments[r->ncomments - 1];
		FUZZ_CHECK(only_bytes(s, last->offset + last->len, hi, true));
	}
	if (!exact)
		FUZZ_CHECK(!r->truncated || r->ncomments == MAXC);
	if (!exact || full->truncated || full->unterminated)
		return;

	/* exact path on a completely lexed range: the maximal trailing run */
	if (r->ncomments == 0)
	{
		if (full->ncomments > 0)
		{
			last = &full->comments[full->ncomments - 1];
			FUZZ_CHECK(!is_block(s, last) ||
					   !only_bytes(s, last->offset + last->len, hi, true));
		}
		FUZZ_CHECK(!r->truncated);
		return;
	}
	first = find_span(full, &r->comments[0]);
	FUZZ_CHECK(first >= 0 && first + r->ncomments == full->ncomments);
	for (i = 0; i < r->ncomments; i++)
		FUZZ_CHECK(same_span(&r->comments[i], &full->comments[first + i], 0));
	if (first > 0)
	{
		const PsscCommentSpan *before = &full->comments[first - 1];
		bool		joins = is_block(s, before) &&
			only_bytes(s, before->offset + before->len, r->comments[0].offset, false);

		/* the comment before the run would extend it: only the cap stops it */
		FUZZ_CHECK(joins == r->truncated);
		FUZZ_CHECK(!joins || r->ncomments == MAXC);
	}
	else
		FUZZ_CHECK(!r->truncated);
}

/* prepend run checks; exact is the unwindowed result (NULL to compute it) */
static void
check_prepend(const char *s, size_t lo, size_t hi, size_t w,
			  const PsscScanResult *r, const PsscScanResult *full,
			  const PsscScanResult *exact)
{
	int			i,
				k;

	check_run_gaps(s, r, false);
	if (r->ncomments > 0)
		FUZZ_CHECK(only_bytes(s, lo, r->comments[0].offset, true));
	if (exact == NULL)
	{
		/* exact: the maximal leading run of a completely lexed range */
		if (full->unterminated)
			return;
		for (i = 0; i < r->ncomments; i++)
			FUZZ_CHECK(i < full->ncomments && same_span(&r->comments[i], &full->comments[i], 0));
		if (r->ncomments == 0)
			FUZZ_CHECK(full->ncomments == 0 ||
					   !only_bytes(s, lo, full->comments[0].offset, true));
		else if (r->ncomments < full->ncomments)
		{
			const PsscCommentSpan *a = &r->comments[r->ncomments - 1],
					   *b = &full->comments[r->ncomments];
			bool		joins = only_bytes(s, a->offset + a->len, b->offset, false);

			FUZZ_CHECK(joins == r->truncated);
		}
		else if (!full->truncated)
			FUZZ_CHECK(!r->truncated);
		return;
	}

	/* windowed: the prefix of the exact run whose comments end in the window */
	for (k = 0; k < exact->ncomments; k++)
		if (exact->comments[k].offset + exact->comments[k].len > lo + w)
			break;
	if (r->ncomments != k)
		fprintf(stderr, "prepend window %zu: %d comments, expected %d\n",
				w, r->ncomments, k);
	FUZZ_CHECK(r->ncomments == k);
	for (i = 0; i < k; i++)
		FUZZ_CHECK(same_span(&r->comments[i], &exact->comments[i], 0));
	FUZZ_CHECK(!r->truncated || exact->truncated);
	if (k == exact->ncomments && !exact->truncated)
		FUZZ_CHECK(!r->truncated);
	(void) hi;
}

/* true if s[lo, hi) cannot hide comment-like text in a literal */
static bool
tail_comparable(const char *s, size_t lo, size_t hi)
{
	for (; lo < hi; lo++)
		if (strchr("'\"$\\-", s[lo]) && s[lo] != '\0')
			return false;
	return true;
}

static void
check_statement(const char *s, size_t lo, size_t hi, bool scs, uint64_t h)
{
	size_t		len = hi - lo;
	size_t		windows[14];
	int			nw = 0,
				i;
	PsscScanResult full,
				exact_app,
				exact_pre,
				r;

	check_full(s, lo, hi, scs, (size_t) (h % (len + 1)), &full);

	/* exact (unwindowed) positional results */
	scan_pos(s, lo, hi, PSSC_POS_APPEND, SIZE_MAX, scs, &exact_app);
	check_append(s, hi, &exact_app, &full, true);
	scan_pos(s, lo, hi, PSSC_POS_PREPEND, SIZE_MAX, scs, &exact_pre);
	check_prepend(s, lo, hi, SIZE_MAX, &exact_pre, &full, NULL);

	windows[nw++] = 0;
	windows[nw++] = 1;
	windows[nw++] = 2;
	windows[nw++] = 5;
	windows[nw++] = 16;
	windows[nw++] = 64;
	windows[nw++] = len;
	windows[nw++] = len / 2;
	windows[nw++] = len > 0 ? len - 1 : 0;
	windows[nw++] = len > 2 ? len - 2 : 0;
	windows[nw++] = (size_t) ((h >> 17) % (len + 2));
	windows[nw++] = (size_t) ((h >> 37) % (len + 2));
	for (i = 0; i < nw; i++)
	{
		size_t		w = windows[i];
		PsscScanResult ref;

		scan_pos(s, lo, hi, PSSC_POS_ANY, w, scs, &r);
		pssc_scan_comments(s, lo, hi, scs, w, &ref);
		FUZZ_CHECK(r.ncomments == ref.ncomments && r.truncated == ref.truncated &&
				   r.unterminated == ref.unterminated &&
				   memcmp(r.comments, ref.comments, r.ncomments * sizeof(PsscCommentSpan)) == 0);

		scan_pos(s, lo, hi, PSSC_POS_APPEND, w, scs, &r);
		if (len <= w)
		{
			FUZZ_CHECK(r.ncomments == exact_app.ncomments &&
					   r.truncated == exact_app.truncated &&
					   memcmp(r.comments, exact_app.comments,
							  r.ncomments * sizeof(PsscCommentSpan)) == 0);
		}
		else
		{
			int			d = exact_app.ncomments - r.ncomments;
			int			k;

			check_append(s, hi, &r, &full, false);
			if (r.ncomments > 0)
				FUZZ_CHECK(r.comments[0].offset >= hi - w);
			/* without literals the tail path reports a suffix of the run */
			if (tail_comparable(s, lo, hi) && !full.unterminated && !full.truncated)
			{
				FUZZ_CHECK(d >= 0);
				for (k = 0; k < r.ncomments; k++)
					FUZZ_CHECK(same_span(&r.comments[k], &exact_app.comments[d + k], 0));
			}
		}

		scan_pos(s, lo, hi, PSSC_POS_PREPEND, w, scs, &r);
		if (len <= w)
			FUZZ_CHECK(r.ncomments == exact_pre.ncomments &&
					   r.truncated == exact_pre.truncated &&
					   memcmp(r.comments, exact_pre.comments,
							  r.ncomments * sizeof(PsscCommentSpan)) == 0);
		else
			check_prepend(s, lo, hi, w, &r, &full, &exact_pre);
	}
}

/* pssc_stmt_range(), pssc_stmt_owned_start() and pssc_scan_footer() */
static void
check_stmt_api(const char *z, size_t zlen, bool scs, uint64_t h)
{
	PsscStmtRange rg;
	int			loc = (h & 1) ? -1 : (int) ((h >> 3) % (zlen + 1));
	int			slen = (int) ((h >> 23) % (zlen + 1));
	size_t		from,
				owned,
				maxb;
	PsscPosition pos;

	if (loc >= 0 && (size_t) slen > zlen - (size_t) loc)
		slen = (int) (zlen - (size_t) loc);
	if ((h >> 41) % 4 == 0)
		slen = 0;
	rg = pssc_stmt_range(z, loc, slen);
	FUZZ_CHECK(rg.start <= rg.end && rg.end <= zlen);
	if (loc < 0)
		FUZZ_CHECK(rg.start == 0 && rg.end == zlen);
	else
		FUZZ_CHECK(rg.start == (size_t) loc &&
				   rg.end == (slen > 0 ? (size_t) loc + (size_t) slen : zlen));

	/* owned start: back over trivia to just after a ';' token */
	from = (h >> 29) % 3 == 0 ? (size_t) ((h >> 31) % (rg.start + 1)) : 0;
	maxb = (h >> 45) % 2 ? SIZE_MAX : (size_t) ((h >> 47) % (zlen + 2));
	pssc_scan_reads = 0;
	owned = pssc_stmt_owned_start(z, from, rg.start, maxb, scs);
	if (from < rg.start)
		check_reads(4 * (unsigned long) (rg.start - from) + 8);
	FUZZ_CHECK(owned <= rg.start);
	if (owned < rg.start)
	{
		FUZZ_CHECK(owned >= from && rg.start - from <= maxb);
		FUZZ_CHECK(owned == 0 || z[owned - 1] == ';');
		FUZZ_CHECK(pssc_scan_only_trivia(z, owned, rg.start));
	}

	/* footer after the statement, in every position */
	for (pos = PSSC_POS_ANY; pos <= PSSC_POS_PREPEND; pos++)
	{
		size_t		ws[3] = {SIZE_MAX, 64, (size_t) ((h >> 51) % (zlen + 2))};
		int			i;

		for (i = 0; i < 3; i++)
		{
			PsscScanResult r;
			size_t		rest = zlen - rg.end;

			memset(&r, 0x5a, sizeof(r));
			pssc_scan_footer(z, rg.end, pos, ws[i], scs, &r);
			check_spans(z, rg.end, zlen, &r);
			FUZZ_CHECK(!r.heuristic && !r.unterminated);
			if (r.ncomments > 0 || r.truncated)
				FUZZ_CHECK(rest <= ws[i] && pssc_scan_only_trivia(z, rg.end, zlen));
			if (pos == PSSC_POS_ANY && rest <= ws[i] &&
				pssc_scan_only_trivia(z, rg.end, zlen))
			{
				PsscScanResult ref;

				pssc_scan_comments(z, rg.end, zlen, scs, ws[i], &ref);
				FUZZ_CHECK(r.ncomments == ref.ncomments &&
						   memcmp(r.comments, ref.comments,
								  r.ncomments * sizeof(PsscCommentSpan)) == 0);
			}
			if (pos == PSSC_POS_APPEND)
				for (int k = 0; k < r.ncomments; k++)
					FUZZ_CHECK(is_block(z, &r.comments[k]));
			check_run_gaps(z, &r, true);
			if (pos != PSSC_POS_ANY)
				FUZZ_CHECK(!r.truncated || r.ncomments == MAXC);
		}
	}
}

FUZZ_ALPHABET("/*-'$\" \n\r;aEUXb&\\x\0")

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint64_t	h = hash64(data, size);
	char	   *s = malloc(size ? size : 1);
	char	   *z;
	size_t		zlen,
				a,
				b;
	int			scs;

	FUZZ_CHECK(s);
	memcpy(s, data, size);
	for (scs = 0; scs <= 1; scs++)
	{
		/* the whole input, in an exactly sized buffer */
		check_statement(s, 0, size, scs, h);
		/* a sub-range; scan.c must not read outside it */
		a = size ? (size_t) (h % (size + 1)) : 0;
		b = a + (size_t) ((h >> 11) % (size - a + 1));
		check_statement(s, a, b, scs, h >> 7);
	}
	free(s);

	/* NUL-terminated copy for the strlen()-based entry points */
	zlen = strnlen((const char *) data, size);
	z = malloc(zlen + 1);
	FUZZ_CHECK(z);
	memcpy(z, data, zlen);
	z[zlen] = '\0';
	check_stmt_api(z, zlen, true, h);
	check_stmt_api(z, zlen, false, h * 31 + 7);
	free(z);
	return 0;
}
