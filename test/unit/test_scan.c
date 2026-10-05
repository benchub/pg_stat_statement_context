/*
 * test_scan.c
 *		Standalone unit tests for src/scan.c (no server needed).
 *
 * Every case is run several ways, and all runs must agree:
 *	- with the input ending exactly at a PROT_NONE guard page, starting
 *	  exactly after one, and with start > 0 right after one, so any read
 *	  outside [start, end) faults;
 *	- embedded between hostile prefixes/suffixes (an open quote, an open
 *	  comment, ...) with start/end pointing at the case, so the result must
 *	  not depend on anything outside the range.
 * test/unit/Makefile builds it with -fsanitize=address,undefined twice: once
 * against the production scan.c, and once with -DPSSC_SCAN_CHECKED, which
 * makes scan.c assert start <= i < end on every byte read and count reads so
 * the driver can check the scan stays linear.
 *
 *	test_scan                     run all tests
 *	test_scan --emit-corpus DIR   write each case input to DIR (fuzz seeds)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "scan.h"

enum
{
	ON = 1,						/* standard_conforming_strings = on */
	OFF = 2,					/* standard_conforming_strings = off */
	BOTH = ON | OFF
};

#define MAXEXP (PSSC_SCAN_MAX_COMMENTS + 2)

typedef struct Case
{
	const char *name;
	const char *in;
	size_t		inlen;			/* 0: strlen(in); set for embedded NULs */
	int			scs;			/* which settings this expectation holds for */
	int			unterminated;
	const char *exp[MAXEXP];	/* expected comment texts, NULL-terminated */
	size_t		exp0len;		/* 0: strlen(exp[0]); set for embedded NULs */
} Case;

#define N NULL

