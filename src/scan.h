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

	/*
	 * True if the result came from the inexact tail path of
	 * pssc_scan_statement() (position append on a statement longer than
	 * scan_window), whether or not it found a comment. Feeds
	 * _info().heuristic_scans (DESIGN.md §6.2). Always false otherwise.
	 */
	bool		heuristic;

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

/* ---------------- statement ranges and positional scanning ---------------- */

/* Where an extractor expects its comment (DESIGN.md §4.2 "position"). */
typedef enum PsscPosition
{
	PSSC_POS_ANY,				/* every comment in the statement */
	PSSC_POS_APPEND,			/* the trailing run of block comments */
	PSSC_POS_PREPEND			/* the leading run of comments */
} PsscPosition;

/* A statement's byte range s[start, end) within its source text. */
typedef struct PsscStmtRange
{
	size_t		start;
	size_t		end;
} PsscStmtRange;

/*
 * Resolve a statement's range from the parser's stmt_location/stmt_len
 * (DESIGN.md §6.5), as CleanQuerytext() does: stmt_location < 0 (unknown)
 * means the whole string and ignores stmt_len; stmt_len <= 0 means from
 * stmt_location to the end of the string. Only those two cases call strlen().
 * query must be NUL-terminated and stmt_location/stmt_len must lie within it
 * (the core guarantees this; it is not checked).
 *
 * The start is the parser's, which differs by major version; callers should
 * replace it with pssc_stmt_owned_start() before scanning.
 */
extern PsscStmtRange pssc_stmt_range(const char *query, int stmt_location,
									 int stmt_len);

/*
 * The start of the text a statement owns: its leading whitespace and
 * comments, back to just after the previous statement's ';' (or the string
 * start). PG14-17 already report stmt_location there; PG18 reports the first
 * token, so a leading comment of the second statement in
 * "SELECT 1; <comment> SELECT 2;" lies outside [stmt_location, end). This
 * returns the same start on all versions (verified against real servers).
 *
 * s[from, stmt_start) is lexed forwards, so ';' inside strings, quoted
 * identifiers and comments is ignored. "from" must be a lexically safe
 * point: 0, or the end of an earlier statement's range in the same string
 * (callers cache it to keep a whole multi-statement string O(n)). Returns
 * the offset just after the last ';' token in the prefix; with from == 0 and
 * no ';', 0. Returns stmt_start unchanged (no extension, always safe) when
 * from >= stmt_start, when stmt_start - from > max_bytes (nothing is read,
 * so the cost is at most max_bytes reads), when the prefix ends inside an
 * unterminated literal or comment, when from > 0 and the prefix holds no
 * ';', or when a token other than ';' follows the last ';' (the caller's
 * range is then not a parser statement start).
 */
extern size_t pssc_stmt_owned_start(const char *s, size_t from,
									size_t stmt_start, size_t max_bytes,
									bool standard_conforming_strings);

/*
 * Find the comments of statement s[start, end) for an extractor with the given
 * position (DESIGN.md §6.2). Never reads outside [start, end) and never
 * reports part of a comment.
 *
 * If end - start <= scan_window, the range is lexed exactly from the front:
 *	- ANY: every comment, as pssc_scan_comments() with max_comment_bytes =
 *	  scan_window (so at most 16 comments and scan_window comment bytes).
 *	- APPEND: the trailing run: the last comment, if it is a block comment
 *	  followed only by ';' and whitespace, plus the block comments directly
 *	  before it separated only by whitespace (e.g. marginalia's structured
 *	  comment followed by its annotation), in source order. A trailing "--"
 *	  comment yields nothing, and a line comment or any token ends the run,
 *	  as on the tail path, so a statement's result does not change when it
 *	  grows past scan_window.
 *	- PREPEND: the leading run: the comments (block or line) before the first
 *	  token, after any ';' and whitespace, separated only by whitespace.
 * Otherwise:
 *	- ANY: still a full forward scan (same result as above).
 *	- PREPEND: as above, but each comment must end within the first
 *	  scan_window bytes (the run stops before one that does not) (a line comment's terminating newline may be the byte
 *	  just after the window). Exact; reads at most scan_window + 1 bytes.
 *	- APPEND: the heuristic tail path; result->heuristic is set. Within the
 *	  last scan_window bytes only: trim trailing ';' and whitespace, require a
 *	  closing star-slash, and walk backwards to the matching slash-star,
 *	  tracking nesting depth. Yields nothing if the walk reaches the window
 *	  start (the comment crosses it), if the candidate is not a well-formed
 *	  comment when re-lexed forwards, if it directly follows a '*' (in
 *	  star-slash-star the star-slash may close an earlier comment; this is
 *	  the one byte read before the window), or if "--" appears between the
 *	  candidate and the preceding newline (or window start), since the
 *	  candidate may then sit inside a line comment. The lexer state at the
 *	  window start is unknown, so a comment-like tail inside a string literal
 *	  or a line comment that began before the window can still be reported:
 *	  that is why the result is flagged heuristic. The run is collected the
 *	  same way, walking back over whitespace to earlier block comments (up to
 *	  17 candidates). The "--" check runs in source order: the earliest
 *	  candidate checks back to its newline (or the window start); a later
 *	  one is accepted if the one before it was, or if a newline with no
 *	  "--" after it lies between them. The reported run is the accepted
 *	  suffix. Reads O(scan_window) bytes.
 *
 * Runs hold at most 16 comments: APPEND keeps the last 16, PREPEND the first
 * 16, and truncated is set if more existed. unterminated is always false for
 * APPEND and PREPEND.
 */
extern void pssc_scan_statement(const char *s, size_t start, size_t end,
								PsscPosition position, size_t scan_window,
								bool standard_conforming_strings,
								PsscScanResult *result);

/*
 * Trailing-footer fallback (DESIGN.md §6.5): the comments after a statement
 * that ends at stmt_end, reported only if the rest of the NUL-terminated
 * string s holds nothing but ';', whitespace and complete comments, which
 * proves the statement is the last one. The caller uses them only if the
 * statement's own range produced no tags.
 *
 * The rest of the string is found with strnlen() and must be at most
 * scan_window bytes, otherwise nothing is reported. This bounds the work per
 * statement, so a string with many untagged statements is not scanned
 * quadratically. The position applies as in pssc_scan_statement() to the
 * footer: ANY reports every comment (cap 16, scan_window bytes), APPEND the
 * trailing run and PREPEND the leading run, where ';' as well as whitespace
 * may separate the comments of a run. Never
 * heuristic. Reads s[stmt_end] up to the NUL or scan_window + 1 bytes.
 */
extern void pssc_scan_footer(const char *s, size_t stmt_end,
							 PsscPosition position, size_t scan_window,
							 bool standard_conforming_strings,
							 PsscScanResult *result);

#ifdef PSSC_SCAN_CHECKED
/*
 * Test-only build (test/unit): every byte read asserts start <= i < end and
 * increments this counter, so tests can check the scan is linear.
 */
extern unsigned long pssc_scan_reads;
#endif

#endif							/* PSSC_SCAN_H */
