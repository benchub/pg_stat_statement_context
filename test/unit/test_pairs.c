/*
 * test_pairs.c
 *		Standalone unit tests for src/pairs.c, the SQLCommenter and marginalia
 *		pair parsers (no server needed).
 *
 * Every case is parsed several ways, and all runs must agree:
 *	- the body ending exactly at a PROT_NONE guard page and starting exactly
 *	  after one, with the pair array and the byte buffer sized exactly to the
 *	  expected output and ending at guard pages, so any read outside the body
 *	  or write outside the caller's storage faults;
 *	- the body followed by hostile bytes (an open quote, a backslash, a
 *	  partial %-escape, a separator, ...) that are outside len and must not
 *	  change the result;
 *	- with too little room (one pair fewer, one byte fewer, nothing at all),
 *	  where pairs must be dropped whole and counted, never cut.
 * test/unit/Makefile builds it with ASan/UBSan twice: against the production
 * pairs.c, and with -DPSSC_PAIRS_CHECKED, which bounds-checks and counts every
 * body read so the driver can check parsing is linear.
 *
 *	test_pairs                     run all tests
 *	test_pairs --emit-corpus DIR   write each case body to DIR (fuzz seeds)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pairs.h"

static int	failures;
static int	checks;

#define FAIL(...) \
	do { \
		failures++; \
		fprintf(stderr, "not ok: "); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} while (0)

/* ---------------- cases ---------------- */

typedef enum Fmt
{
	SC,							/* sqlcommenter, url_decode = on */
	SC_RAW,						/* sqlcommenter, url_decode = off */
	MG							/* marginalia (kv/ps: NULL = default opts) */
} Fmt;

typedef struct ExpPair
{
	const char *k;
	size_t		kl;
	const char *v;
	size_t		vl;
	unsigned int flags;
} ExpPair;

#define MAXEXP 8

typedef struct Case
{
	const char *name;
	Fmt			fmt;
	const char *kv;				/* MG: kv_sep, NULL = opts NULL (defaults) */
	size_t		kvl;
	const char *ps;				/* MG: pair_sep */
	size_t		psl;
	const char *in;
	size_t		inlen;
	size_t		nmalformed;
	ExpPair		exp[MAXEXP];	/* terminated by k == NULL */
} Case;

/* string literals only: lengths come from sizeof, so embedded NULs work */
#define IN(s)		s, sizeof(s) - 1
#define P(k, v)		{k, sizeof(k) - 1, v, sizeof(v) - 1, 0}
#define PF(k, v, f) {k, sizeof(k) - 1, v, sizeof(v) - 1, f}
#define END			{NULL, 0, NULL, 0, 0}
#define N_SEP		NULL, 0, NULL, 0
#define SEP(kv, ps) kv, sizeof(kv) - 1, ps, sizeof(ps) - 1

#define KNUL PSSC_PAIR_KEY_NUL
#define VNUL PSSC_PAIR_VALUE_NUL
#define BADE PSSC_PAIR_BAD_ESCAPE

#define TRACEPARENT "00-5bd66ef5095369c7b0d1f8f4bd33716a-c532cb4098ac3dd2-01"