static const Case cases[] = {
	/* ---- basics ---- */
	{"empty", "", 0, BOTH, 0, {N}},
	{"no comment", "SELECT 1", 0, BOTH, 0, {N}},
	{"trailing block", "SELECT 1 /*a*/", 0, BOTH, 0, {"/*a*/", N}},
	{"leading block", "/*a*/SELECT 1", 0, BOTH, 0, {"/*a*/", N}},
	{"several", "/*a*/SELECT/*b*/1--c", 0, BOTH, 0, {"/*a*/", "/*b*/", "--c", N}},
	{"sqlcommenter", "SELECT * FROM t /*action='show',controller='users'*/", 0, BOTH, 0,
	{"/*action='show',controller='users'*/", N}},
	{"line ends at LF", "SELECT 1 -- x\n, 2", 0, BOTH, 0, {"-- x", N}},
	{"line ends at CR", "-- x\r\nSELECT 1", 0, BOTH, 0, {"-- x", N}},
	{"line at end of range", "SELECT 1 --tail", 0, BOTH, 0, {"--tail", N}},
	{"empty line comment", "SELECT 1 --\n", 0, BOTH, 0, {"--", N}},
	{"bare --", "--", 0, BOTH, 0, {"--", N}},
	{"two line comments", "-- a\n-- b\nSELECT 1", 0, BOTH, 0, {"-- a", "-- b", N}},
	{"block opener inside line comment", "-- a /* b\nSELECT 1 */", 0, BOTH, 0, {"-- a /* b", N}},
	{"line opener inside block", "/* a -- b */ SELECT 1", 0, BOTH, 0, {"/* a -- b */", N}},
	{"quote inside block", "/* it's */ SELECT 1 /*b*/", 0, BOTH, 0, {"/* it's */", "/*b*/", N}},
	{"quote inside line", "-- it's\nSELECT 1 /*b*/", 0, BOTH, 0, {"-- it's", "/*b*/", N}},
	{"dollar inside block", "/* $$ */ SELECT 1 /*b*/", 0, BOTH, 0, {"/* $$ */", "/*b*/", N}},
	{"lone slash and dash", "SELECT 4 / 2 - 1", 0, BOTH, 0, {N}},

	/* ---- nested block comments ---- */
	{"nested", "/* a /* b */ c */ SELECT 1", 0, BOTH, 0, {"/* a /* b */ c */", N}},
	{"nested tight", "/*/**/*/x", 0, BOTH, 0, {"/*/**/*/", N}},
	{"nested 3 deep", "/*1/*2/*3*/2*/1*/ /*z*/", 0, BOTH, 0, {"/*1/*2/*3*/2*/1*/", "/*z*/", N}},
	{"slash after opener is not nesting", "/*/ x */ y", 0, BOTH, 0, {"/*/ x */", N}},
	{"empty block", "/**/", 0, BOTH, 0, {"/**/", N}},
	{"stars", "/***/ /** x **/", 0, BOTH, 0, {"/***/", "/** x **/", N}},
	{"close then star", "/* a */* b */", 0, BOTH, 0, {"/* a */", N}},
	{"star-slash-star closes", "/* a */*/ x", 0, BOTH, 0, {"/* a */", N}},

	/* ---- comments starting inside operator tokens (scan.l {operator}) ---- */
	{"slash-star after *", "SELECT 2*/*c*/3", 0, BOTH, 0, {"/*c*/", N}},
	{"dash-dash after +", "SELECT 1+--c\n2", 0, BOTH, 0, {"--c", N}},
	{"dash-dash after !=", "SELECT 3 !=--c\n2", 0, BOTH, 0, {"--c", N}},
	{"slash-star after -", "SELECT 1-/*c*/-2", 0, BOTH, 0, {"/*c*/", N}},
	{"slash-star after @", "SELECT @/*c*/-1", 0, BOTH, 0, {"/*c*/", N}},
	{"dash-dash after number", "SELECT 1--c\n", 0, BOTH, 0, {"--c", N}},
	{"slash-star after identifier", "SELECT a/*c*/", 0, BOTH, 0, {"/*c*/", N}},

	/* ---- plain strings ---- */
	{"comment text in string", "SELECT '/*x*/', '--y'", 0, BOTH, 0, {N}},
	{"comment after string", "SELECT 'a'/*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"doubled quotes", "SELECT 'a''/*x*/''b'", 0, BOTH, 0, {N}},
	{"only doubled quotes", "SELECT '''' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"empty string", "SELECT '' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"backslash-quote scs on", "SELECT 'a\\' /*x*/", 0, ON, 0, {"/*x*/", N}},
	{"backslash-quote scs off", "SELECT 'a\\' /*x*/", 0, OFF, 1, {N}},
	{"double backslash", "SELECT 'a\\\\' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	/* scs on: \ is literal and '' is doubling, so the string runs to the last ' */
	{"escaped quote then quote, on", "SELECT '\\'' /*x*/ '", 0, ON, 0, {N}},
	{"escaped quote then quote, off", "SELECT '\\'' /*x*/ '", 0, OFF, 1, {"/*x*/", N}},
	{"double quote in string", "SELECT 'a\"b' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"dollar in string", "SELECT '$$' /*x*/ '$$'", 0, BOTH, 0, {"/*x*/", N}},
	{"keyword glued to string", "SELECT'a'/*x*/", 0, BOTH, 0, {"/*x*/", N}},

	/* ---- E'' strings ---- */
	{"E-string backslash-quote", "SELECT E'a\\' /*x*/ '", 0, BOTH, 0, {N}},
	{"e-string lower case", "SELECT e'a\\' /*x*/ '", 0, BOTH, 0, {N}},
	{"E-string double backslash", "SELECT e'\\\\' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"E-string doubled quote", "SELECT E'a''/*x*/'", 0, BOTH, 0, {N}},
	/* '' keeps the E-string going; closing and reopening would start a plain one */
	{"E-string doubled quote then backslash", "SELECT E'a''\\' /*x*/ '", 0, ON, 0, {N}},
	{"E-string backslash-newline", "SELECT E'\\\n' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"E-string octal escape", "SELECT E'\\101' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"E-string after (", "SELECT (E'\\' /*x*/ ')", 0, BOTH, 0, {N}},
	{"xE is an identifier, on", "SELECT xE'a\\' /*x*/", 0, ON, 0, {"/*x*/", N}},
	{"xE is an identifier, off", "SELECT xE'a\\' /*x*/", 0, OFF, 1, {N}},
	/* PG14 (realfail1) lexes 1 then E'..'; PG15+ reject the trailing junk */
	{"E right after a number", "SELECT 1E'\\' /*x*/", 0, ON, 1, {N}},
	{"x right after a number, off", "SELECT 0x'\\' /*x*/", 0, OFF, 0, {"/*x*/", N}},
	{"number then identifier then string", "SELECT 1e5'\\' /*x*/ '", 0, OFF, 0, {N}},
	{"E-string backslash at end", "SELECT E'abc\\", 0, BOTH, 1, {N}},

	/* ---- U&'' strings (no backslash escapes in the lexer) ---- */
	{"U& string backslash", "SELECT U&'\\' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"u& string doubled quote", "SELECT u&'a''/*x*/'", 0, BOTH, 0, {N}},
	{"U& string with UESCAPE", "SELECT U&'d!0061t' UESCAPE '!' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"U& not adjacent, off", "SELECT U& '\\' /*x*/", 0, OFF, 1, {N}},
	{"U& space before &, off", "SELECT U &'\\' /*x*/", 0, OFF, 1, {N}},
	{"U then space then quote, off", "SELECT U '\\' /*x*/", 0, OFF, 1, {N}},
	{"xU& is identifier, off", "SELECT xU&'\\' /*x*/", 0, OFF, 1, {N}},
	{"U&& then string", "SELECT U&&'\\' /*x*/", 0, ON, 0, {"/*x*/", N}},

	/* ---- B'' / X'' / N'' strings ---- */
	{"B string no escapes, off", "SELECT B'\\' /*x*/", 0, OFF, 0, {"/*x*/", N}},
	{"X string no escapes, off", "SELECT x'\\' /*x*/", 0, OFF, 0, {"/*x*/", N}},
	{"abX is identifier, off", "SELECT abX'\\' /*x*/", 0, OFF, 1, {N}},
	{"B string: '' ends it, off", "SELECT B'01''\\' /*x*/'", 0, OFF, 0, {N}},
	{"B string: '' ends it, on", "SELECT B'01''\\' /*x*/'", 0, ON, 1, {"/*x*/", N}},
	{"N string is plain, off", "SELECT N'\\' /*x*/ '", 0, OFF, 0, {N}},
	{"N string is plain, on", "SELECT N'\\' /*x*/ '", 0, ON, 1, {"/*x*/", N}},
	{"B string comment text", "SELECT b'/*x*/'", 0, BOTH, 0, {N}},

	/* ---- string continuation (quote, whitespace with newline, quote) ---- */
	{"E continued by newline", "SELECT E'a'\n'\\' /*x*/ '", 0, BOTH, 0, {N}},
	{"E not continued without newline", "SELECT E'a' '\\' /*x*/ '", 0, ON, 1, {"/*x*/", N}},
	{"E continued across -- comment", "SELECT E'a' -- c\n'\\' /*x*/ '", 0, BOTH, 0, {"-- c", N}},
	{"E continued, blank lines and comments", "SELECT E'a'\n\n  -- c\n\t-- d\n '\\' /*x*/ '", 0, BOTH, 0,
	{"-- c", "-- d", N}},
	{"block comment breaks continuation", "SELECT E'a' /* c */\n'\\' /*x*/ '", 0, ON, 1,
	{"/* c */", "/*x*/", N}},
	{"E continued by CR", "SELECT E'a'\r'\\' /*x*/ '", 0, BOTH, 0, {N}},
	{"E continued with \\f and \\v", "SELECT E'a' \f\n\v'\\' /*x*/ '", 0, BOTH, 0, {N}},
	{"line comment at end of lookahead", "SELECT E'a' -- c", 0, BOTH, 0, {"-- c", N}},
	{"lookahead hits identifier", "SELECT E'a'\n x '\\' /*x*/ '", 0, ON, 1, {"/*x*/", N}},
	{"lookahead hits dash", "SELECT E'a'\n-1 /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"lookahead at end", "SELECT E'a'\n", 0, BOTH, 0, {N}},
	{"B continued, off", "SELECT B'0'\n'\\' /*x*/", 0, OFF, 0, {"/*x*/", N}},
	{"U& continued, off", "SELECT U&'a'\n'\\' /*x*/", 0, OFF, 0, {"/*x*/", N}},
	{"plain continued", "SELECT 'a'\n'b' /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"continued then doubled", "SELECT 'a'\n'b''/*x*/'", 0, BOTH, 0, {N}},

	/* ---- quoted identifiers ---- */
	{"comment in quoted ident", "SELECT 1 AS \"/*x*/\"", 0, BOTH, 0, {N}},
	{"doubled dquote", "SELECT 1 AS \"a\"\"/*x*/\"\"b\"", 0, BOTH, 0, {N}},
	{"no backslash escape in ident", "SELECT 1 AS \"a\\\" /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"U& quoted ident", "SELECT 1 AS U&\"/*x*/\"", 0, BOTH, 0, {N}},
	{"u& quoted ident doubled", "SELECT 1 AS u&\"a\"\"--x\" /*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"quote in quoted ident", "SELECT \"it's\" /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"unterminated quoted ident", "SELECT 1 AS \"abc /*x*/", 0, BOTH, 1, {N}},

	/* ---- dollar quotes, parameters, $ in identifiers ---- */
	{"empty-tag dollar quote", "SELECT $$ /*x*/ $$", 0, BOTH, 0, {N}},
	{"dollar quote then comment", "SELECT $$a$$ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"tagged dollar quote", "SELECT $tag$ $$ /*x*/ $tag$ /*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"nested tags", "SELECT $a$ $b$ /*x*/ $b$ $a$", 0, BOTH, 0, {N}},
	{"tag prefixes", "SELECT $abc$ $ab$ $abc /*x*/ $abc$ /*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"underscore tag", "SELECT $_$ x $_$/*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"tag with digit", "SELECT $q1$ /*x*/ $q1$ /*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"tags are case sensitive", "SELECT $A$ /*x*/ $a$ /*y*/", 0, BOTH, 1, {N}},
	{"multibyte tag", "SELECT $\xc3\xa9$ /*x*/ $\xc3\xa9$/*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"mismatched delim rescans its $", "SELECT $a$$b$a$/*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"close preceded by letters", "SELECT $a$ x$a$ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"lone $ inside dollar quote", "SELECT $$ $ /*x*/ $$ /*y*/", 0, BOTH, 0, {"/*y*/", N}},
	{"no backslash escapes in dollar quote", "SELECT $$\\$$ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"quote in dollar quote", "SELECT $$'$$ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"$1 parameter", "SELECT $1 /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	/* PG14 lexes $1 then E'..'; PG18 rejects "trailing junk after parameter" */
	{"$1 then E-string", "SELECT $1E'\\' /*x*/ '", 0, ON, 0, {N}},
	{"$1$ is not a delimiter", "SELECT $1$ /*x*/ $1$", 0, BOTH, 0, {"/*x*/", N}},
	{"$12 then $$", "SELECT $12$$ /*x*/ $$", 0, BOTH, 0, {N}},
	{"$ inside identifier", "SELECT a$b$ /*x*/ $b$ /*y*/", 0, BOTH, 1, {"/*x*/", N}},
	{"identifier ending in $$", "SELECT a$$ /*x*/ $$", 0, BOTH, 1, {"/*x*/", N}},
	{"identifier ending in $", "SELECT 1 AS x$ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"$tag without closing $", "SELECT $abc /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"$ then space", "SELECT $ $ /*x*/", 0, BOTH, 0, {"/*x*/", N}},
	{"digit then $$", "SELECT 1$$ /*x*/ $$", 0, BOTH, 0, {N}},
	{"space then $$", "SELECT a $$ /*x*/ $$", 0, BOTH, 0, {N}},
	{"unterminated dollar quote", "SELECT $a$ /*x*/ $a", 0, BOTH, 1, {N}},
	{"function body", "CREATE FUNCTION f() RETURNS int AS $body$ SELECT 1 /*in*/ $body$ LANGUAGE sql /*out*/",
	0, BOTH, 0, {"/*out*/", N}},

	/* ---- multibyte / high-bit bytes ---- */
	{"utf8 everywhere", "SELECT 'h\xc3\xa9llo' /* \xc3\xbcn\xc3\xaf \xe2\x9c\x93 */ -- \xe6\x97\xa5\xe6\x9c\xac\n",
	0, BOTH, 0, {"/* \xc3\xbcn\xc3\xaf \xe2\x9c\x93 */", "-- \xe6\x97\xa5\xe6\x9c\xac", N}},
	{"utf8 identifier then quote, on", "SELECT \xc3\xa9'\\' /*x*/", 0, ON, 0, {"/*x*/", N}},
	{"utf8 identifier then quote, off", "SELECT \xc3\xa9'\\' /*x*/", 0, OFF, 1, {N}},
	{"utf8 ident ending in E", "SELECT \xc3\xa9" "E'\\' /*x*/", 0, ON, 0, {"/*x*/", N}},
	{"4-byte utf8 in comment", "/*\xf0\x9f\x98\x80*/", 0, BOTH, 0, {"/*\xf0\x9f\x98\x80*/", N}},
	{"invalid utf8 bytes", "\xff\xfe /*\xff*/ \x80", 0, BOTH, 0, {"/*\xff*/", N}},
	{"utf8 in quoted ident", "SELECT \"\xc3\xa9\" /*x*/", 0, BOTH, 0, {"/*x*/", N}},

	/* ---- NUL bytes are ordinary characters ---- */
	{"NUL in comment", "SELECT 1 /*a\0b*/ x", 18, BOTH, 0, {"/*a\0b*/", N}, 7},
	{"NUL in string", "SELECT '\0' /*x*/", 16, BOTH, 0, {"/*x*/", N}},

	/* ---- unterminated constructs yield no span ---- */
	{"unterminated block", "SELECT 1 /* x", 0, BOTH, 1, {N}},
	{"unterminated nested", "SELECT 1 /* a /* b */", 0, BOTH, 1, {N}},
	{"complete then unterminated", "SELECT 1 /*a*/ /* b", 0, BOTH, 1, {"/*a*/", N}},
	{"bare opener", "/*", 0, BOTH, 1, {N}},
	{"opener then slash", "/*/", 0, BOTH, 1, {N}},
	{"opener then star", "/**", 0, BOTH, 1, {N}},
	{"unterminated string", "SELECT 'abc /*x*/", 0, BOTH, 1, {N}},
	{"unterminated E string", "SELECT E'abc /*x*/", 0, BOTH, 1, {N}},
	{"unterminated B string", "SELECT B'0", 0, BOTH, 1, {N}},
	{"unterminated U& string", "SELECT U&'a", 0, BOTH, 1, {N}},
	{"unterminated U& ident", "SELECT U&\"a", 0, BOTH, 1, {N}},
	{"unterminated empty-tag dollar", "SELECT $$abc /*x*/", 0, BOTH, 1, {N}},
	{"string closed at end", "SELECT 'a'", 0, BOTH, 0, {N}},
	{"trailing slash", "SELECT 1 /", 0, BOTH, 0, {N}},
	{"trailing dash", "SELECT 1 -", 0, BOTH, 0, {N}},
	{"trailing $", "SELECT $", 0, BOTH, 0, {N}},
	{"trailing U&", "SELECT U&", 0, BOTH, 0, {N}},
	{"trailing E", "SELECT E", 0, BOTH, 0, {N}},

	/* ---- multi-statement strings (ranges are -5's job; lexing is not) ---- */
	{"two statements", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, BOTH, 0, {"/*a*/", "/*b*/", N}},
	{"footer", "SELECT 1; SELECT 2; /*controller:x*/", 0, BOTH, 0, {"/*controller:x*/", N}},
};

/* pssc_scan_only_trivia() cases */
typedef struct TriviaCase
{
	const char *in;
	size_t		inlen;
	int			want;
} TriviaCase;

static const TriviaCase trivia_cases[] = {
	{"", 0, 1},
	{" ", 0, 1},
	{";", 0, 1},
	{" ;\n\t\f\v\r;; ", 0, 1},
	{"/*a*/", 0, 1},
	{"-- x", 0, 1},
	{"--", 0, 1},
	{"--\n", 0, 1},
	{"; -- x\n;/*b /*c*/ */ ;\n/*controller:x*/", 0, 1},
	{"-- 'unbalanced\n;", 0, 1},
	{"/* it's */", 0, 1},
	{"/*\xc3\xa9*/ ;", 0, 1},
	{"\x0b", 0, 1},
	{"/* x", 0, 0},
	{"/* a /* b */", 0, 0},
	{"/*", 0, 0},
	{"/", 0, 0},
	{"-", 0, 0},
	{"*/", 0, 0},
	{"x", 0, 0},
	{"; SELECT 1", 0, 0},
	{"'a'", 0, 0},
	{"$$ $$", 0, 0},
	{"/*a*/ 1", 0, 0},
	{"\"x\"", 0, 0},
	{";\0", 2, 0},
	{"\xc3\xa9", 0, 0},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))
#define NTRIVIA (sizeof(trivia_cases) / sizeof(trivia_cases[0]))

static int	failures;
static int	checks;

#define FAIL(...) \
	do { \
		failures++; \
		fprintf(stderr, "not ok: "); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} while (0)

/* ---------------- guard-paged buffers ---------------- */

static size_t pagesz;

typedef struct Guarded
{
	char	   *base;
	size_t		total;
	char	   *data;			/* first usable byte (right after low guard) */
	size_t		usable;
} Guarded;

static Guarded
guarded_alloc(size_t len)
{
	Guarded		g;
	size_t		npages = (len + pagesz - 1) / pagesz + 1;

	g.total = (npages + 2) * pagesz;
	g.base = mmap(NULL, g.total, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANON, -1, 0);
	if (g.base == MAP_FAILED)
	{
		perror("mmap");
		exit(2);
	}
	if (mprotect(g.base, pagesz, PROT_NONE) != 0 ||
		mprotect(g.base + g.total - pagesz, pagesz, PROT_NONE) != 0)
	{
		perror("mprotect");
		exit(2);
	}
	g.data = g.base + pagesz;
	g.usable = g.total - 2 * pagesz;
	return g;
}

static void
guarded_free(Guarded *g)
{
	munmap(g->base, g->total);
}

/* ---------------- scanning in several placements ---------------- */

#ifdef PSSC_SCAN_CHECKED
static double max_reads_per_byte;

static void
check_linear(const char *what, size_t len, unsigned long reads)
{
	/* at most 3 reads per byte, plus a little slack for tiny inputs */
	unsigned long limit = 3 * (unsigned long) len + 8;

	if (len >= 64 && (double) reads / len > max_reads_per_byte)
		max_reads_per_byte = (double) reads / len;
	if (getenv("SHOWRATIO") && len >= 64) fprintf(stderr, "%s %.2f\n", what, (double) reads / len);
	if (reads > limit)
		FAIL("%s: %lu byte reads for %zu bytes (limit %lu)", what, reads, len, limit);
}
#endif

static void
scan_at(const char *s, size_t start, size_t end, int scs_on, size_t budget,
		PsscScanResult *r, const char *what)
{
	memset(r, 0x7f, sizeof(*r));
#ifdef PSSC_SCAN_CHECKED
	pssc_scan_reads = 0;
#endif
	pssc_scan_comments(s, start, end, scs_on != 0, budget, r);
#ifdef PSSC_SCAN_CHECKED
	check_linear(what, end - start, pssc_scan_reads);
#else
	(void) what;
#endif
}

static int
same_result(const PsscScanResult *a, const PsscScanResult *b, size_t shift)
{
	int			i;

	if (a->ncomments != b->ncomments || a->truncated != b->truncated ||
		a->unterminated != b->unterminated)
		return 0;
	for (i = 0; i < a->ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
		if (a->comments[i].offset + shift != b->comments[i].offset ||
			a->comments[i].len != b->comments[i].len)
			return 0;
	return 1;
}

static const char *const prefixes[] = {"'", "/*", "$$", "E'\\", "\"", "x", "-", "U&", "$a"};
static const char *const suffixes[] = {"*/", "'", "\n'", "$$", "\"", "/", "-", "*", "$"};

/*
 * Scan in[0, len) with the input ending at a guard page (the reference
 * result), starting right after a guard page, and embedded between hostile
 * prefixes/suffixes; every variant must match the reference.
 */
static void
scan_everywhere(const char *name, const char *in, size_t len, int scs_on,
				size_t budget, PsscScanResult *ref)
{
	Guarded		g = guarded_alloc(len + 16);
	PsscScanResult r;
	char	   *s;
	size_t		p;

	/* input ends exactly where the high guard page begins */
	s = g.data + g.usable - len;
	memcpy(s, in, len);
	scan_at(s, 0, len, scs_on, budget, ref, name);

	/* input starts exactly where the low guard page ends */
	memcpy(g.data, in, len);
	scan_at(g.data, 0, len, scs_on, budget, &r, name);
	if (!same_result(ref, &r, 0))
		FAIL("%s: result differs when placed after the low guard page", name);

	/*
	 * Nonzero start: s points 7 bytes before the first readable byte, so
	 * reading below start faults.
	 */
	s = g.data - 7;
	scan_at(s, 7, 7 + len, scs_on, budget, &r, name);
	if (!same_result(ref, &r, 7))
		FAIL("%s: result differs with start = 7 after the low guard page", name);
	guarded_free(&g);

	for (p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++)
	{
		size_t		pl = strlen(prefixes[p]);
		size_t		sl = strlen(suffixes[p]);
		Guarded		h = guarded_alloc(pl + len + sl);

		s = h.data + h.usable - (pl + len + sl);
		memcpy(s, prefixes[p], pl);
		memcpy(s + pl, in, len);
		memcpy(s + pl + len, suffixes[p], sl);
		scan_at(s, pl, pl + len, scs_on, budget, &r, name);
		if (!same_result(ref, &r, pl))
			FAIL("%s: result depends on bytes outside the range (prefix \"%s\", suffix \"%s\")",
				 name, prefixes[p], suffixes[p]);
		guarded_free(&h);
	}
}

static void
print_result(const char *in, const PsscScanResult *r)
{
	int			i;

	fprintf(stderr, "    got %d comment(s), truncated=%d unterminated=%d\n",
			r->ncomments, (int) r->truncated, (int) r->unterminated);
	for (i = 0; i < r->ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
		fprintf(stderr, "      [%zu,+%zu) \"%.*s\"\n", r->comments[i].offset,
				r->comments[i].len, (int) r->comments[i].len,
				in + r->comments[i].offset);
}

static void
run_case(const Case *c)
{
	size_t		len = c->inlen ? c->inlen : strlen(c->in);
	int			scs;

	for (scs = ON; scs <= OFF; scs <<= 1)
	{
		PsscScanResult r;
		int			nexp = 0;
		int			ok = 1;
		int			i;
		const char *mode = scs == ON ? "on" : "off";

		if (!(c->scs & scs))
			continue;
		checks++;
		scan_everywhere(c->name, c->in, len, scs == ON, SIZE_MAX, &r);

		while (c->exp[nexp])
			nexp++;
		if (r.ncomments != nexp || r.truncated || r.unterminated != (c->unterminated != 0))
			ok = 0;
		for (i = 0; ok && i < nexp; i++)
		{
			size_t		el = (i == 0 && c->exp0len) ? c->exp0len : strlen(c->exp[i]);

			if (r.comments[i].len != el ||
				r.comments[i].offset > len || r.comments[i].len > len - r.comments[i].offset ||
				memcmp(c->in + r.comments[i].offset, c->exp[i], el) != 0)
				ok = 0;
		}
		if (!ok)
		{
			FAIL("case \"%s\" (scs %s): input \"%s\"", c->name, mode, c->in);
			fprintf(stderr, "    want %d comment(s), unterminated=%d:", nexp, c->unterminated);
			for (i = 0; i < nexp; i++)
				fprintf(stderr, " \"%s\"", c->exp[i]);
			fprintf(stderr, "\n");
			print_result(c->in, &r);
		}
	}
}

static void
run_trivia(const TriviaCase *t)
{
	size_t		len = t->inlen ? t->inlen : strlen(t->in);
	Guarded		g = guarded_alloc(len + 4);
	char	   *s;
	int			got,
				got2;
	size_t		p;

	checks++;
	s = g.data + g.usable - len;
	memcpy(s, t->in, len);
#ifdef PSSC_SCAN_CHECKED
	pssc_scan_reads = 0;
#endif
	got = pssc_scan_only_trivia(s, 0, len);
#ifdef PSSC_SCAN_CHECKED
	check_linear("only_trivia", len, pssc_scan_reads);
#endif
	memcpy(g.data, t->in, len);
	got2 = pssc_scan_only_trivia(g.data - 3, 3, 3 + len);
	if (got != t->want || got2 != got)
		FAIL("only_trivia(\"%s\") = %d / %d, want %d", t->in, got, got2, t->want);
	guarded_free(&g);

	for (p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++)
	{
		size_t		pl = strlen(prefixes[p]);
		size_t		sl = strlen(suffixes[p]);
		Guarded		h = guarded_alloc(pl + len + sl);

		s = h.data + h.usable - (pl + len + sl);
		memcpy(s, prefixes[p], pl);
		memcpy(s + pl, t->in, len);
		memcpy(s + pl + len, suffixes[p], sl);
		if (pssc_scan_only_trivia(s, pl, pl + len) != got)
			FAIL("only_trivia(\"%s\") depends on bytes outside the range", t->in);
		guarded_free(&h);
	}
}

/* ---------------- limits: 16 comments, scan_window budget ---------------- */

static void
expect_limits(const char *name, const char *in, size_t budget,
			  int want_n, int want_trunc, int want_unterm)
{
	PsscScanResult r;

	checks++;
	scan_everywhere(name, in, strlen(in), 1, budget, &r);
	if (r.ncomments != want_n || r.truncated != (want_trunc != 0) ||
		r.unterminated != (want_unterm != 0))
	{
		FAIL("limits \"%s\" (budget %zu): want n=%d truncated=%d unterminated=%d",
			 name, budget, want_n, want_trunc, want_unterm);
		print_result(in, &r);
	}
}

static void
run_limits(void)
{
	char		buf[1024];
	int			i;

	/* exactly 16 comments: all reported, not truncated */
	buf[0] = '\0';
	for (i = 0; i < 16; i++)
		strcat(buf, "/*c*/ x ");
	expect_limits("16 comments", buf, SIZE_MAX, 16, 0, 0);

	/* 16 complete comments, then text that only looks like comments */
	strcat(buf, "'/*no*/' \"--no\" $$/*no*/$$");
	expect_limits("16 comments + fake ones", buf, SIZE_MAX, 16, 0, 0);

	/* 16 complete comments, then an unterminated one: not truncated */
	buf[0] = '\0';
	for (i = 0; i < 16; i++)
		strcat(buf, "/*c*/ x ");
	strcat(buf, "/* open");
	expect_limits("16 comments + unterminated", buf, SIZE_MAX, 16, 0, 1);

	/* 17 and 40 comments: 16 reported, truncated */
	buf[0] = '\0';
	for (i = 0; i < 17; i++)
		strcat(buf, "-- c\n");
	expect_limits("17 comments", buf, SIZE_MAX, 16, 1, 0);
	for (i = 17; i < 40; i++)
		strcat(buf, "/*c*/");
	strcat(buf, "/* open");
	expect_limits("40 comments + unterminated", buf, SIZE_MAX, 16, 1, 0);

	/* the reported spans are the first 16, in order */
	{
		PsscScanResult r;

		buf[0] = '\0';
		for (i = 0; i < 20; i++)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "/*%02d*/", i);
		checks++;
		scan_everywhere("first 16 in order", buf, strlen(buf), 1, SIZE_MAX, &r);
		if (r.ncomments != 16 || !r.truncated)
			FAIL("first 16 in order: got %d truncated=%d", r.ncomments, (int) r.truncated);
		for (i = 0; i < r.ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
			if (r.comments[i].offset != (size_t) i * 6 || r.comments[i].len != 6)
				FAIL("first 16 in order: comment %d at [%zu,+%zu)", i,
					 r.comments[i].offset, r.comments[i].len);
	}

	/* scan_window budget: three 5-byte comments */
	strcpy(buf, "/*a*/ x /*b*/ y --cde\n");
	expect_limits("budget 15", buf, 15, 3, 0, 0);
	expect_limits("budget 14", buf, 14, 2, 1, 0);
	expect_limits("budget 10", buf, 10, 2, 1, 0);
	expect_limits("budget 9", buf, 9, 1, 1, 0);
	expect_limits("budget 5", buf, 5, 1, 1, 0);
	expect_limits("budget 4", buf, 4, 0, 1, 0);
	expect_limits("budget 0", buf, 0, 0, 1, 0);
	expect_limits("budget 0, no comments", "SELECT '/*x*/'", 0, 0, 0, 0);
	expect_limits("budget 0, unterminated", "SELECT 1 /* x", 0, 0, 0, 1);
	/* the budget stops at the first comment that does not fit */
	expect_limits("big comment first", "/* a long comment */ /*b*/", 10, 0, 1, 0);
	/* nested comments count once, by the outer span */
	expect_limits("nested counts once", "/*/*a*/*/", 9, 1, 0, 0);
	expect_limits("nested over budget", "/*/*a*/*/", 8, 0, 1, 0);
	/* a line comment's span excludes its newline */
	expect_limits("line excludes newline", "-- ab\n", 5, 1, 0, 0);
}

/* ---------------- linearity on pathological input ---------------- */

static void
run_pathological(void)
{
	size_t		n = 200000;
	char	   *buf = malloc(n + 64);
	PsscScanResult r;
	size_t		i,
				pos;

	if (!buf)
		exit(2);

	/* long dollar tag with many same-length near misses */
	{
		size_t		tag = 1000;

		pos = 0;
		buf[pos++] = '$';
		memset(buf + pos, 'a', tag);
		pos += tag;
		buf[pos++] = '$';
		while (pos + tag + 2 < n)
		{
			buf[pos++] = '$';
			memset(buf + pos, 'a', tag - 1);
			pos += tag - 1;
			buf[pos++] = 'b';
			buf[pos++] = '$';
		}
		checks++;
		scan_everywhere("long tag near misses", buf, pos, 1, SIZE_MAX, &r);
		if (r.ncomments != 0 || !r.unterminated)
			FAIL("long tag near misses: n=%d unterminated=%d", r.ncomments, (int) r.unterminated);
	}

	/* every other byte is a $ inside a dollar quote */
	for (i = 0; i < n; i++)
		buf[i] = (i % 2) ? 'x' : '$';
	memcpy(buf, "$q$", 3);
	checks++;
	scan_everywhere("many $ in dollar quote", buf, n, 1, SIZE_MAX, &r);
	if (r.ncomments != 0 || !r.unterminated)
		FAIL("many $ in dollar quote");

	/* quote-continuation lookahead over long whitespace, then failure */
	pos = 0;
	while (pos + 64 < n)
	{
		memcpy(buf + pos, "E'a'", 4);
		pos += 4;
		memset(buf + pos, ' ', 50);
		pos += 50;
		buf[pos++] = '\n';
		buf[pos++] = 'x';
	}
	checks++;
	scan_everywhere("failed continuations", buf, pos, 1, SIZE_MAX, &r);
	if (r.ncomments != 0 || r.unterminated)
		FAIL("failed continuations");

	/* successful continuations across long whitespace */
	pos = 0;
	memcpy(buf + pos, "E'a'", 4);
	pos += 4;
	while (pos + 64 < n)
	{
		buf[pos++] = '\n';
		memset(buf + pos, ' ', 50);
		pos += 50;
		memcpy(buf + pos, "'b'", 3);
		pos += 3;
	}
	checks++;
	scan_everywhere("successful continuations", buf, pos, 1, SIZE_MAX, &r);
	if (r.ncomments != 0 || r.unterminated)
		FAIL("successful continuations");

	/* deep nesting */
	for (i = 0; i + 1 < n; i += 2)
		memcpy(buf + i, "/*", 2);
	checks++;
	scan_everywhere("deep nesting", buf, n, 1, SIZE_MAX, &r);
	if (r.ncomments != 0 || !r.unterminated)
		FAIL("deep nesting");

	/* many short strings, identifiers, parameters and dollar quotes */
	for (i = 0; i + 10 <= n; i += 10)
		memcpy(buf + i, "'a'a$1$$x$", 10);
	checks++;
	scan_everywhere("token soup", buf, i, 0, SIZE_MAX, &r);

	free(buf);
}

/* ---------------- fuzz corpus ---------------- */

static int
emit_corpus(const char *dir)
{
	size_t		i;

	for (i = 0; i < NCASES; i++)
	{
		char		path[4096];
		FILE	   *f;
		size_t		len = cases[i].inlen ? cases[i].inlen : strlen(cases[i].in);

		snprintf(path, sizeof(path), "%s/case-%03zu%s.sql", dir, i,
				 cases[i].scs == OFF ? "-scs-off" : "");
		f = fopen(path, "wb");
		if (!f || fwrite(cases[i].in, 1, len, f) != len || fclose(f) != 0)
		{
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return 1;
		}
	}
	printf("wrote %zu files to %s\n", (size_t) NCASES, dir);
	return 0;
}

int
main(int argc, char **argv)
{
	size_t		i;

	if (argc == 3 && strcmp(argv[1], "--emit-corpus") == 0)
		return emit_corpus(argv[2]);
	if (argc != 1)
	{
		fprintf(stderr, "usage: %s [--emit-corpus DIR]\n", argv[0]);
		return 2;
	}

	pagesz = (size_t) sysconf(_SC_PAGESIZE);

	for (i = 0; i < NCASES; i++)
		run_case(&cases[i]);
	for (i = 0; i < NTRIVIA; i++)
		run_trivia(&trivia_cases[i]);
	run_limits();
	run_pathological();

#ifdef PSSC_SCAN_CHECKED
	printf("max byte reads per input byte (inputs >= 64 bytes): %.2f\n", max_reads_per_byte);
#endif
	if (failures)
	{
		printf("FAILED: %d failure(s) in %d checks\n", failures, checks);
		return 1;
	}
	printf("ok: %d checks passed (%zu lexer cases, %zu trivia cases)\n",
		   checks, (size_t) NCASES, (size_t) NTRIVIA);
	return 0;
}
