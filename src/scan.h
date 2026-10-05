/*
 * scan.h
 *		Comment scanner: a forward lexer that finds SQL comments
 *		(DESIGN.md §3.1 item 1, §6.2, §6.5, §6.11).
 *
 * Backend-independent: no postgres.h, no palloc, no elog, no heap
 * allocation, so it can be unit-tested and fuzzed standalone
 * (test/unit/test_scan.c). The scanner is a single O(n) pass that only reads
 * s[start] .. s[end - 1]; the input need not be NUL-terminated, and NUL bytes
 * inside the range are ordinary characters.
 *
 * Lexing follows src/backend/parser/scan.l (PG14-18), starting in its
 * INITIAL state at "start":
 *	- '...' strings with '' doubling; backslash escapes too when
 *	  standard_conforming_strings is off.
 *	- E'...' (always backslash escapes), U&'...', B'...', X'...' (no doubling:
 *	  the first quote ends them) and N'...' strings. A prefix letter counts only
 *	  at the start of a token: in xE'...' the identifier is "xE" and a plain
 *	  string follows. U& must be immediately followed by the quote.
 *	- String continuation: a closing quote followed by whitespace and "--"
 *	  comments containing at least one newline, then a quote, continues the
 *	  same literal in the same escape mode (E'a' <newline> 'b\'c' is one
 *	  E-string).
 *	- "quoted identifiers" and U&"..." with "" doubling.
 *	- $tag$ ... $tag$ dollar quotes (tag: [A-Za-z\200-\377_][A-Za-z\200-\377_0-9]*
 *	  or empty), $1 parameters, and identifiers that contain $ (a$b$c) never
 *	  start a dollar quote.
 *	- Nested slash-star comments and "--" line comments. Inside an operator
 *	  (e.g. "+--"), "--" and slash-star still start a comment.
 *
 * Multibyte text needs no special handling: every server encoding is
 * ASCII-safe, so bytes >= 0x80 never look like a delimiter. They are
 * identifier characters, as in scan.l.
 *
 * Known divergences from scan.l only affect input the server rejects with a
 * syntax error (such input never reaches the hooks): PG15+ reject "1abc"
 * and PG18 rejects "$1abc", which this scanner lexes the PG14 way (1E'..' is
 * the number 1 then an E-string), and PG14-16 treat \v as a non-whitespace
 * character.
 */
#ifndef PSSC_SCAN_H
#define PSSC_SCAN_H

#include <stddef.h>
#ifndef true					/* c.h (with stdbool.h) may already define bool */
#include <stdbool.h>
#endif

/* Max comments reported per range (DESIGN.md §6.11). */
#define PSSC_SCAN_MAX_COMMENTS	16

/*
 * One complete comment: s[offset] .. s[offset + len - 1], delimiters
 * included. A block comment starts with slash-star and ends with the
 * matching star-slash (nested comments are part of the outermost one). A line
 * comment starts with "--" and runs up to, not including, the terminating \n
 * or \r (or to "end"). Offsets are relative to s, not to start.
 */
typedef struct PsscCommentSpan
{
	size_t		offset;
	size_t		len;
} PsscCommentSpan;

typedef struct PsscScanResult
{
	int			ncomments;		/* valid entries in comments[] */

	/*
	 * True if the range holds at least one more complete comment than was
	 * reported, because PSSC_SCAN_MAX_COMMENTS was reached or because the
	 * next comment would push the reported bytes past max_comment_bytes.
	 */
	bool		truncated;

	/*
	 * True if the range ended inside a comment, string, quoted identifier or
	 * dollar quote. Such an incomplete construct never yields a span. Not
	 * computed (false) once truncated is set, since scanning stops there.
	 */
	bool		unterminated;

	PsscCommentSpan comments[PSSC_SCAN_MAX_COMMENTS];
} PsscScanResult;

/*
 * Lex s[start, end) from scan.l's INITIAL state and report its complete
 * comments in order.
 *
 * max_comment_bytes is the scan_window budget for comments (DESIGN.md §6.11):
 * comments are reported in order while the sum of their span lengths
 * (delimiters included) stays <= max_comment_bytes. The first comment that
 * would exceed it is not reported (a comment is never cut), result->truncated
 * is set, and scanning stops. Pass SIZE_MAX for no budget. Scanning also stops
 * at the first complete comment after PSSC_SCAN_MAX_COMMENTS have been
 * reported. Limiting how much of a long statement is lexed (head/tail
 * windows, §6.2) is the caller's job; this function lexes the range it is
 * given.
 */
extern void pssc_scan_comments(const char *s, size_t start, size_t end,
							   bool standard_conforming_strings,
							   size_t max_comment_bytes,
							   PsscScanResult *result);

/*
 * True if s[start, end) contains only ';', whitespace (scan.l's
 * [ \t\n\r\f\v]) and complete comments; an empty range qualifies. An
 * unterminated block comment does not qualify; a line comment may end at
 * "end". Used for the trailing-footer rule (DESIGN.md §6.5).
 */
extern bool pssc_scan_only_trivia(const char *s, size_t start, size_t end);

#ifdef PSSC_SCAN_CHECKED
/*
 * Test-only build (test/unit): every byte read asserts start <= i < end and
 * increments this counter, so tests can check the scan is linear.
 */
extern unsigned long pssc_scan_reads;
#endif

#endif							/* PSSC_SCAN_H */
