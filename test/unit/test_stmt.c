/*
 * test_stmt.c
 *		Standalone unit tests for the statement-level API in src/scan.c
 *		(pssc_stmt_range, pssc_scan_statement, pssc_scan_footer; DESIGN.md
 *		§6.2, §6.5). No server needed.
 *
 * Like test_scan.c, each statement case is scanned in several placements
 * (ending at a PROT_NONE guard page, starting right after one, start > 0
 * after one, and wrapped in hostile prefixes/suffixes), and all results must
 * agree, so reading outside [start, end) either faults or is detected.
 * Every reported span is also checked to be a complete comment by an
 * independent validator ("never half a comment"), and a randomized property
 * check compares the exact modes against a reference built from
 * pssc_scan_comments(). The -DPSSC_SCAN_CHECKED build (test_stmt_checked)
 * asserts bounds on every byte read and checks that windowed scans read
 * O(scan_window) bytes, not O(statement).
 *
 *	test_stmt                     run all tests
 *	test_stmt --emit-corpus DIR   write each case input to DIR (fuzz seeds)
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
	OFF = 2,
	BOTH = ON | OFF
};

#define A PSSC_POS_APPEND
#define P PSSC_POS_PREPEND
#define Y PSSC_POS_ANY
#define N NULL
#define DEF 2048				/* scan_window default (§4.1) */
#define MAXEXP (PSSC_SCAN_MAX_COMMENTS + 2)
#define UPTO_SEMI (-99)			/* stmt_len: up to the first ';' after loc */
#define NOOFF ((long) -1)

/* ~38 bytes of IN-list filler; with window 16 a statement using it is long */
#define PAD "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12"

typedef struct StmtCase
{
	const char *name;
	const char *in;
	int			loc;			/* stmt_location */
	int			len;			/* stmt_len, or UPTO_SEMI */
	int			pos;
	size_t		window;
	int			scs;
	int			heuristic;
	int			truncated;
	int			unterminated;
	const char *exp[MAXEXP];	/* expected comment texts, NULL-terminated */
	long		off0;			/* expected offset of exp[0] in in, or NOOFF */
} StmtCase;