static const Case cases[] = {
	/* ---- DESIGN.md §4.2 examples ---- */
	{"4.2 sqlcommenter", SC, N_SEP, IN("key='value',key2='value2'"), 0,
	{P("key", "value"), P("key2", "value2"), END}},
	{"4.2 marginalia", MG, N_SEP, IN("application:Foo,controller:users,action:show"), 0,
	{P("application", "Foo"), P("controller", "users"), P("action", "show"), END}},
	{"4.2 marginalia line with colons", MG, N_SEP, IN("controller:users,line:app/models/u.rb:12"), 0,
	{P("controller", "users"), P("line", "app/models/u.rb:12"), END}},
	{"4.2 sqlcommenter url-decoded", SC, N_SEP, IN("route='%2Fpolls%201000'"), 0,
	{P("route", "/polls 1000"), END}},

	/* ---- real-world SQLCommenter (spec exhibit, Rails QueryLogs, OTel) ---- */
	{"spec exhibit", SC, N_SEP,
		IN("action='%2Fparam*d',controller='index',framework='spring',"
		   "traceparent='" TRACEPARENT "',"
		   "tracestate='congo%3Dt61rcWkgMzE%2Crojo%3D00f067aa0ba902b7'"), 0,
		{P("action", "/param*d"), P("controller", "index"), P("framework", "spring"),
			P("traceparent", TRACEPARENT),
	P("tracestate", "congo=t61rcWkgMzE,rojo=00f067aa0ba902b7"), END}},
	/* the spec's own exhibit has a typo: controller='index,'framework=... */
	{"spec exhibit typo", SC, N_SEP,
		IN("action='%2Fparam*d',controller='index,'framework='spring',"
		   "traceparent='" TRACEPARENT "'"), 1,
	{P("action", "/param*d"), P("traceparent", TRACEPARENT), END}},
	{"spec multi-line parse example", SC, N_SEP,
		IN("action='%2Fparam*d',controller='index',framework='spring',\n"
		   "traceparent='" TRACEPARENT "',\ntracestate='congo%3Dt61rcWkgMzE'"), 0,
		{P("action", "/param*d"), P("controller", "index"), P("framework", "spring"),
	P("traceparent", TRACEPARENT), P("tracestate", "congo=t61rcWkgMzE"), END}},
	{"rails sqlcommenter", SC, N_SEP,
		IN("application='MyApp',controller='users',action='index',db_driver='pg'"), 0,
		{P("application", "MyApp"), P("controller", "users"), P("action", "index"),
	P("db_driver", "pg"), END}},
	{"django", SC, N_SEP,
		IN("controller='index',db_driver='django.db.backends.postgresql',"
		   "framework='django%3A4.2',route='%5Epolls/%24'"), 0,
		{P("controller", "index"), P("db_driver", "django.db.backends.postgresql"),
	P("framework", "django:4.2"), P("route", "^polls/$"), END}},

	/* ---- real-world marginalia / Rails QueryLogs legacy format ---- */
	{"marginalia line component", MG, N_SEP,
		IN("application:BCX,controller:project_imports,action:show,"
		   "line:/app/models/user.rb:12:in `block in <class:User>'"), 0,
		{P("application", "BCX"), P("controller", "project_imports"), P("action", "show"),
	P("line", "/app/models/user.rb:12:in `block in <class:User>'"), END}},
	{"marginalia job", MG, N_SEP, IN("application:Foo,job:ImportJob"), 0,
	{P("application", "Foo"), P("job", "ImportJob"), END}},
	{"marginalia keeps %-escapes", MG, N_SEP, IN("route:%2Fa%00b"), 0,
	{P("route", "%2Fa%00b"), END}},
	{"marginalia keeps quotes and backslashes", MG, N_SEP, IN("a:'x\\'"), 0,
	{P("a", "'x\\'"), END}},

	/* ---- with_annotation free text must not become pairs ---- */
	{"annotation sc", SC, N_SEP, IN("custom annotation"), 1, {END}},
	{"annotation mg", MG, N_SEP, IN("custom annotation"), 1, {END}},
	{"annotation one word mg", MG, N_SEP, IN("annotation"), 1, {END}},
	{"annotation prose with colon", MG, N_SEP, IN("Request from user: 5"), 1, {END}},
	{"annotation prose with equals", SC, N_SEP, IN("see x = 'y'"), 1, {END}},
	{"marginalia body to sqlcommenter", SC, N_SEP, IN("controller:users,action:show"), 2, {END}},
	{"sqlcommenter body to marginalia", MG, N_SEP, IN("controller='users',action='show'"), 2, {END}},

	/* ---- values containing colons / separators ---- */
	{"sc value with colons", SC, N_SEP, IN("line='app/models/u.rb:12'"), 0,
	{P("line", "app/models/u.rb:12"), END}},
	{"sc quoted comma", SC, N_SEP, IN("a='x,y',b='z'"), 0,
	{P("a", "x,y"), P("b", "z"), END}},
	{"sc equals in value", SC, N_SEP, IN("a='x=y'"), 0, {P("a", "x=y"), END}},
	{"mg empty value then colon", MG, N_SEP, IN("a::b"), 0, {P("a", ":b"), END}},

	/* ---- escaped quotes ---- */
	{"escaped quote", SC, N_SEP, IN("name='O\\'Brien',x='1'"), 0,
	{P("name", "O'Brien"), P("x", "1"), END}},
	{"escaped quote raw", SC_RAW, N_SEP, IN("name='O\\'Brien'"), 0,
	{P("name", "O'Brien"), END}},
	{"escaped quote then comma", SC, N_SEP, IN("k='a\\',b',c='d'"), 0,
	{P("k", "a',b"), P("c", "d"), END}},
	{"escaped backslash ends value", SC, N_SEP, IN("k='a\\\\',c='d'"), 0,
	{P("k", "a\\"), P("c", "d"), END}},
	{"escaped backslash then escaped quote", SC, N_SEP, IN("k='\\\\\\''"), 0,
	{P("k", "\\'"), END}},
	{"other backslash kept", SC, N_SEP, IN("k='a\\d\\%41'"), 0,
	{P("k", "a\\d\\A"), END}},
	{"only escaped quote", SC, N_SEP, IN("k='\\''"), 0, {P("k", "'"), END}},
	{"escaped quote in key", SC, N_SEP, IN("it\\'s='x'"), 0, {P("it's", "x"), END}},
	{"url-encoded quote", SC, N_SEP, IN("k='%27a%27'"), 0, {P("k", "'a'"), END}},
	/* decoded bytes are not unescaped again */
	{"unescape before decode", SC, N_SEP, IN("k='%5C%27'"), 0, {P("k", "\\'"), END}},
	{"doubled quote is not an escape", SC, N_SEP, IN("a='x''y',b='z'"), 1, {P("b", "z"), END}},

	/* ---- %-escapes ---- */
	{"pct basic", SC, N_SEP, IN("k='%41%62%2c%2C%7e'"), 0, {P("k", "Ab,,~"), END}},
	{"pct mixed case", SC, N_SEP, IN("k='%aF%Af'"), 0, {P("k", "\xaf\xaf"), END}},
	{"pct utf-8", SC, N_SEP, IN("k='%E2%9C%93'"), 0, {P("k", "\xe2\x9c\x93"), END}},
	{"pct invalid hex", SC, N_SEP, IN("k='%zz'"), 0, {PF("k", "%zz", BADE), END}},
	{"pct one digit", SC, N_SEP, IN("k='%4'"), 0, {PF("k", "%4", BADE), END}},
	{"pct one digit then non-hex", SC, N_SEP, IN("k='%4g1'"), 0, {PF("k", "%4g1", BADE), END}},
	{"pct trailing", SC, N_SEP, IN("k='100%'"), 0, {PF("k", "100%", BADE), END}},
	{"pct percent percent", SC, N_SEP, IN("k='%%41'"), 0, {PF("k", "%A", BADE), END}},
	{"pct encoded percent", SC, N_SEP, IN("k='%2541'"), 0, {P("k", "%41"), END}},
	{"pct bad flag per pair", SC, N_SEP, IN("a='%',b='%20'"), 0,
	{PF("a", "%", BADE), P("b", " "), END}},
	{"pct 00 kept and flagged", SC, N_SEP, IN("k='a%00b',x='y'"), 0,
	{PF("k", "a\0b", VNUL), P("x", "y"), END}},
	{"pct 00 only", SC, N_SEP, IN("k='%00'"), 0, {PF("k", "\0", VNUL), END}},
	{"pct 00 in key", SC, N_SEP, IN("k%00x='v'"), 0, {PF("k\0x", "v", KNUL), END}},
	{"pct 00 key and value", SC, N_SEP, IN("%00='%00'"), 0,
	{PF("\0", "\0", KNUL | VNUL), END}},
	{"pct 00 and bad escape", SC, N_SEP, IN("k='%00%g'"), 0,
	{PF("k", "\0%g", VNUL | BADE), END}},
	/* '+' is a space (form encoding: Go url.QueryEscape, Java URLEncoder); %2B is '+' */
	{"plus is space", SC, N_SEP, IN("k='a+b%2Bc'"), 0, {P("k", "a b+c"), END}},
	{"plus only", SC, N_SEP, IN("k='+'"), 0, {P("k", " "), END}},
	{"plus in key", SC, N_SEP, IN("route+parameter='x'"), 0, {P("route parameter", "x"), END}},
	{"plus after bad escape", SC, N_SEP, IN("k='%+'"), 0, {PF("k", "% ", BADE), END}},
	{"go emitter (url.QueryEscape)", SC, N_SEP,
		IN("application='My+App',db_driver='database%2Fsql',route='%2Fpolls%2F%7Bid%7D+edit',"
		   "x='a%2Bb'"), 0,
		{P("application", "My App"), P("db_driver", "database/sql"),
	P("route", "/polls/{id} edit"), P("x", "a+b"), END}},
	{"java emitter (URLEncoder)", SC, N_SEP,
		IN("action='%2Fparam+first',controller='index',framework='spring',x='C%2B%2B'"), 0,
		{P("action", "/param first"), P("controller", "index"), P("framework", "spring"),
	P("x", "C++"), END}},
	{"python emitter (urllib quote)", SC, N_SEP,
		IN("route='%2Fpolls%201000',x='a%2Bb'"), 0,
	{P("route", "/polls 1000"), P("x", "a+b"), END}},
	{"node emitter (encodeURIComponent)", SC, N_SEP,
		IN("route='%2Fusers%2F%3Aid',x='My%20App%2B'"), 0,
	{P("route", "/users/:id"), P("x", "My App+"), END}},
	{"raw keeps plus", SC_RAW, N_SEP, IN("k+1='a+b%2B'"), 0, {P("k+1", "a+b%2B"), END}},
	{"pct key decoded", SC, N_SEP, IN("route%20parameter='x'"), 0,
	{P("route parameter", "x"), END}},
	{"pct key bad escape", SC, N_SEP, IN("a%zb='x'"), 0, {PF("a%zb", "x", BADE), END}},
	{"pct encoded whitespace kept", SC, N_SEP, IN("k='%20x%09'"), 0, {P("k", " x\t"), END}},
	{"raw NUL in sc value", SC, N_SEP, IN("k='a\0b'"), 0, {PF("k", "a\0b", VNUL), END}},
	{"raw NUL in mg value", MG, N_SEP, IN("k:a\0b"), 0, {PF("k", "a\0b", VNUL), END}},
	{"raw NUL in mg key", MG, N_SEP, IN("k\0:b"), 0, {PF("k\0", "b", KNUL), END}},

	/* ---- url_decode = off ---- */
	{"raw keeps escapes", SC_RAW, N_SEP, IN("route='%2Fpolls%201000',x='%00'"), 0,
	{P("route", "%2Fpolls%201000"), P("x", "%00"), END}},
	{"raw keeps bad escapes unflagged", SC_RAW, N_SEP, IN("k='%zz%'"), 0, {P("k", "%zz%"), END}},
	{"raw key not decoded", SC_RAW, N_SEP, IN("a%20b='v'"), 0, {P("a%20b", "v"), END}},
	{"raw raw NUL still flagged", SC_RAW, N_SEP, IN("k='\0'"), 0, {PF("k", "\0", VNUL), END}},

	/* ---- empty keys and values ---- */
	{"sc empty key", SC, N_SEP, IN("='v'"), 1, {END}},
	{"sc empty key then pair", SC, N_SEP, IN("='v',k='w'"), 1, {P("k", "w"), END}},
	{"sc whitespace key", SC, N_SEP, IN("  ='v'"), 1, {END}},
	{"sc empty value", SC, N_SEP, IN("k=''"), 0, {P("k", ""), END}},
	{"sc empty value then pair", SC, N_SEP, IN("k='',j='x'"), 0, {P("k", ""), P("j", "x"), END}},
	{"sc empty key and value", SC, N_SEP, IN("=''"), 1, {END}},
	{"sc missing value", SC, N_SEP, IN("k="), 1, {END}},
	{"mg empty key", MG, N_SEP, IN(":v"), 1, {END}},
	{"mg empty key then pair", MG, N_SEP, IN(":v,k:w"), 1, {P("k", "w"), END}},
	{"mg empty value", MG, N_SEP, IN("k:"), 0, {P("k", ""), END}},
	{"mg empty value trailing space", MG, N_SEP, IN("k: ,j:x"), 0, {P("k", ""), P("j", "x"), END}},
	{"mg separator only", MG, N_SEP, IN(":"), 1, {END}},

	/* ---- empty input and empty segments ---- */
	{"sc empty body", SC, N_SEP, IN(""), 0, {END}},
	{"mg empty body", MG, N_SEP, IN(""), 0, {END}},
	{"sc whitespace body", SC, N_SEP, IN(" \t\n\r\f\v"), 0, {END}},
	{"mg whitespace body", MG, N_SEP, IN(" \t\n\r\f\v"), 0, {END}},
	{"sc empty segments", SC, N_SEP, IN(",a='1',, ,b='2',"), 0, {P("a", "1"), P("b", "2"), END}},
	{"mg empty segments", MG, N_SEP, IN(",a:1,, ,b:2,"), 0, {P("a", "1"), P("b", "2"), END}},
	{"sc only commas", SC, N_SEP, IN(",,,"), 0, {END}},

	/* ---- whitespace trimming ---- */
	{"sc trims", SC, N_SEP, IN("  a = 'b' ,\n\tc='d'  \n"), 0, {P("a", "b"), P("c", "d"), END}},
	{"sc keeps whitespace in quotes", SC, N_SEP, IN("a=' x '"), 0, {P("a", " x "), END}},
	{"mg trims", MG, N_SEP, IN(" a : b c , d:e\n"), 0, {P("a", "b c"), P("d", "e"), END}},
	{"sc key with inner space", SC, N_SEP, IN("my key='v',k='w'"), 1, {P("k", "w"), END}},
	{"mg key with inner space", MG, N_SEP, IN("my key:v,k:w"), 1, {P("k", "w"), END}},
	{"mg key with tab", MG, N_SEP, IN("my\tkey:v"), 1, {END}},
	{"non-ascii whitespace not trimmed", MG, N_SEP, IN("\xc2\xa0k:v"), 0,
	{P("\xc2\xa0k", "v"), END}},

	/* ---- malformed sqlcommenter pairs ---- */
	{"sc unquoted", SC, N_SEP, IN("k=v"), 1, {END}},
	{"sc unquoted then pair", SC, N_SEP, IN("k=v,a='b'"), 1, {P("a", "b"), END}},
	{"sc double-quoted", SC, N_SEP, IN("k=\"v\",a='b'"), 1, {P("a", "b"), END}},
	{"sc junk after value", SC, N_SEP, IN("a='1' x,b='2'"), 1, {P("b", "2"), END}},
	{"sc first equals splits", SC, N_SEP, IN("a=b='c'"), 1, {END}},
	{"sc junk with quoted comma", SC, N_SEP, IN("a='x'j='y,z',b='w'"), 2, {P("b", "w"), END}},
	{"sc unterminated", SC, N_SEP, IN("b='2',a='oops"), 1, {P("b", "2"), END}},
	{"sc unterminated resumes", SC, N_SEP, IN("a='oops,b=2,c=3"), 3, {END}},
	/* the closing quote search runs to the end once; later quotes are all escaped */
	{"sc closing quote after junk", SC, N_SEP, IN("a='oops,b=2,c='3"), 1, {END}},
	{"sc escaped quote shifts closing quote", SC, N_SEP, IN("a='x\\',b='y\\'"), 1, {END}},
	{"sc unterminated then marginalia", SC, N_SEP, IN("a='1,b:2"), 2, {END}},
	{"sc lone quote", SC, N_SEP, IN("'"), 1, {END}},
	{"sc lone backslash", SC, N_SEP, IN("k='\\"), 1, {END}},

	/* ---- custom kv_sep / pair_sep ---- */
	{"custom = and space", MG, SEP("=", " "), IN("svc=billing op=charge"), 0,
	{P("svc", "billing"), P("op", "charge"), END}},
	{"custom = and space, runs of spaces", MG, SEP("=", " "), IN("  svc=billing   op=charge "), 0,
	{P("svc", "billing"), P("op", "charge"), END}},
	{"custom multi-byte seps", MG, SEP("=>", ";"), IN("a=>1;b=>2=>3"), 0,
	{P("a", "1"), P("b", "2=>3"), END}},
	{"custom pipe pair_sep keeps commas", MG, SEP(":", "|"), IN("a:1,2|b:3"), 0,
	{P("a", "1,2"), P("b", "3"), END}},
	{"custom two-byte pair_sep", MG, SEP(":", ",,"), IN("a:1,b:2,,c:3"), 0,
	{P("a", "1,b:2"), P("c", "3"), END}},
	{"custom pair_sep partial at end", MG, SEP(":", ";;"), IN("a:1;"), 0, {P("a", "1;"), END}},
	{"custom kv_sep partial", MG, SEP("::", ","), IN("a:b::c,d:e"), 1, {P("a:b", "c"), END}},
	{"custom seps equal", MG, SEP(":", ":"), IN("a:1"), 2, {END}},
	{"custom default chars rejected", MG, SEP("=", ";"), IN("a:1,b:2"), 1, {END}},
	{"custom empty seps use defaults", MG, SEP("", ""), IN("a:1,b:2"), 0,
	{P("a", "1"), P("b", "2"), END}},
	{"custom overlong seps use defaults", MG, SEP("123456789", "123456789"), IN("a:1,b:2"), 0,
	{P("a", "1"), P("b", "2"), END}},
	/* separator overlap: pair boundaries are found first, kv_sep only within a segment */
	{"overlap pair_sep inside kv_sep", MG, SEP("=>", ">"), IN("a=>b"), 2, {END}},
	{"overlap pair_sep inside kv_sep, every one splits", MG, SEP("=>", ">"), IN("a=>b>c=d=>e"), 4, {END}},
	{"overlap pair_sep straddles kv_sep end", MG, SEP("=>", ">,"), IN("a=>,b=>c"), 1,
	{P("b", "c"), END}},
	{"overlap kv_sep inside pair_sep", MG, SEP(">", "=>"), IN("a>1=>b>2"), 0,
	{P("a", "1"), P("b", "2"), END}},
	{"overlap kv_sep prefix of pair_sep", MG, SEP(";", ";;"), IN("a;1;;b;2"), 0,
	{P("a", "1"), P("b", "2"), END}},
	{"overlap pair_sep prefix of kv_sep", MG, SEP("::", ":"), IN("a::b"), 2, {END}},
	{"custom NUL pair_sep", MG, SEP(":", "\0"), IN("a:1\0b:2"), 0, {P("a", "1"), P("b", "2"), END}},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

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

/* the last n bytes before the high guard page */
static char *
guarded_tail(Guarded *g, size_t n)
{
	return g->data + g->usable - n;
}

/* ---------------- running a parser ---------------- */

static size_t
case_len(const Case *c)
{
	return c->inlen;
}

static size_t
case_nexp(const Case *c)
{
	size_t		n = 0;

	while (n < MAXEXP && c->exp[n].k)
		n++;
	return n;
}

static size_t
case_expbytes(const Case *c)
{
	size_t		i,
				n = 0;

	for (i = 0; i < case_nexp(c); i++)
		n += c->exp[i].kl + c->exp[i].vl;
	return n;
}

#ifdef PSSC_PAIRS_CHECKED
static double max_reads_per_byte;

static void
check_linear(const char *what, size_t len, unsigned long reads)
{
	unsigned long limit = 3 * (unsigned long) len + 8;

	if (len >= 64 && (double) reads / len > max_reads_per_byte)
		max_reads_per_byte = (double) reads / len;
	if (reads > limit)
		FAIL("%s: %lu byte reads for %zu bytes (limit %lu)", what, reads, len, limit);
}
#endif

static void
parse_opts(Fmt fmt, const PsscMarginaliaOpts *opts, const char *body, size_t len,
		   const PsscPairOut *out, PsscPairResult *r)
{
	/* poison the result so a parser that leaves a field unset is caught */
	memset(r, 0xA5, sizeof(*r));
#ifdef PSSC_PAIRS_CHECKED
	pssc_pairs_reads = 0;
#endif
	if (fmt == MG)
		pssc_parse_marginalia(body, len, opts, out, r);
	else
		pssc_parse_sqlcommenter(body, len, fmt == SC, out, r);
#ifdef PSSC_PAIRS_CHECKED
	check_linear("parse", len, pssc_pairs_reads);
#endif
}

static void
parse_case(const Case *c, const char *body, const PsscPairOut *out,
		   PsscPairResult *r)
{
	PsscMarginaliaOpts o;

	o.kv_sep = c->kv;
	o.kv_sep_len = c->kvl;
	o.pair_sep = c->ps;
	o.pair_sep_len = c->psl;
	parse_opts(c->fmt, c->kv ? &o : NULL, body, case_len(c), out, r);
}

/* Every reported pair lies in out->buf, inside bufused, and bufused fits. */
static void
check_storage(const char *name, const PsscPairOut *out, const PsscPairResult *r)
{
	size_t		i;

	checks++;
	if (r->npairs > out->max_pairs)
		FAIL("%s: npairs %zu > max_pairs %zu", name, r->npairs, out->max_pairs);
	if (r->bufused > out->bufsize)
		FAIL("%s: bufused %zu > bufsize %zu", name, r->bufused, out->bufsize);
	for (i = 0; i < r->npairs && i < out->max_pairs; i++)
	{
		const PsscPair *p = &out->pairs[i];

		if ((p->keylen && (p->key < out->buf || p->key + p->keylen > out->buf + r->bufused)) ||
			(p->valuelen && (p->value < out->buf || p->value + p->valuelen > out->buf + r->bufused)))
			FAIL("%s: pair %zu points outside the used buffer", name, i);
	}
}

static bool
pair_equal(const PsscPair *p, const ExpPair *e)
{
	return p->keylen == e->kl && memcmp(p->key, e->k, e->kl) == 0 &&
		p->valuelen == e->vl && memcmp(p->value, e->v, e->vl) == 0 &&
		p->flags == e->flags;
}

static void
show_pairs(const PsscPairOut *out, const PsscPairResult *r)
{
	size_t		i;

	fprintf(stderr, "    got npairs=%zu nmalformed=%zu ndropped=%zu bufused=%zu\n",
			r->npairs, r->nmalformed, r->ndropped, r->bufused);
	for (i = 0; i < r->npairs && i < out->max_pairs && i < 16; i++)
		fprintf(stderr, "    [%zu] \"%.*s\" = \"%.*s\" flags=%u\n", i,
				(int) out->pairs[i].keylen, out->pairs[i].key,
				(int) out->pairs[i].valuelen, out->pairs[i].value,
				out->pairs[i].flags);
}

/* Full match against the case's expectation (enough room for everything). */
static void
check_exact(const Case *c, const char *how, const PsscPairOut *out,
			const PsscPairResult *r)
{
	size_t		nexp = case_nexp(c);
	size_t		i;
	bool		ok = true;

	checks++;
	check_storage(c->name, out, r);
	if (r->npairs != nexp || r->nmalformed != c->nmalformed || r->ndropped != 0 ||
		r->bufused != case_expbytes(c))
		ok = false;
	for (i = 0; ok && i < nexp; i++)
		if (!pair_equal(&out->pairs[i], &c->exp[i]))
			ok = false;
	if (!ok)
	{
		FAIL("%s (%s): expected %zu pairs, %zu malformed, %zu bytes", c->name, how,
			 nexp, c->nmalformed, case_expbytes(c));
		show_pairs(out, r);
	}
}

/*
 * Too little room: the result must be a subsequence of the expected pairs,
 * with npairs + ndropped = expected, and the same malformed count.
 */
static void
check_partial(const Case *c, const char *how, const PsscPairOut *out,
			  const PsscPairResult *r, size_t min_dropped)
{
	size_t		nexp = case_nexp(c);
	size_t		i,
				j = 0;
	bool		ok = true;

	checks++;
	check_storage(c->name, out, r);
	if (r->npairs + r->ndropped != nexp || r->nmalformed != c->nmalformed ||
		r->ndropped < min_dropped)
		ok = false;
	for (i = 0; ok && i < r->npairs; i++)
	{
		while (j < nexp && !pair_equal(&out->pairs[i], &c->exp[j]))
			j++;
		if (j == nexp)
			ok = false;
		j++;
	}
	if (!ok)
	{
		FAIL("%s (%s): bad partial result (expected %zu pairs, >= %zu dropped)",
			 c->name, how, nexp, min_dropped);
		show_pairs(out, r);
	}
}

static const char *const hostile_suffixes[] = {
	"'", "\\", "%", "%0", "%00", ",", ":", "='a'", ":x", "\\'", "''", " ", ";", "=>",
};

static void
run_case(const Case *c)
{
	size_t		len = case_len(c);
	size_t		nexp = case_nexp(c);
	size_t		nbytes = case_expbytes(c);
	Guarded		gin = guarded_alloc(len + 16);
	Guarded		gpairs = guarded_alloc((nexp + 1) * sizeof(PsscPair));
	Guarded		gbuf = guarded_alloc(nbytes + 1);
	PsscPairOut out;
	PsscPairResult r;
	PsscPair	big[64];
	char		bigbuf[512];
	size_t		i;

	/* exact room, everything ending at guard pages */
	out.pairs = (PsscPair *) guarded_tail(&gpairs, nexp * sizeof(PsscPair));
	out.max_pairs = nexp;
	out.buf = guarded_tail(&gbuf, nbytes);
	out.bufsize = nbytes;

	memcpy(guarded_tail(&gin, len), c->in, len);
	parse_case(c, guarded_tail(&gin, len), &out, &r);
	check_exact(c, "body at high guard, exact room", &out, &r);

	memcpy(gin.data, c->in, len);
	parse_case(c, gin.data, &out, &r);
	check_exact(c, "body after low guard, exact room", &out, &r);

	/* plenty of room */
	out.pairs = big;
	out.max_pairs = 64;
	out.buf = bigbuf;
	out.bufsize = sizeof(bigbuf);
	parse_case(c, gin.data, &out, &r);
	check_exact(c, "plenty of room", &out, &r);

	/* a buffer of len bytes always suffices */
	out.buf = guarded_tail(&gin, len);	/* reuse: body is at gin.data */
	out.bufsize = len;
	if (len + 16 + len <= gin.usable)
	{
		parse_case(c, gin.data, &out, &r);
		check_exact(c, "buffer of len bytes", &out, &r);
	}
	out.buf = bigbuf;
	out.bufsize = sizeof(bigbuf);

	/* bytes after the body must not matter */
	for (i = 0; i < sizeof(hostile_suffixes) / sizeof(hostile_suffixes[0]); i++)
	{
		const char *h = hostile_suffixes[i];
		size_t		hl = strlen(h);
		Guarded		g = guarded_alloc(len + hl);
		char		how[64];

		memcpy(guarded_tail(&g, len + hl), c->in, len);
		memcpy(guarded_tail(&g, hl), h, hl);
		parse_case(c, guarded_tail(&g, len + hl), &out, &r);
		snprintf(how, sizeof(how), "followed by \"%s\"", h);
		check_exact(c, how, &out, &r);
		guarded_free(&g);
	}

	/* too little room */
	if (nexp > 0)
	{
		out.pairs = (PsscPair *) guarded_tail(&gpairs, (nexp - 1) * sizeof(PsscPair));
		out.max_pairs = nexp - 1;
		out.buf = guarded_tail(&gbuf, nbytes);
		out.bufsize = nbytes;
		parse_case(c, gin.data, &out, &r);
		check_partial(c, "one pair fewer", &out, &r, 1);
		/* the pair array fills in order: exactly the first nexp - 1 pairs */
		checks++;
		for (i = 0; i < r.npairs; i++)
			if (r.npairs != nexp - 1 || !pair_equal(&out.pairs[i], &c->exp[i]))
			{
				FAIL("%s: one pair fewer did not keep the first pairs", c->name);
				break;
			}

		out.pairs = (PsscPair *) guarded_tail(&gpairs, nexp * sizeof(PsscPair));
		out.max_pairs = nexp;
		if (nbytes > 0)
		{
			out.buf = guarded_tail(&gbuf, nbytes - 1);
			out.bufsize = nbytes - 1;
			parse_case(c, gin.data, &out, &r);
			check_partial(c, "one byte fewer", &out, &r, 1);
		}

		/* no room at all */
		out.pairs = NULL;
		out.max_pairs = 0;
		out.buf = NULL;
		out.bufsize = 0;
		parse_case(c, gin.data, &out, &r);
		check_partial(c, "no room", &out, &r, nexp);
	}

	guarded_free(&gin);
	guarded_free(&gpairs);
	guarded_free(&gbuf);
}

/* ---------------- greedy dropping ---------------- */

static void
run_capacity(void)
{
	PsscPair	pairs[4];
	char		buf[64];
	PsscPairOut out = {pairs, 4, buf, 0};
	PsscPairResult r;

	/* "bbbb" (5 bytes with key) does not fit 4 bytes, but a later short pair does */
	out.bufsize = 4;
	parse_opts(SC, NULL, IN("a='1',b='bbbb',c='3'"), &out, &r);
	checks++;
	if (r.npairs != 2 || r.ndropped != 1 || r.bufused != 4 ||
		pairs[0].key[0] != 'a' || pairs[1].key[0] != 'c')
	{
		FAIL("greedy: a later pair that fits must still be kept");
		show_pairs(&out, &r);
	}

	/* pairs dropped for the pair cap still count, and malformed ones too */
	out.max_pairs = 1;
	out.bufsize = sizeof(buf);
	parse_opts(MG, NULL, IN("a:1,junk,b:2,c:3"), &out, &r);
	checks++;
	if (r.npairs != 1 || r.ndropped != 2 || r.nmalformed != 1)
	{
		FAIL("cap: expected 1 kept, 2 dropped, 1 malformed");
		show_pairs(&out, &r);
	}
}

/* ---------------- pssc_comment_body ---------------- */

typedef struct BodyCase
{
	const char *span;
	size_t		len;
	bool		ok;
	const char *body;			/* expected body when ok */
	size_t		bodylen;
} BodyCase;

static const BodyCase body_cases[] = {
	{IN("/*a*/"), true, IN("a")},
	{IN("/**/"), true, IN("")},
	{IN("/* key='v' */"), true, IN(" key='v' ")},
	{IN("/* a /* b */ c */"), true, IN(" a /* b */ c ")},
	{IN("/***/"), true, IN("*")},
	{IN("--x"), true, IN("x")},
	{IN("-- a:b"), true, IN(" a:b")},
	{IN("--"), true, IN("")},
	{IN("--*/"), true, IN("*/")},
	{IN("/*/"), false, IN("")},
	{IN("/*"), false, IN("")},
	{IN("/*a"), false, IN("")},
	{IN("/*a*/ "), false, IN("")},
	{IN("-x"), false, IN("")},
	{IN("-"), false, IN("")},
	{IN("x"), false, IN("")},
	{IN(""), false, IN("")},
	{IN("*/"), false, IN("")},
};

static void
run_body_cases(void)
{
	size_t		i;

	for (i = 0; i < sizeof(body_cases) / sizeof(body_cases[0]); i++)
	{
		const BodyCase *b = &body_cases[i];
		Guarded		g = guarded_alloc(b->len);
		char	   *s = guarded_tail(&g, b->len);
		size_t		off = 12345,
					blen = 12345;
		bool		ok;

		memcpy(s, b->span, b->len);
		ok = pssc_comment_body(s, b->len, &off, &blen);
		checks++;
		if (ok != b->ok ||
			(ok && (blen != b->bodylen || off + blen > b->len ||
					memcmp(s + off, b->body, blen) != 0)) ||
			(!ok && (off != 0 || blen != 0)))
			FAIL("comment_body(\"%.*s\"): got %d off=%zu len=%zu", (int) b->len,
				 b->span, ok, off, blen);
		guarded_free(&g);
	}
}

/* ---------------- pathological inputs (linear time, no crash) ---------------- */

static void
run_big_prefixed(const char *what, Fmt fmt, const char *prefix,
				 const char *unit, size_t n_units)
{
	size_t		pl = strlen(prefix);
	size_t		ul = strlen(unit);
	size_t		len = pl + ul * n_units;
	Guarded		g = guarded_alloc(len);
	char	   *s = guarded_tail(&g, len);
	static PsscPair pairs[1024];
	Guarded		gb = guarded_alloc(len);
	PsscPairOut out = {pairs, 1024, NULL, len};
	PsscPairResult r;
	size_t		i;

	out.buf = guarded_tail(&gb, len);
	memcpy(s, prefix, pl);
	for (i = 0; i < n_units; i++)
		memcpy(s + pl + i * ul, unit, ul);
	checks++;
	parse_opts(fmt, NULL, s, len, &out, &r);
	check_storage(what, &out, &r);
	if (r.bufused > len)
		FAIL("%s: decoded output longer than input", what);
	guarded_free(&g);
	guarded_free(&gb);
}

static void
run_big(const char *what, Fmt fmt, const char *unit, size_t n_units)
{
	run_big_prefixed(what, fmt, "", unit, n_units);
}

static void
run_pathological(void)
{
	run_big("quotes", SC, "a=',", 20000);
	/* one unterminated quote, then many pairs whose quotes are all escaped */
	run_big_prefixed("unterminated quote then escaped quotes", SC, "a='", ",a=\\'", 20000);
	run_big_prefixed("unterminated quote then escaped quotes, spaced", SC, "a='", ", a = \\' ", 20000);
	run_big("escaped quotes", SC, "a='\\',", 20000);
	run_big("backslashes", SC, "\\", 100000);
	run_big("percents", SC, "k='%%%,", 20000);
	run_big("many pairs", SC, "a='%41',", 20000);
	run_big("junk after quote", SC, "a='x'y", 20000);
	run_big("mg many pairs", MG, "a:b,", 50000);
	run_big("mg no separators", MG, "abcdef", 50000);
	run_big("mg colons", MG, ":::,", 50000);
	run_big("sc commas", SC, ",", 100000);
	run_big("sc long key", SC, "k", 100000);
	run_big("mg long key", MG, "k", 100000);
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
		size_t		len = case_len(&cases[i]);

		snprintf(path, sizeof(path), "%s/%s-%03zu.txt", dir,
				 cases[i].fmt == MG ? "mg" : "sc", i);
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
	run_capacity();
	run_body_cases();
	run_pathological();

#ifdef PSSC_PAIRS_CHECKED
	printf("max byte reads per body byte (bodies >= 64 bytes): %.2f\n", max_reads_per_byte);
#endif
	if (failures)
	{
		printf("FAILED: %d failure(s) in %d checks\n", failures, checks);
		return 1;
	}
	printf("ok: %d checks passed (%zu parser cases, %zu comment-body cases)\n",
		   checks, (size_t) NCASES, sizeof(body_cases) / sizeof(body_cases[0]));
	return 0;
}