static const StmtCase stmt_cases[] = {
	/* ---- AC: "SELECT 1 <a>; SELECT 2 <b>": each gets only its own comment ---- */
	{"AC1 stmt1 append", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, A, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 9},
	{"AC1 stmt1 any", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, Y, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 9},
	{"AC1 stmt1 prepend", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"AC1 stmt2 append", "SELECT 1 /*a*/; SELECT 2 /*b*/", 15, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 25},
	{"AC1 stmt2 any", "SELECT 1 /*a*/; SELECT 2 /*b*/", 15, 0, Y, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 25},
	{"AC1 stmt2 prepend", "SELECT 1 /*a*/; SELECT 2 /*b*/", 15, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	/* stmt_location = -1: the whole string (stmt_len ignored) */
	{"loc -1 any", "SELECT 1 /*a*/; SELECT 2 /*b*/", -1, 0, Y, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 9},
	{"loc -1 ignores len", "SELECT 1 /*a*/; SELECT 2 /*b*/", -1, 5, Y, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 9},
	{"loc -1 append", "SELECT 1 /*a*/; SELECT 2 /*b*/", -1, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 25},
	{"loc -1 prepend", "/*a*/ SELECT 1; SELECT 2 /*b*/", -1, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 0},
	/* stmt_len = 0: to the end of the string */
	{"len 0 rest", "SELECT 1; SELECT 2 /*b*/ ;", 9, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 19},
	{"len 0 rest prepend", "SELECT 1; /*b*/ SELECT 2", 9, 0, P, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 10},

	/* ---- AC: "SELECT 1; SELECT 2; <footer>": the ranges own no comment ---- */
	{"AC2 stmt1 append", "SELECT 1; SELECT 2; /*controller:x*/", 0, 8, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"AC2 stmt1 any", "SELECT 1; SELECT 2; /*controller:x*/", 0, 8, Y, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"AC2 stmt2 append", "SELECT 1; SELECT 2; /*controller:x*/", 9, 9, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"AC2 stmt2 any", "SELECT 1; SELECT 2; /*controller:x*/", 9, 9, Y, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"AC2 stmt2 prepend", "SELECT 1; SELECT 2; /*controller:x*/", 9, 9, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},

	/* ---- append, exact (fits in the window) ---- */
	{"append trailing ; ws", "SELECT 1 /*a*/ ;\n ", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 9},
	{"append ;; tab", "SELECT 1 /*a*/ ;; \t", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 9},
	{"append not last", "SELECT /*a*/ 1", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append run of two", "SELECT 1 /*a*/ /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 9},
	{"append identical twice", "/*a*/ /*a*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*a*/", N}, 0},
	{"append trailing line", "SELECT 1 /*a*/ -- x", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append only line", "SELECT 1 -- x", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append line with block text", "SELECT 1 -- /*a*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append open string after", "SELECT 1 /*a*/ 'open", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append open comment after", "SELECT 1 /*a*/ /* open", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append comment in string", "SELECT '/*a*/'", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append comment in dollar", "SELECT $$ /*a*/ $$", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append nested", "SELECT 1 /* a /* b */ c */", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/* a /* b */ c */", N}, 9},
	{"append empty", "", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"append only comment", "/*a*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 0},
	{"append backslash scs on", "SELECT '\\' /*a*/", 0, 0, A, DEF, ON, 0, 0, 0, {"/*a*/", N}, 11},
	{"append backslash scs off", "SELECT '\\' /*a*/", 0, 0, A, DEF, OFF, 0, 0, 0, {N}, NOOFF},
	/* a run longer than 16 keeps its last 16 comments */
	{"append many comments", "/**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**/ /*z*/",
	0, 0, A, DEF, BOTH, 0, 1, 0, {"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/",
	"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/*z*/", N}, 12},
	{"append window = len", "SELECT 1 /*a*/", 0, 0, A, 14, BOTH, 0, 0, 0, {"/*a*/", N}, 9},

	/* ---- prepend, exact ---- */
	{"prepend leading", "/*a*/ SELECT 1", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 0},
	{"prepend run of two", " \n/*a*/ /*b*/ SELECT", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 2},
	{"prepend line", "-- a\nSELECT 1 /*b*/", 0, 0, P, DEF, BOTH, 0, 0, 0, {"-- a", N}, 0},
	{"prepend line to end", "-- a", 0, 0, P, DEF, BOTH, 0, 0, 0, {"-- a", N}, 0},
	{"prepend not first", "SELECT /*a*/", 0, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"prepend open", "/* open SELECT", 0, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"prepend after ;", ";/*a*/ SELECT", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 1},
	{"prepend string first", "'x' /*a*/", 0, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"prepend empty", "", 0, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"prepend 2nd statement", "SELECT 1; /*a*/ SELECT 2", 9, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 10},

	/* ---- any, exact ---- */
	{"any all", "/*a*/ SELECT /*b*/ 1 --c", 0, 0, Y, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", "--c", N}, 0},
	{"any unterminated", "SELECT 1 /*a*/ 'x", 0, 0, Y, DEF, BOTH, 0, 0, 1, {"/*a*/", N}, 9},

	/* ---- AC: long statements (window 16), append = heuristic tail path ---- */
	{"long append", "SELECT " PAD " /*c:x*/ ;  ", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	{"long append fills window", "SELECT " PAD " /*controller:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*controller:x*/", N}, NOOFF},
	/* AC: a comment that crosses the window start yields nothing */
	{"long append crosses by 1", "SELECT " PAD " /*controller:xy*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append crosses (;)", "SELECT " PAD " /*controller:x*/;", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append crosses far", "SELECT /* " PAD " */", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append nested inner only", "SELECT " PAD " /*b*/ c */", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append nested", "SELECT " PAD " /* a /*b*/ c */", 0, 0, A, 16, BOTH, 1, 0, 0, {"/* a /*b*/ c */", N}, NOOFF},
	{"long append trailing line", "SELECT " PAD " /*c:x*/ -- n", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append inside line comment", "SELECT " PAD " -- /*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append -- glued", "SELECT " PAD "--/*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append single -", "SELECT " PAD " 1-/*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	{"long append -- on earlier line", "SELECT 1 -- n\n" PAD " /*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	{"long append -- on earlier line CR", "SELECT 1 -- n\r" PAD "\r/*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	{"long append -- just before newline", "SELECT " PAD " --\n/*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	/* known inexactness: the "--" lies before the window */
	{"long append misattributed line", "SELECT 1 -- " PAD " /*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	{"long append ws only", "SELECT /*c:x*/ " PAD "                    ", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append no closer", "SELECT " PAD " /*c:x*/ 1", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append bare closer", "SELECT " PAD " */", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append empty comment", "SELECT " PAD " /**/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/**/", N}, NOOFF},
	{"long append stars", "SELECT " PAD " /***/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/***/", N}, NOOFF},
	/* star-slash-star: the star-slash may close an earlier comment */
	{"long append after *", "SELECT " PAD "*2*/*c*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append after closer", "SELECT /*" PAD "*/*c*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append * before window", "SELECT " PAD "*/*controller:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append after * spaced", "SELECT " PAD "* /*c*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c*/", N}, NOOFF},
	{"long append slash-star-slash", "SELECT " PAD " /*/ x */", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append run glued", "SELECT " PAD "/*a*//*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*a*/", "/*b*/", N}, NOOFF},
	{"long append run spaced", "SELECT " PAD " /*a*/ /*b*/ ;", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*a*/", "/*b*/", N}, NOOFF},
	{"long append window starts in comment", "SELECT /* " PAD " */ /*c:x*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", N}, NOOFF},
	/* AC: string literals ending in star-slash on the tail path */
	{"long append open literal ends in */", "SELECT '" PAD " /*c:evil*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:evil*/", N}, NOOFF},
	{"exact any same literal", "SELECT '" PAD " /*c:evil*/", 0, 0, Y, DEF, BOTH, 0, 0, 1, {N}, NOOFF},
	{"exact append same literal", "SELECT '" PAD " /*c:evil*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long append literal then real", "SELECT '" PAD "/*c:evil*/' /*c:real*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:real*/", N}, NOOFF},
	{"long append literal last", "SELECT 1 WHERE s = '" PAD " /*c:evil*/'", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append window 0", "SELECT 1 /*a*/", 0, 0, A, 0, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append range in multi", "SELECT " PAD " /*a:1*/; SELECT 2 /*b:2*/", 0, UPTO_SEMI, A, 16, BOTH, 1, 0, 0, {"/*a:1*/", N}, NOOFF},
	{"long append 2nd range in multi", "SELECT 1 /*a:1*/; SELECT " PAD " /*b:2*/", 17, 0, A, 16, BOTH, 1, 0, 0, {"/*b:2*/", N}, NOOFF},

	/* ---- AC: long statements, prepend = first window, exact ---- */
	{"long prepend", "/*c:x*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"/*c:x*/", N}, 0},
	{"long prepend fills window", "/*controller:x*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"/*controller:x*/", N}, 0},
	{"long prepend crosses (ws)", " /*controller:x*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend crosses", "/*controller:xy*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend line", "-- c:x\nSELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"-- c:x", N}, 0},
	{"long prepend line cut", "-- controller:xyz\nSELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend line newline after window", "-- controller:xy\nSELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"-- controller:xy", N}, 0},
	{"long prepend line CR after window", "-- controller:xy\rSELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"-- controller:xy", N}, 0},
	{"long prepend comment in tail", "SELECT " PAD " /*c:x*/", 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend run of two", " \n\t/*a*/ /*b*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 3},
	{"long prepend open", "/* open SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend string", "'/*a*/' SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend nested 17", "/* a /* b */ c */ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend nested 17 w17", "/* a /* b */ c */ SELECT " PAD, 0, 0, P, 17, BOTH, 0, 0, 0, {"/* a /* b */ c */", N}, 0},
	{"long prepend window 0", "/*a*/ SELECT 1", 0, 0, P, 0, BOTH, 0, 0, 0, {N}, NOOFF},
	{"long prepend 2nd range in multi", "SELECT 1; /*b:2*/ SELECT " PAD, 9, 0, P, 16, BOTH, 0, 0, 0, {"/*b:2*/", N}, 10},

	/* ---- AC: long statements, any = full forward scan, exact ---- */
	{"long any both", "/*p:x*/ SELECT " PAD " /*a:y*/", 0, 0, Y, 16, BOTH, 0, 0, 0, {"/*p:x*/", "/*a:y*/", N}, 0},
	{"long any budget", "/*abcdefghij*/ SELECT " PAD " /*b*/", 0, 0, Y, 16, BOTH, 0, 1, 0, {"/*abcdefghij*/", N}, 0},
	{"long any misattribution case", "SELECT 1 -- " PAD " /*c:x*/", 0, 0, Y, 16, BOTH, 0, 1, 0, {N}, NOOFF},
	{"long any middle comment", "SELECT " PAD " /*m*/ " PAD, 0, 0, Y, 16, BOTH, 0, 0, 0, {"/*m*/", N}, NOOFF},
	{"long any window 0", "SELECT 1 /*a*/", 0, 0, Y, 0, BOTH, 0, 1, 0, {N}, NOOFF},
	{"long prepend vs append vs any (prepend)", "/*p:x*/ SELECT " PAD " /*a:y*/", 0, 0, P, 16, BOTH, 0, 0, 0, {"/*p:x*/", N}, 0},
	{"long prepend vs append vs any (append)", "/*p:x*/ SELECT " PAD " /*a:y*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*a:y*/", N}, NOOFF},

	/* ---- runs: marginalia with_annotation appends a second comment ---- */
#define ANN "SELECT 1 /*application:Foo,controller:users,action:show*/ /*custom annotation*/"
	{"annotation append", ANN, 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*application:Foo,controller:users,action:show*/", "/*custom annotation*/", N}, 9},
	{"annotation any", ANN, 0, 0, Y, DEF, BOTH, 0, 0, 0, {"/*application:Foo,controller:users,action:show*/", "/*custom annotation*/", N}, 9},
	{"annotation prepend", ANN, 0, 0, P, DEF, BOTH, 0, 0, 0, {N}, NOOFF},
	{"annotation prepend run", "/*application:Foo*/ /*custom annotation*/ SELECT 1", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*application:Foo*/", "/*custom annotation*/", N}, 0},
	{"annotation long append", "SELECT " PAD " /*c:x*/ /*ann*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c:x*/", "/*ann*/", N}, NOOFF},
	{"annotation long append exact fit", "SELECT " PAD " /*c:x*/ /*ann*/", 0, 0, A, 15, BOTH, 1, 0, 0, {"/*c:x*/", "/*ann*/", N}, NOOFF},
	{"annotation long append run cut by window", "SELECT " PAD " /*c:x*/ /*ann*/", 0, 0, A, 14, BOTH, 1, 0, 0, {"/*ann*/", N}, NOOFF},
	{"annotation long prepend", "/*c:x*/ /*ann*/ SELECT " PAD, 0, 0, P, 16, BOTH, 0, 0, 0, {"/*c:x*/", "/*ann*/", N}, 0},
	{"annotation long prepend run cut by window", "/*c:x*/ /*ann*/ SELECT " PAD, 0, 0, P, 14, BOTH, 0, 0, 0, {"/*c:x*/", N}, 0},
	{"append run newline sep", "SELECT 1 /*a*/\n\t/*b*/ ;", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a*/", "/*b*/", N}, 9},
	{"append run nested", "SELECT 1 /*a /*x*/ */ /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*a /*x*/ */", "/*b*/", N}, 9},
	{"append run stops at token", "SELECT 1 /*a*/ x /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 17},
	{"append run stops at ;", "SELECT 1 /*a*/; /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 16},
	{"append run stops at line", "SELECT 1 /*a*/ -- x\n /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 21},
	{"append run stops at string", "SELECT 1 /*a*/ '' /*b*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*b*/", N}, 18},
	{"prepend run stops at token", "/*a*/ x /*b*/", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 0},
	{"prepend run stops at ;", "/*a*/; /*b*/ x", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", N}, 0},
	{"prepend run with lines", "/*a*/\n-- b\n/*c*/ SELECT 1 /*d*/", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", "-- b", "/*c*/", N}, 0},
	{"prepend run line last", "/*a*/ -- b", 0, 0, P, DEF, BOTH, 0, 0, 0, {"/*a*/", "-- b", N}, 0},
	{"prepend run 17", "/**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//*z*/ x", 0, 0, P, DEF, BOTH, 0, 1, 0,
	{"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}, 0},
	{"prepend run 16", "/**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//*z*/ x", 0, 0, P, DEF, BOTH, 0, 0, 0,
	{"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/*z*/", N}, 0},
	{"append run 16", "x /*z*//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**/", 0, 0, A, DEF, BOTH, 0, 0, 0,
	{"/*z*/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}, 2},
	{"long append run 17", "SELECT " PAD " /*z*//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**/", 0, 0, A, 70, BOTH, 1, 1, 0,
	{"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}, NOOFF},
	{"long append run 16", "SELECT " PAD PAD " /*z*//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**/", 0, 0, A, 70, BOTH, 1, 0, 0,
	{"/*z*/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}, NOOFF},
	{"long append run stops at token", "SELECT " PAD " /*a*/ x /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
	{"long append run stops at ;", "SELECT " PAD " /*a*/; /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
	{"long append run stops at line", "SELECT " PAD "/*a*/--\n/*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
	{"long append run -- inside earlier", "SELECT " PAD "/* -- */ /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/* -- */", "/*b*/", N}, NOOFF},
	{"long append run in line comment", "SELECT " PAD " -- /*a*/ /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {N}, NOOFF},
	{"long append run newline rescues", "SELECT " PAD " -- /*a*/\n/*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
	{"long append run newline inside earlier", "SELECT " PAD "-- /*a\nb*/ /*c*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*c*/", N}, NOOFF},
	{"exact append run newline inside earlier", "SELECT " PAD "-- /*a\nb*/ /*c*/", 0, 0, A, DEF, BOTH, 0, 0, 0, {"/*c*/", N}, NOOFF},
	{"long append run earlier after *", "SELECT " PAD "*/*a*/ /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
	{"long append run earlier crosses", "SELECT /* " PAD " */ /*b*/", 0, 0, A, 16, BOTH, 1, 0, 0, {"/*b*/", N}, NOOFF},
};

typedef struct FooterCase
{
	const char *name;
	const char *in;
	size_t		inlen;			/* 0: strlen(in); set for embedded NULs */
	size_t		stmt_end;
	int			pos;
	size_t		window;
	int			truncated;
	const char *exp[MAXEXP];
} FooterCase;

#define F17 "/**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//**//*z*/"

static const FooterCase footer_cases[] = {
	/* AC: only the last statement gets the footer */
	{"AC2 stmt1 append", "SELECT 1; SELECT 2; /*controller:x*/", 0, 8, A, DEF, 0, {N}},
	{"AC2 stmt1 any", "SELECT 1; SELECT 2; /*controller:x*/", 0, 8, Y, DEF, 0, {N}},
	{"AC2 stmt1 prepend", "SELECT 1; SELECT 2; /*controller:x*/", 0, 8, P, DEF, 0, {N}},
	{"AC2 stmt2 append", "SELECT 1; SELECT 2; /*controller:x*/", 0, 18, A, DEF, 0, {"/*controller:x*/", N}},
	{"AC2 stmt2 any", "SELECT 1; SELECT 2; /*controller:x*/", 0, 18, Y, DEF, 0, {"/*controller:x*/", N}},
	{"AC2 stmt2 prepend", "SELECT 1; SELECT 2; /*controller:x*/", 0, 18, P, DEF, 0, {"/*controller:x*/", N}},
	/* AC1: a comment owned by a later statement is never a footer */
	{"AC1 stmt1", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, Y, DEF, 0, {N}},
	{"AC1 stmt2 at end", "SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 30, Y, DEF, 0, {N}},
	{"two append", "SELECT 1; /*a*/ /*b*/ ;\n", 0, 8, A, DEF, 0, {"/*a*/", "/*b*/", N}},
	{"two any", "SELECT 1; /*a*/ /*b*/ ;\n", 0, 8, Y, DEF, 0, {"/*a*/", "/*b*/", N}},
	{"two prepend", "SELECT 1; /*a*/ /*b*/ ;\n", 0, 8, P, DEF, 0, {"/*a*/", "/*b*/", N}},
	{"line last append", "SELECT 1; /*a*/ -- n", 0, 8, A, DEF, 0, {N}},
	{"line last any", "SELECT 1; /*a*/ -- n", 0, 8, Y, DEF, 0, {"/*a*/", "-- n", N}},
	{"line last prepend", "SELECT 1; /*a*/ -- n", 0, 8, P, DEF, 0, {"/*a*/", "-- n", N}},
	{"line only prepend", "SELECT 1; -- n\n", 0, 8, P, DEF, 0, {"-- n", N}},
	{"line only append", "SELECT 1; -- n\n", 0, 8, A, DEF, 0, {N}},
	{"nested append", "SELECT 1; /*a /*b*/ */", 0, 8, A, DEF, 0, {"/*a /*b*/ */", N}},
	{"not trivia", "SELECT 1; /*a*/ x", 0, 8, Y, DEF, 0, {N}},
	{"not trivia append", "SELECT 1; /*a*/ x", 0, 8, A, DEF, 0, {N}},
	{"open comment", "SELECT 1; /* open", 0, 8, Y, DEF, 0, {N}},
	{"open comment after one", "SELECT 1; /*a*/ /* open", 0, 8, A, DEF, 0, {N}},
	{"string", "SELECT 1; '/*a*/'", 0, 8, Y, DEF, 0, {N}},
	{"no footer", "SELECT 1;", 0, 8, Y, DEF, 0, {N}},
	{"at end", "SELECT 1;", 0, 9, Y, DEF, 0, {N}},
	{"NUL ends string", "SELECT 1; /*a*/\0 x", 18, 8, Y, DEF, 0, {"/*a*/", N}},
	{"NUL before comment", "SELECT 1; \0/*x*/", 16, 8, Y, DEF, 0, {N}},
	{"window = rest", "SELECT 1; /*controller:x*/", 0, 8, A, 18, 0, {"/*controller:x*/", N}},
	{"window < rest", "SELECT 1; /*controller:x*/", 0, 8, A, 17, 0, {N}},
	{"window unbounded", "SELECT 1; /*controller:x*/", 0, 8, A, SIZE_MAX, 0, {"/*controller:x*/", N}},
	{"window huge", "SELECT 1; /*controller:x*/", 0, 8, Y, (size_t) PTRDIFF_MAX + 1, 0, {"/*controller:x*/", N}},
	{"window < rest any", "SELECT 1; /*controller:x*/", 0, 8, Y, 17, 0, {N}},
	{"17 any", "SELECT 1;" F17, 0, 9, Y, DEF, 1, {"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/",
	"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}},
	{"17 append", "SELECT 1;" F17, 0, 9, A, DEF, 1, {"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/",
	"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/*z*/", N}},
	{"17 prepend", "SELECT 1;" F17, 0, 9, P, DEF, 1, {"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/",
	"/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", "/**/", N}},
	/* runs: the footer's comments may also be separated by ';' */
	{"annotation footer append", "SELECT 1; /*application:Foo,controller:users*/ /*custom annotation*/", 0, 8, A, DEF, 0,
	{"/*application:Foo,controller:users*/", "/*custom annotation*/", N}},
	{"annotation footer prepend", "SELECT 1; /*application:Foo,controller:users*/ /*custom annotation*/", 0, 8, P, DEF, 0,
	{"/*application:Foo,controller:users*/", "/*custom annotation*/", N}},
	{"run across ; append", "SELECT 1; /*a*/ ; /*b*/;", 0, 8, A, DEF, 0, {"/*a*/", "/*b*/", N}},
	{"run across ; prepend", "SELECT 1; /*a*/ ; /*b*/;", 0, 8, P, DEF, 0, {"/*a*/", "/*b*/", N}},
	{"run broken by line append", "SELECT 1; /*a*/ -- x\n/*b*/", 0, 8, A, DEF, 0, {"/*b*/", N}},
	{"run with line prepend", "SELECT 1; /*a*/ -- x\n/*b*/", 0, 8, P, DEF, 0, {"/*a*/", "-- x", "/*b*/", N}},
};

typedef struct RangeCase
{
	const char *in;
	int			loc;
	int			len;
	size_t		want_start;
	size_t		want_end;
} RangeCase;

static const RangeCase range_cases[] = {
	{"abcdef", -1, 0, 0, 6},
	{"abcdef", -1, 3, 0, 6},
	{"abcdef", -1, -1, 0, 6},
	{"abcdef", 2, 0, 2, 6},
	{"abcdef", 2, -1, 2, 6},
	{"abcdef", 2, 3, 2, 5},
	{"abcdef", 0, 6, 0, 6},
	{"abcdef", 6, 0, 6, 6},
	{"", -1, 0, 0, 0},
	{"", 0, 0, 0, 0},
	{"SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, 0, 14},
	{"SELECT 1 /*a*/; SELECT 2 /*b*/", 15, 0, 15, 30},
};

/*
 * Statement locations reported by real servers (post_parse_analyze_hook,
 * psql -c, simple query protocol unless noted): PG14-17 report identical
 * values, PG18 starts a statement at its first token, excluding leading
 * whitespace and comments (stmt_len still ends at the ';' or string end).
 * Each row is one statement; rows for the same string are consecutive. The
 * owned range must be the PG14-17 range under both conventions, and the
 * expected comments are those of that owned range (any/append/prepend) and of
 * the footer after it (append).
 */
typedef struct ServerStmt
{
	const char *q;
	int			loc14,
				len14,
				loc18,
				len18;
	const char *any[MAXEXP];
	const char *app[MAXEXP];
	const char *pre[MAXEXP];
	const char *foot[MAXEXP];
} ServerStmt;

#define MULTI1 "/*a*/ /*b*/ SELECT 1; /*c*/ /*d*/ SELECT 2 /*e*/ /*f*/; /*g*/"
#define LINES "/*a*/\n-- b\n/*c*/ SELECT 1 /*d*/ -- e\n/*f*/;"

static const ServerStmt server_stmts[] = {
	{"SELECT 1", 0, 0, 0, 0, {N}, {N}, {N}, {N}},
	{"SELECT 1;", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"/*c:foo*/ SELECT 1;", 0, 18, 10, 8, {"/*c:foo*/", N}, {N}, {"/*c:foo*/", N}, {N}},
	{"/*c:foo*/ SELECT 1", 0, 0, 10, 0, {"/*c:foo*/", N}, {N}, {"/*c:foo*/", N}, {N}},
	{"/*controller:foo*/ SELECT 1;", 0, 27, 19, 8, {"/*controller:foo*/", N}, {N}, {"/*controller:foo*/", N}, {N}},
	{"SELECT 1 /*c:foo*/;", 0, 18, 0, 18, {"/*c:foo*/", N}, {"/*c:foo*/", N}, {N}, {N}},
	{"SELECT 1 /*c:foo*/", 0, 0, 0, 0, {"/*c:foo*/", N}, {"/*c:foo*/", N}, {N}, {N}},
	{"  SELECT 1  ;  ", 0, 12, 2, 10, {N}, {N}, {N}, {N}},
	{"SELECT 1; /*c:bar*/ SELECT 2;", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1; /*c:bar*/ SELECT 2;", 9, 19, 20, 8, {"/*c:bar*/", N}, {N}, {"/*c:bar*/", N}, {N}},
	{"SELECT 1 /*a*/; SELECT 2 /*b*/", 0, 14, 0, 14, {"/*a*/", N}, {"/*a*/", N}, {N}, {N}},
	{"SELECT 1 /*a*/; SELECT 2 /*b*/", 15, 0, 16, 0, {"/*b*/", N}, {"/*b*/", N}, {N}, {N}},
	{"SELECT 1; SELECT 2; /*controller:x*/", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1; SELECT 2; /*controller:x*/", 9, 9, 10, 8, {N}, {N}, {N}, {"/*controller:x*/", N}},
	{"-- lead\nSELECT 1;", 0, 16, 8, 8, {"-- lead", N}, {N}, {"-- lead", N}, {N}},
	{"SELECT 1 -- trail\n;", 0, 18, 0, 18, {"-- trail", N}, {N}, {N}, {N}},
	{"SELECT 1;; /*x*/ SELECT 2", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1;; /*x*/ SELECT 2", 10, 0, 17, 0, {"/*x*/", N}, {N}, {"/*x*/", N}, {N}},
	{"SELECT 1 /*a*/ /*b*/ ; SELECT 2 /*c*/ ;", 0, 21, 0, 21, {"/*a*/", "/*b*/", N}, {"/*a*/", "/*b*/", N}, {N}, {N}},
	{"SELECT 1 /*a*/ /*b*/ ; SELECT 2 /*c*/ ;", 22, 16, 23, 15, {"/*c*/", N}, {"/*c*/", N}, {N}, {N}},
	{"/*u*/ SET search_path = public /*v*/;", 0, 36, 6, 30, {"/*u*/", "/*v*/", N}, {"/*v*/", N}, {"/*u*/", N}, {N}},
	{"/*x*/ SELECT 1 /*y*/; /*z*/", 0, 20, 6, 14, {"/*x*/", "/*y*/", N}, {"/*y*/", N}, {"/*x*/", N}, {"/*z*/", N}},
	{"/*p*/ (SELECT 1) /*a*/;", 0, 22, 6, 16, {"/*p*/", "/*a*/", N}, {"/*a*/", N}, {"/*p*/", N}, {N}},
	{"/*w*/ WITH x AS (SELECT 1) SELECT * FROM x /*a*/;", 0, 48, 6, 42, {"/*w*/", "/*a*/", N}, {"/*a*/", N}, {"/*w*/", N}, {N}},
	{ANN, 0, 0, 0, 0, {"/*application:Foo,controller:users,action:show*/", "/*custom annotation*/", N},
	{"/*application:Foo,controller:users,action:show*/", "/*custom annotation*/", N}, {N}, {N}},
	{MULTI1, 0, 20, 12, 8, {"/*a*/", "/*b*/", N}, {N}, {"/*a*/", "/*b*/", N}, {N}},
	{MULTI1, 21, 33, 34, 20, {"/*c*/", "/*d*/", "/*e*/", "/*f*/", N}, {"/*e*/", "/*f*/", N}, {"/*c*/", "/*d*/", N}, {"/*g*/", N}},
	{"SELECT 1 /*a; b*/; /*c; d*/ SELECT 2", 0, 17, 0, 17, {"/*a; b*/", N}, {"/*a; b*/", N}, {N}, {N}},
	{"SELECT 1 /*a; b*/; /*c; d*/ SELECT 2", 18, 0, 28, 0, {"/*c; d*/", N}, {N}, {"/*c; d*/", N}, {N}},
	{"/*e*/ EXPLAIN SELECT 1 /*f*/", 0, 0, 6, 0, {"/*e*/", "/*f*/", N}, {"/*f*/", N}, {"/*e*/", N}, {N}},
	{"/*d*/ DO $$BEGIN PERFORM /*n*/ 1; END$$ /*t*/;", 0, 45, 6, 39, {"/*d*/", "/*t*/", N}, {"/*t*/", N}, {"/*d*/", N}, {N}},
	/* extended query protocol (psql \bind) */
	{"/*ext*/ SELECT $1 /*tail*/ ", 0, 0, 8, 0, {"/*ext*/", "/*tail*/", N}, {"/*tail*/", N}, {"/*ext*/", N}, {N}},
	/* ';' inside strings, identifiers, dollar quotes and comments */
	{"SELECT ';' ; /*c*/ SELECT 2", 0, 11, 0, 11, {N}, {N}, {N}, {N}},
	{"SELECT ';' ; /*c*/ SELECT 2", 12, 0, 19, 0, {"/*c*/", N}, {N}, {"/*c*/", N}, {N}},
	{"SELECT 1; -- x;y\nSELECT 2", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1; -- x;y\nSELECT 2", 9, 0, 17, 0, {"-- x;y", N}, {N}, {"-- x;y", N}, {N}},
	{"SELECT 1; /* a /* ; */ ; */ SELECT 2", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1; /* a /* ; */ ; */ SELECT 2", 9, 0, 28, 0, {"/* a /* ; */ ; */", N}, {N}, {"/* a /* ; */ ; */", N}, {N}},
	{"SELECT 1; /* x; -- */\nSELECT 2", 0, 8, 0, 8, {N}, {N}, {N}, {N}},
	{"SELECT 1; /* x; -- */\nSELECT 2", 9, 0, 22, 0, {"/* x; -- */", N}, {N}, {"/* x; -- */", N}, {N}},
	{"SELECT $q$;$q$ ; /*c*/ SELECT 2", 0, 15, 0, 15, {N}, {N}, {N}, {N}},
	{"SELECT $q$;$q$ ; /*c*/ SELECT 2", 16, 0, 23, 0, {"/*c*/", N}, {N}, {"/*c*/", N}, {N}},
	{"SELECT E'\\';' ; /*c*/ SELECT 2", 0, 14, 0, 14, {N}, {N}, {N}, {N}},
	{"SELECT E'\\';' ; /*c*/ SELECT 2", 15, 0, 22, 0, {"/*c*/", N}, {N}, {"/*c*/", N}, {N}},
	{"SELECT \"a;b\" FROM (SELECT 1 AS \"a;b\") t; /*c*/ SELECT 2", 0, 39, 0, 39, {N}, {N}, {N}, {N}},
	{"SELECT \"a;b\" FROM (SELECT 1 AS \"a;b\") t; /*c*/ SELECT 2", 40, 0, 47, 0, {"/*c*/", N}, {N}, {"/*c*/", N}, {N}},
	{LINES, 0, 42, 17, 25, {"/*a*/", "-- b", "/*c*/", "/*d*/", "-- e", "/*f*/", N}, {"/*f*/", N},
	{"/*a*/", "-- b", "/*c*/", N}, {N}},
};

typedef struct OwnedCase
{
	const char *name;
	const char *in;
	const char *from_at;		/* from = offset of this text, NULL: 0 */
	const char *stmt_at;		/* stmt_start = offset of this text (last match) */
	size_t		max_bytes;
	int			scs;
	const char *want_at;		/* want = offset just past this text, NULL: stmt_start */
} OwnedCase;

static const OwnedCase owned_cases[] = {
	{"max_bytes = gap", "SELECT 1; /*c*/ SELECT 2", N, "SELECT 2", 16, BOTH, "SELECT 1;"},
	{"max_bytes < gap", "SELECT 1; /*c*/ SELECT 2", N, "SELECT 2", 15, BOTH, N},
	{"max_bytes 0, start 0", "SELECT 1", N, "SELECT 1", 0, BOTH, N},
	{"from after the gap", "SELECT 1; /*c*/ SELECT 2", "SELECT 2", "SELECT 2", 0, BOTH, N},
	{"from inside the gap", "SELECT 1; /*c*/ SELECT 2", "/*c*/", "SELECT 2", DEF, BOTH, N},
	{"from at ;", "SELECT 1; /*c*/ SELECT 2", "; /*c*/", "SELECT 2", DEF, BOTH, "SELECT 1;"},
	{"no ; before", "/*a*/ -- b\n /*c*/ SELECT 1", N, "SELECT 1", DEF, BOTH, ""},
	{"token after ; (contract violated)", "SELECT 1; x SELECT 2", N, "SELECT 2", DEF, BOTH, N},
	{"token, no ; (contract violated)", "x /*c*/ SELECT 2", N, "SELECT 2", DEF, BOTH, N},
	{"unterminated before", "SELECT 'x; /*c*/ SELECT 2", N, "SELECT 2", DEF, BOTH, N},
	{"scs on: backslash ends the string", "SELECT 'a\\';' ; /*c*/ SELECT 2", N, "SELECT 2", DEF, ON, N},
	{"scs off: backslash escapes", "SELECT 'a\\';' ; /*c*/ SELECT 2", N, "SELECT 2", DEF, OFF, "' ;"},
	{"empty statements", "SELECT 1;;; ; /*c*/ SELECT 2", N, "SELECT 2", DEF, BOTH, "; ;"},
	{"line comment to stmt", "SELECT 1; -- c\nSELECT 2", N, "SELECT 2", DEF, BOTH, "SELECT 1;"},
};

#define NSERVER (sizeof(server_stmts) / sizeof(server_stmts[0]))
#define NOWNED (sizeof(owned_cases) / sizeof(owned_cases[0]))

#define NSTMT (sizeof(stmt_cases) / sizeof(stmt_cases[0]))
#define NFOOTER (sizeof(footer_cases) / sizeof(footer_cases[0]))
#define NRANGE (sizeof(range_cases) / sizeof(range_cases[0]))

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

/* ---------------- independent span validation ---------------- */

static int
is_nl(char c)
{
	return c == '\n' || c == '\r';
}

/*
 * True if s[off, off + len) is one complete comment by scan.l's rules, and
 * (for a line comment) is followed by a newline or by the end of the range.
 * Written independently of scan.c.
 */
static int
complete_comment(const char *s, size_t start, size_t end, size_t off, size_t len)
{
	size_t		i,
				stop = off + len;

	if (off < start || len < 2 || off > end || len > end - off)
		return 0;
	if (s[off] == '-' && s[off + 1] == '-')
	{
		for (i = off + 2; i < stop; i++)
			if (is_nl(s[i]))
				return 0;
		return stop == end || is_nl(s[stop]);
	}
	if (s[off] == '/' && s[off + 1] == '*')
	{
		int			depth = 1;

		for (i = off + 2; i < stop;)
		{
			if (i + 1 < stop && s[i] == '*' && s[i + 1] == '/')
			{
				i += 2;
				if (--depth == 0)
					return i == stop;
			}
			else if (i + 1 < stop && s[i] == '/' && s[i + 1] == '*')
			{
				depth++;
				i += 2;
			}
			else
				i++;
		}
		return 0;
	}
	return 0;
}

/* ---------------- placements ---------------- */

static int
same_result(const PsscScanResult *a, const PsscScanResult *b, size_t shift)
{
	int			i;

	if (a->ncomments != b->ncomments || a->truncated != b->truncated ||
		a->unterminated != b->unterminated || a->heuristic != b->heuristic)
		return 0;
	for (i = 0; i < a->ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
		if (a->comments[i].offset + shift != b->comments[i].offset ||
			a->comments[i].len != b->comments[i].len)
			return 0;
	return 1;
}

static void
print_result(const char *s, const PsscScanResult *r)
{
	int			i;

	fprintf(stderr, "    got %d comment(s), truncated=%d unterminated=%d heuristic=%d\n",
			r->ncomments, (int) r->truncated, (int) r->unterminated, (int) r->heuristic);
	for (i = 0; i < r->ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
		fprintf(stderr, "      [%zu,+%zu) \"%.*s\"\n", r->comments[i].offset,
				r->comments[i].len, (int) r->comments[i].len,
				s + r->comments[i].offset);
}

static void
stmt_at(const char *s, size_t start, size_t end, int pos, size_t window,
		int scs_on, PsscScanResult *r, const char *what)
{
	memset(r, 0x7f, sizeof(*r));
#ifdef PSSC_SCAN_CHECKED
	pssc_scan_reads = 0;
#endif
	pssc_scan_statement(s, start, end, (PsscPosition) pos, window, scs_on != 0, r);
#ifdef PSSC_SCAN_CHECKED
	{
		size_t		len = end - start;
		int			windowed = len > window && pos != Y;
		/* the exact append path lexes the range, then checks its tail */
		unsigned long limit = windowed ? 4 * (unsigned long) window + 8
			: 4 * (unsigned long) len + 8;

		if (pssc_scan_reads > limit)
			FAIL("%s: %lu byte reads for a %zu-byte range, window %zu (limit %lu)",
				 what, pssc_scan_reads, len, window, limit);
	}
#else
	(void) what;
#endif
}

static const char *const prefixes[] = {"'", "/*", "$$", "E'\\", "\"", "x", "-", "U&", "--", "/"};
static const char *const suffixes[] = {"*/", "'", "\n'", "$$", "\"", "/", "-", "*", "*/", "*"};

/*
 * Scan the statement s[start, end) of the full input placed at the end of a
 * guarded buffer (the reference), then the range bytes alone ending at the
 * high guard page, starting at the low one, with start = 7 right after it,
 * and between hostile prefixes/suffixes. All results must match the
 * reference.
 */
/* Map offsets from a placement where the range starts at base back to in. */
static void
rebase(PsscScanResult *r, size_t base, size_t start)
{
	int			i;

	for (i = 0; i < r->ncomments && i < PSSC_SCAN_MAX_COMMENTS; i++)
		r->comments[i].offset = r->comments[i].offset - base + start;
}

/*
 * Scan the statement s[start, end) of the full input placed at the end of a
 * guarded buffer (the reference), then the range bytes alone ending at the
 * high guard page, starting at the low one, with start = 7 right after it,
 * and between hostile prefixes/suffixes. All results must match the
 * reference.
 */
static void
stmt_everywhere(const char *name, const char *in, size_t inlen, size_t start,
				size_t end, int pos, size_t window, int scs_on,
				PsscScanResult *ref)
{
	size_t		len = end - start;
	Guarded		g = guarded_alloc(inlen + 16);
	PsscScanResult r;
	char	   *s;
	size_t		p;

	s = g.data + g.usable - inlen;
	memcpy(s, in, inlen);
	stmt_at(s, start, end, pos, window, scs_on, ref, name);
	guarded_free(&g);

	g = guarded_alloc(len + 16);
	s = g.data + g.usable - len;
	memcpy(s, in + start, len);
	stmt_at(s, 0, len, pos, window, scs_on, &r, name);
	rebase(&r, 0, start);
	if (!same_result(ref, &r, 0))
		FAIL("%s: result differs when the range ends at the high guard page", name);

	memcpy(g.data, in + start, len);
	stmt_at(g.data, 0, len, pos, window, scs_on, &r, name);
	rebase(&r, 0, start);
	if (!same_result(ref, &r, 0))
		FAIL("%s: result differs when the range starts after the low guard page", name);

	s = g.data - 7;
	stmt_at(s, 7, 7 + len, pos, window, scs_on, &r, name);
	rebase(&r, 7, start);
	if (!same_result(ref, &r, 0))
		FAIL("%s: result differs with start = 7 after the low guard page", name);
	guarded_free(&g);

	for (p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++)
	{
		size_t		pl = strlen(prefixes[p]);
		size_t		sl = strlen(suffixes[p]);
		Guarded		h = guarded_alloc(pl + len + sl);

		s = h.data + h.usable - (pl + len + sl);
		memcpy(s, prefixes[p], pl);
		memcpy(s + pl, in + start, len);
		memcpy(s + pl + len, suffixes[p], sl);
		stmt_at(s, pl, pl + len, pos, window, scs_on, &r, name);
		rebase(&r, pl, start);
		if (!same_result(ref, &r, 0))
			FAIL("%s: result depends on bytes outside the range (prefix \"%s\", suffix \"%s\")",
				 name, prefixes[p], suffixes[p]);
		guarded_free(&h);
	}
}

static int
only_ws_semi(const char *s, size_t a, size_t b)
{
	for (; a < b; a++)
		if (!(s[a] == ';' || s[a] == ' ' || s[a] == '\t' || s[a] == '\n' ||
			  s[a] == '\r' || s[a] == '\f' || s[a] == '\v'))
			return 0;
	return 1;
}

static int
only_ws(const char *s, size_t a, size_t b)
{
	for (; a < b; a++)
		if (!(s[a] == ' ' || s[a] == '\t' || s[a] == '\n' ||
			  s[a] == '\r' || s[a] == '\f' || s[a] == '\v'))
			return 0;
	return 1;
}

/*
 * Every span is a complete comment inside the range (and window, if any),
 * spans are in order, and a positional result is one run: comments separated
 * only by whitespace, after only ';' and whitespace (prepend) or before only
 * ';' and whitespace (append, block comments only).
 */
static void
check_spans(const char *name, const char *s, size_t start, size_t end,
			int pos, size_t window, const PsscScanResult *r)
{
	int			i;
	size_t		lo = start,
				hi = end;

	if (r->ncomments < 0 || r->ncomments > PSSC_SCAN_MAX_COMMENTS)
	{
		FAIL("%s: ncomments = %d", name, r->ncomments);
		return;
	}
	if (end - start > window && pos == A)
		lo = end - window;
	else if (end - start > window && pos == P)
		hi = start + window;
	for (i = 0; i < r->ncomments; i++)
	{
		const PsscCommentSpan *c = &r->comments[i];

		if (!complete_comment(s, start, end, c->offset, c->len))
			FAIL("%s: span %d [%zu,+%zu) is not a complete comment", name, i,
				 c->offset, c->len);
		else if (c->offset < lo || c->offset + c->len > hi)
			FAIL("%s: span %d [%zu,+%zu) outside the window [%zu,%zu)", name, i,
				 c->offset, c->len, lo, hi);
		if (i > 0 && c->offset < c[-1].offset + c[-1].len)
			FAIL("%s: span %d out of order", name, i);
		else if (i > 0 && pos != Y && !only_ws(s, c[-1].offset + c[-1].len, c->offset))
			FAIL("%s: spans %d and %d are not one run", name, i - 1, i);
		if (pos == A && s[c->offset] != '/')
			FAIL("%s: append span %d is not a block comment", name, i);
	}
	if (r->ncomments > 0 && pos == A)
	{
		const PsscCommentSpan *c = &r->comments[r->ncomments - 1];

		if (!only_ws_semi(s, c->offset + c->len, end))
			FAIL("%s: append run is not at the end of the range", name);
	}
	if (r->ncomments > 0 && pos == P && !only_ws_semi(s, start, r->comments[0].offset))
		FAIL("%s: prepend run is not at the start of the range", name);
}

static void
run_stmt_case(const StmtCase *c)
{
	size_t		inlen = strlen(c->in);
	int			scs;
	int			len = c->len;
	PsscStmtRange range;

	if (len == UPTO_SEMI)
		len = (int) (strchr(c->in + c->loc, ';') - (c->in + c->loc));
	range = pssc_stmt_range(c->in, c->loc, len);
	if (range.start > range.end || range.end > inlen)
	{
		FAIL("case \"%s\": bad range [%zu,%zu)", c->name, range.start, range.end);
		return;
	}

	for (scs = ON; scs <= OFF; scs <<= 1)
	{
		PsscScanResult r;
		int			nexp = 0;
		int			ok = 1;
		int			i;

		if (!(c->scs & scs))
			continue;
		checks++;
		stmt_everywhere(c->name, c->in, inlen, range.start, range.end, c->pos,
						c->window, scs == ON, &r);
		check_spans(c->name, c->in, range.start, range.end, c->pos, c->window, &r);

		while (c->exp[nexp])
			nexp++;
		if (r.ncomments != nexp || r.truncated != (c->truncated != 0) ||
			r.unterminated != (c->unterminated != 0) ||
			r.heuristic != (c->heuristic != 0))
			ok = 0;
		for (i = 0; ok && i < nexp; i++)
		{
			size_t		el = strlen(c->exp[i]);

			if (r.comments[i].len != el ||
				r.comments[i].offset > inlen || el > inlen - r.comments[i].offset ||
				memcmp(c->in + r.comments[i].offset, c->exp[i], el) != 0)
				ok = 0;
		}
		if (ok && nexp > 0 && c->off0 != NOOFF && r.comments[0].offset != (size_t) c->off0)
			ok = 0;
		if (!ok)
		{
			FAIL("case \"%s\" (scs %s, pos %d, window %zu, range [%zu,%zu)): input \"%s\"",
				 c->name, scs == ON ? "on" : "off", c->pos, c->window,
				 range.start, range.end, c->in);
			fprintf(stderr, "    want %d comment(s), truncated=%d unterminated=%d heuristic=%d:",
					nexp, c->truncated, c->unterminated, c->heuristic);
			for (i = 0; i < nexp; i++)
				fprintf(stderr, " \"%s\"", c->exp[i]);
			fprintf(stderr, "\n");
			print_result(c->in, &r);
		}
	}
}

static void
footer_at(const char *s, size_t stmt_end, int pos, size_t window,
		  PsscScanResult *r)
{
	memset(r, 0x7f, sizeof(*r));
	pssc_scan_footer(s, stmt_end, (PsscPosition) pos, window, true, r);
}

static void
run_footer_case(const FooterCase *c)
{
	size_t		inlen = c->inlen ? c->inlen : strlen(c->in);
	size_t		rest = inlen - c->stmt_end;
	Guarded		g = guarded_alloc(inlen + 32);
	PsscScanResult r,
				r2;
	char	   *s;
	int			nexp = 0;
	int			ok = 1;
	int			i;

	checks++;
	/* the string and its NUL end exactly at the high guard page */
	s = g.data + g.usable - (inlen + 1);
	memcpy(s, c->in, inlen);
	s[inlen] = '\0';
	footer_at(s, c->stmt_end, c->pos, c->window, &r);

	/* only the bytes from stmt_end on, right after the low guard page */
	memcpy(g.data, c->in + c->stmt_end, rest);
	g.data[rest] = '\0';
	footer_at(g.data - c->stmt_end, c->stmt_end, c->pos, c->window, &r2);
	if (!same_result(&r, &r2, 0))
		FAIL("footer \"%s\": result depends on bytes before stmt_end", c->name);
	guarded_free(&g);

	while (c->exp[nexp])
		nexp++;
	if (r.ncomments != nexp || r.truncated != (c->truncated != 0) ||
		r.unterminated || r.heuristic)
		ok = 0;
	for (i = 0; ok && i < nexp; i++)
	{
		size_t		el = strlen(c->exp[i]);

		if (r.comments[i].offset < c->stmt_end || r.comments[i].len != el ||
			r.comments[i].offset > inlen || el > inlen - r.comments[i].offset ||
			memcmp(c->in + r.comments[i].offset, c->exp[i], el) != 0 ||
			!complete_comment(c->in, c->stmt_end, inlen, r.comments[i].offset, el))
			ok = 0;
	}
	if (!ok)
	{
		FAIL("footer \"%s\" (stmt_end %zu, pos %d, window %zu): input \"%s\"",
			 c->name, c->stmt_end, c->pos, c->window, c->in);
		fprintf(stderr, "    want %d comment(s), truncated=%d:", nexp, c->truncated);
		for (i = 0; i < nexp; i++)
			fprintf(stderr, " \"%s\"", c->exp[i]);
		fprintf(stderr, "\n");
		print_result(c->in, &r);
	}
}

static void
run_range_case(const RangeCase *c)
{
	size_t		len = strlen(c->in);
	Guarded		g = guarded_alloc(len + 1);
	char	   *s = g.data + g.usable - (len + 1);
	PsscStmtRange r;

	checks++;
	memcpy(s, c->in, len + 1);
	r = pssc_stmt_range(s, c->loc, c->len);
	if (r.start != c->want_start || r.end != c->want_end)
		FAIL("range(\"%s\", %d, %d) = [%zu,%zu), want [%zu,%zu)", c->in, c->loc,
			 c->len, r.start, r.end, c->want_start, c->want_end);
	guarded_free(&g);
}

/*
 * Explicit location and length must not read the string (no strlen): the
 * buffer has no NUL and ends at a guard page.
 */
static void
run_range_no_strlen(void)
{
	Guarded		g = guarded_alloc(8);
	char	   *s = g.data + g.usable - 6;
	PsscStmtRange r;

	checks++;
	memcpy(s, "abcdef", 6);
	r = pssc_stmt_range(s, 1, 5);
	if (r.start != 1 || r.end != 6)
		FAIL("range without NUL = [%zu,%zu), want [1,6)", r.start, r.end);
	guarded_free(&g);
}

/* ---------------- owned statement ranges ---------------- */

static size_t
owned_at(const char *s, size_t from, size_t stmt_start, size_t max_bytes,
		 int scs_on, const char *what)
{
	size_t		r;

#ifdef PSSC_SCAN_CHECKED
	pssc_scan_reads = 0;
#endif
	r = pssc_stmt_owned_start(s, from, stmt_start, max_bytes, scs_on != 0);
#ifdef PSSC_SCAN_CHECKED
	if (stmt_start - from > max_bytes || from >= stmt_start)
	{
		if (pssc_scan_reads != 0)
			FAIL("%s: %lu byte reads, want none", what, pssc_scan_reads);
	}
	else if (pssc_scan_reads > 2 * (stmt_start - from) + 2)
		FAIL("%s: %lu byte reads for a %zu-byte prefix", what, pssc_scan_reads,
			 stmt_start - from);
#else
	(void) what;
#endif
	return r;
}

/*
 * pssc_stmt_owned_start() on s[from, stmt_start) placed so that from is right
 * after the low guard page and stmt_start right before the high one (only
 * that prefix may be read), and in the full string.
 */
static size_t
owned_everywhere(const char *name, const char *in, size_t from,
				 size_t stmt_start, size_t max_bytes, int scs_on)
{
	size_t		n = stmt_start >= from ? stmt_start - from : 0;
	Guarded		g = guarded_alloc(n + 1);
	char	   *s = g.data + g.usable - n;
	size_t		want = owned_at(in, from, stmt_start, max_bytes, scs_on, name);
	size_t		r;

	if (from <= stmt_start)
	{
		memcpy(s, in + from, n);
		r = owned_at(s - from, from, stmt_start, max_bytes, scs_on, name);
		if (r != want)
			FAIL("%s: result depends on bytes outside [from, stmt_start) (%zu vs %zu)",
				 name, r, want);
		memcpy(g.data, in + from, n);
		r = owned_at(g.data - from, from, stmt_start, max_bytes, scs_on, name);
		if (r != want)
			FAIL("%s: result differs after the low guard page (%zu vs %zu)",
				 name, r, want);
	}
	guarded_free(&g);
	return want;
}

static void
expect_spans(const char *what, const char *s, const PsscScanResult *r,
			 const char *const *exp)
{
	int			nexp = 0;
	int			ok;
	int			i;

	while (exp[nexp])
		nexp++;
	ok = r->ncomments == nexp && !r->truncated && !r->unterminated && !r->heuristic;
	for (i = 0; ok && i < nexp; i++)
		ok = r->comments[i].len == strlen(exp[i]) &&
			memcmp(s + r->comments[i].offset, exp[i], r->comments[i].len) == 0;
	if (!ok)
	{
		FAIL("%s: input \"%s\"", what, s);
		fprintf(stderr, "    want %d comment(s):", nexp);
		for (i = 0; i < nexp; i++)
			fprintf(stderr, " \"%s\"", exp[i]);
		fprintf(stderr, "\n");
		print_result(s, r);
	}
}

static void
run_server_stmt(const ServerStmt *c, size_t prev_end)
{
	size_t		inlen = strlen(c->q);
	PsscStmtRange r14 = pssc_stmt_range(c->q, c->loc14, c->len14);
	PsscStmtRange r18 = pssc_stmt_range(c->q, c->loc18, c->len18);
	int			scs;

	checks++;
	if (r14.end != r18.end || r18.start < r14.start || r14.end > inlen)
	{
		FAIL("server \"%s\": inconsistent rows [%zu,%zu) vs [%zu,%zu)", c->q,
			 r14.start, r14.end, r18.start, r18.end);
		return;
	}
	for (scs = ON; scs <= OFF; scs <<= 1)
	{
		size_t		froms[2];
		int			f;
		PsscScanResult r;
		char		what[160];

		froms[0] = 0;
		froms[1] = prev_end;
		for (f = 0; f < 2; f++)
		{
			size_t		o18 = owned_everywhere(c->q, c->q, froms[f], r18.start, DEF, scs == ON);
			size_t		o14 = owned_everywhere(c->q, c->q, froms[f], r14.start, DEF, scs == ON);

			if (o18 != r14.start || o14 != r14.start)
				FAIL("server \"%s\" stmt at %d/%d: owned start from %zu = %zu (PG18), %zu (PG14), want %zu",
					 c->q, c->loc14, c->loc18, froms[f], o18, o14, r14.start);
		}
		snprintf(what, sizeof(what), "server stmt at %d/%d any", c->loc14, c->loc18);
		stmt_everywhere(what, c->q, inlen, r14.start, r14.end, Y, DEF, scs == ON, &r);
		expect_spans(what, c->q, &r, c->any);
		snprintf(what, sizeof(what), "server stmt at %d/%d append", c->loc14, c->loc18);
		stmt_everywhere(what, c->q, inlen, r14.start, r14.end, A, DEF, scs == ON, &r);
		check_spans(what, c->q, r14.start, r14.end, A, DEF, &r);
		expect_spans(what, c->q, &r, c->app);
		snprintf(what, sizeof(what), "server stmt at %d/%d prepend", c->loc14, c->loc18);
		stmt_everywhere(what, c->q, inlen, r14.start, r14.end, P, DEF, scs == ON, &r);
		check_spans(what, c->q, r14.start, r14.end, P, DEF, &r);
		expect_spans(what, c->q, &r, c->pre);
		snprintf(what, sizeof(what), "server stmt at %d/%d footer", c->loc14, c->loc18);
		pssc_scan_footer(c->q, r14.end, A, DEF, scs == ON, &r);
		expect_spans(what, c->q, &r, c->foot);
	}
}

static void
run_owned_case(const OwnedCase *c)
{
	const char *in = c->in;
	size_t		from = 0,
				stmt_start,
				want;
	const char *p,
			   *q = NULL;
	int			scs;

	if (c->from_at)
		from = (size_t) (strstr(in, c->from_at) - in);
	for (p = in; (p = strstr(p, c->stmt_at)) != NULL; p++)
		q = p;
	stmt_start = (size_t) (q - in);
	want = stmt_start;
	if (c->want_at)
		want = *c->want_at ? (size_t) (strstr(in, c->want_at) - in) + strlen(c->want_at) : 0;
	for (scs = ON; scs <= OFF; scs <<= 1)
	{
		size_t		r;

		if (!(c->scs & scs))
			continue;
		checks++;
		r = owned_everywhere(c->name, in, from, stmt_start, c->max_bytes, scs == ON);
		if (r != want)
			FAIL("owned \"%s\" (scs %s): from %zu, stmt_start %zu: got %zu, want %zu",
				 c->name, scs == ON ? "on" : "off", from, stmt_start, r, want);
	}
}

/* ---------------- windowed scans read O(scan_window) ---------------- */

static void
run_big(void)
{
	size_t		n = 1 << 20;
	size_t		window = 2048;
	char	   *buf = malloc(n + 64);
	PsscScanResult r;
	size_t		i,
				len;
	static const char head[] = "/*controller:h*/ SELECT 1 WHERE x IN (";
	static const char tail[] = "1) /*controller:t*/ ;\n";

	if (!buf)
		exit(2);
	memcpy(buf, head, sizeof(head) - 1);
	for (i = sizeof(head) - 1; i + sizeof(tail) < n; i += 3)
		memcpy(buf + i, "1, ", 3);
	memcpy(buf + i, tail, sizeof(tail));
	len = i + sizeof(tail) - 1;

	checks++;
	stmt_everywhere("1MB append", buf, len, 0, len, A, window, 1, &r);
	if (r.ncomments != 1 || !r.heuristic ||
		memcmp(buf + r.comments[0].offset, "/*controller:t*/", 16) != 0)
	{
		FAIL("1MB append");
		print_result(buf, &r);
	}
	checks++;
	stmt_everywhere("1MB prepend", buf, len, 0, len, P, window, 1, &r);
	if (r.ncomments != 1 || r.heuristic || r.comments[0].offset != 0 ||
		r.comments[0].len != 16)
	{
		FAIL("1MB prepend");
		print_result(buf, &r);
	}
	checks++;
	stmt_everywhere("1MB any", buf, len, 0, len, Y, window, 1, &r);
	if (r.ncomments != 2 || r.heuristic)
	{
		FAIL("1MB any");
		print_result(buf, &r);
	}
	free(buf);
}

/* ---------------- randomized property check ---------------- */

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint32_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t) (rng >> 11);
}

/*
 * Reference for the exact modes, built from pssc_scan_comments() over the
 * whole range. Returns 0 if the reference cannot be built (truncated).
 */
static int
reference(const char *s, size_t len, int pos, size_t window, int scs_on,
		  PsscScanResult *ref)
{
	PsscScanResult all;

	pssc_scan_comments(s, 0, len, scs_on != 0, SIZE_MAX, &all);
	if (all.truncated)
		return 0;
	memset(ref, 0, sizeof(*ref));
	if (pos == Y)
	{
		pssc_scan_comments(s, 0, len, scs_on != 0, window, ref);
		return 1;
	}
	if (all.ncomments == 0)
		return 1;
	if (pos == A)
	{
		/* the trailing run of block comments, at most its last 16 */
		int			k = all.ncomments - 1;
		int			first;
		PsscCommentSpan last = all.comments[k];

		if (all.unterminated || s[last.offset] != '/' ||
			!only_ws_semi(s, last.offset + last.len, len))
			return 1;
		first = k;
		while (first > 0 && s[all.comments[first - 1].offset] == '/' &&
			   only_ws(s, all.comments[first - 1].offset + all.comments[first - 1].len,
					   all.comments[first].offset))
			first--;
		if (k - first + 1 > PSSC_SCAN_MAX_COMMENTS)
		{
			first = k + 1 - PSSC_SCAN_MAX_COMMENTS;
			ref->truncated = true;
		}
		ref->ncomments = k - first + 1;
		memcpy(ref->comments, all.comments + first,
			   sizeof(PsscCommentSpan) * ref->ncomments);
	}
	else
	{
		/* the leading run within the first window, at most 16 */
		size_t		hend = len > window ? window : len;
		int			k;

		if (!only_ws_semi(s, 0, all.comments[0].offset))
			return 1;
		for (k = 0; k < all.ncomments; k++)
		{
			PsscCommentSpan c = all.comments[k];

			if (c.offset + c.len > hend)
				break;
			if (k > 0 && !only_ws(s, all.comments[k - 1].offset + all.comments[k - 1].len,
								  c.offset))
				break;
			if (k == PSSC_SCAN_MAX_COMMENTS)
			{
				ref->truncated = true;
				break;
			}
			ref->comments[k] = c;
			ref->ncomments = k + 1;
		}
	}
	return 1;
}

static void
run_random(void)
{
	static const char alpha[] = "/*-'$\" \n;aE\\";
	static const char safe[] = "/* \n;a";	/* comments only, no strings */
	char		buf[48];
	int			iter;
	unsigned long tail_found = 0,
				tail_full = 0,
				tail_checked = 0;

	for (iter = 0; iter < 300000; iter++)
	{
		int			only_comments = (iter % 3) == 0;
		const char *al = only_comments ? safe : alpha;
		size_t		nal = strlen(al);
		size_t		len = rnd() % 40;
		size_t		window = rnd() % 44;
		int			pos = (int) (rnd() % 3);
		int			scs_on = (int) (rnd() % 2);
		size_t		i;
		PsscScanResult r,
					ref;
		char		name[64];

		for (i = 0; i < len; i++)
			buf[i] = al[rnd() % nal];
		/* make comment-shaped tails likely, so the tail path finds some */
		if (only_comments && len >= 6 && rnd() % 2)
		{
			size_t		e = len - rnd() % 3;
			size_t		o = rnd() % (e - 3);

			memcpy(buf + e - 2, "*/", 2);
			memcpy(buf + o, "/*", 2);
			if (e - o >= 9 && rnd() % 2)
			{
				size_t		m = o + 2 + rnd() % (e - o - 8);

				memcpy(buf + m, rnd() % 2 ? "*/ /*" : "*//*", 4 + (rnd() % 2));
			}
		}
		snprintf(name, sizeof(name), "random #%d", iter);

		memset(&r, 0x7f, sizeof(r));
		pssc_scan_statement(buf, 0, len, (PsscPosition) pos, window, scs_on, &r);
		checks++;
		check_spans(name, buf, 0, len, pos, window, &r);
		if (r.heuristic != (pos == A && len > window))
			FAIL("%s: heuristic = %d", name, (int) r.heuristic);
		if (pos != Y && (r.truncated || r.unterminated))
			FAIL("%s: positional scan set truncated/unterminated", name);

		if (pos == A && len > window)
		{
			PsscScanResult all;

			/*
			 * Tail path: on input made only of comments and other bytes,
			 * that lexes completely, it may miss comments of the run but
			 * must report a suffix of the exact run, never anything else.
			 */
			if (!only_comments)
				continue;
			pssc_scan_comments(buf, 0, len, true, SIZE_MAX, &all);
			if (all.unterminated || all.truncated)
				continue;
			if (!reference(buf, len, A, SIZE_MAX, 1, &ref))
				continue;
			tail_checked++;
			if (r.ncomments > 0)
			{
				int			k,
							d = ref.ncomments - r.ncomments;

				tail_found++;
				if (r.ncomments == ref.ncomments)
					tail_full++;
				for (k = 0; d >= 0 && k < r.ncomments; k++)
					if (ref.comments[d + k].offset != r.comments[k].offset ||
						ref.comments[d + k].len != r.comments[k].len)
						d = -1;
				if (d < 0)
				{
					FAIL("%s: tail path result is not a suffix of the exact run: \"%.*s\"",
						 name, (int) len, buf);
					print_result(buf, &ref);
					print_result(buf, &r);
				}
			}
			continue;
		}
		if (!reference(buf, len, pos, window, scs_on, &ref))
			continue;
		ref.heuristic = r.heuristic;
		if (pos != Y)
			ref.truncated = ref.unterminated = false;
		if (!same_result(&ref, &r, 0))
		{
			FAIL("%s: pos %d window %zu scs %d: \"%.*s\" differs from reference",
				 name, pos, window, scs_on, (int) len, buf);
			print_result(buf, &ref);
			print_result(buf, &r);
		}
	}
	if (tail_found == 0 || tail_checked == 0)
		FAIL("random: the tail path was never exercised (found %lu of %lu)",
			 tail_found, tail_checked);
	printf("random: tail path found comments in %lu of %lu comparable inputs (the whole run in %lu)\n",
		   tail_found, tail_checked, tail_full);
}

/* ---------------- fuzz corpus ---------------- */

static int
emit_corpus(const char *dir)
{
	size_t		i;

	for (i = 0; i < NSTMT; i++)
	{
		char		path[4096];
		FILE	   *f;
		size_t		len = strlen(stmt_cases[i].in);

		snprintf(path, sizeof(path), "%s/stmt-%03zu.sql", dir, i);
		f = fopen(path, "wb");
		if (!f || fwrite(stmt_cases[i].in, 1, len, f) != len || fclose(f) != 0)
		{
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return 1;
		}
	}
	printf("wrote %zu files to %s\n", (size_t) NSTMT, dir);
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

	for (i = 0; i < NRANGE; i++)
		run_range_case(&range_cases[i]);
	run_range_no_strlen();
	for (i = 0; i < NSTMT; i++)
		run_stmt_case(&stmt_cases[i]);
	for (i = 0; i < NFOOTER; i++)
		run_footer_case(&footer_cases[i]);
	for (i = 0; i < NSERVER; i++)
	{
		size_t		prev_end = 0;

		if (i > 0 && strcmp(server_stmts[i].q, server_stmts[i - 1].q) == 0)
			prev_end = pssc_stmt_range(server_stmts[i - 1].q, server_stmts[i - 1].loc14,
									   server_stmts[i - 1].len14).end;
		run_server_stmt(&server_stmts[i], prev_end);
	}
	for (i = 0; i < NOWNED; i++)
		run_owned_case(&owned_cases[i]);
	run_big();
	run_random();

	if (failures)
	{
		printf("FAILED: %d failure(s) in %d checks\n", failures, checks);
		return 1;
	}
	printf("ok: %d checks passed (%zu range, %zu statement, %zu footer, %zu server, %zu owned cases)\n",
		   checks, (size_t) NRANGE, (size_t) NSTMT, (size_t) NFOOTER,
		   (size_t) NSERVER, (size_t) NOWNED);
	return 0;
}
