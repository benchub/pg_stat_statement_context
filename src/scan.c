/*
 * scan.c
 *		Comment scanner: a forward lexer that finds SQL comments, following
 *		the rules of src/backend/parser/scan.l (PG14-18). See scan.h for the
 *		API and DESIGN.md §3.1, §6.2, §6.5, §6.11.
 *
 * Backend-independent on purpose (no postgres.h, palloc or elog) so that
 * test/unit and a fuzzer can build it standalone. Every byte read goes
 * through at(), which the PSSC_SCAN_CHECKED test build bounds-checks.
 *
 * Linear time: each construct is lexed once, except that the lookahead for a
 * string continuation (scan.l's xqs state) may examine the whitespace and
 * "--" comments after a closing quote twice, and a non-matching $tag$ inside
 * a dollar quote is compared against the opening tag as it is read.
 */
#include <stdint.h>
#include <string.h>

#include "scan.h"

#ifdef PSSC_SCAN_CHECKED
#include <stdio.h>
#include <stdlib.h>

unsigned long pssc_scan_reads;
#endif

/* Returned by the skip_* helpers when the range ends inside a construct. */
#define UNTERMINATED SIZE_MAX

typedef struct Lexer
{
	const char *s;
	size_t		start;
	size_t		end;
} Lexer;

/* String-literal flavors, after scan.l's start conditions. */
typedef enum StrKind
{
	STR_XQ,						/* '..' (scs on), U&'..': '' doubling only */
	STR_XE,						/* E'..', '..' (scs off): also backslash escapes */
	STR_XB						/* B'..', X'..': the first quote ends it */
} StrKind;

typedef enum EmitMode
{
	EMIT_ALL,					/* every comment, within the cap and budget */
	EMIT_RUN,					/* only the latest run of block comments */
	EMIT_NONE					/* no comments; only token tracking */
} EmitMode;

typedef struct Emitter
{
	PsscScanResult *result;
	EmitMode	mode;
	size_t		budget;			/* EMIT_ALL: comment bytes still allowed */
	bool		stopped;

	/* EMIT_RUN */
	const Lexer *lx;			/* to check the gaps between comments */
	bool		semi_sep;		/* ';' may separate the run's comments */
	bool		run_dropped;	/* the run lost comments to the cap */

	/* end of the last ';' token and of the last other non-comment token */
	size_t		semi_end;
	size_t		token_end;
} Emitter;

static inline unsigned char
at(const Lexer *lx, size_t i)
{
#ifdef PSSC_SCAN_CHECKED
	if (i < lx->start || i >= lx->end)
	{
		fprintf(stderr, "scan.c: read at %zu outside [%zu, %zu)\n",
				i, lx->start, lx->end);
		abort();
	}
	pssc_scan_reads++;
#endif
	return (unsigned char) lx->s[i];
}

/* scan.l {space}; \v is whitespace from PG17 on. */
static inline bool
is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
		c == '\f' || c == '\v';
}

static inline bool
is_newline(unsigned char c)
{
	return c == '\n' || c == '\r';
}

static inline bool
is_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

/* scan.l {ident_start} and {dolq_start}: [A-Za-z\200-\377_] */
static inline bool
is_ident_start(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		c == '_' || c >= 0x80;
}

/* scan.l {dolq_cont}: [A-Za-z\200-\377_0-9] ({ident_cont} also allows $) */
static inline bool
is_dolq_cont(unsigned char c)
{
	return is_ident_start(c) || is_digit(c);
}

static bool only_sep(const Lexer *lx, size_t p, size_t q, bool semi);

/*
 * EMIT_RUN: keep the latest run of block comments separated only by
 * whitespace (and ';' if semi_sep), at most its last PSSC_SCAN_MAX_COMMENTS.
 * A line comment or any token ends the run.
 */
static void
emit_run(Emitter *em, size_t offset, size_t len, bool block)
{
	PsscScanResult *r = em->result;

	/*
	 * Line comments never join a run. They need no reset here: the text of
	 * a line comment fails the next block comment's gap check, and a
	 * trailing one fails the caller's end check.
	 */
	if (!block)
		return;
	if (r->ncomments > 0)
	{
		PsscCommentSpan *prev = &r->comments[r->ncomments - 1];

		if (!only_sep(em->lx, prev->offset + prev->len, offset, em->semi_sep))
		{
			r->ncomments = 0;
			em->run_dropped = false;
		}
	}
	if (r->ncomments == PSSC_SCAN_MAX_COMMENTS)
	{
		memmove(r->comments, r->comments + 1,
				sizeof(PsscCommentSpan) * (PSSC_SCAN_MAX_COMMENTS - 1));
		r->ncomments--;
		em->run_dropped = true;
	}
	r->comments[r->ncomments].offset = offset;
	r->comments[r->ncomments].len = len;
	r->ncomments++;
}

/*
 * Record a complete comment. EMIT_ALL honors the count cap and the byte
 * budget, and returns false (and sets truncated) when it does not fit;
 * scanning stops.
 */
static bool
emit(Emitter *em, size_t offset, size_t len, bool block)
{
	PsscScanResult *r = em->result;

	if (em->mode == EMIT_RUN)
	{
		emit_run(em, offset, len, block);
		return true;
	}
	if (em->mode == EMIT_NONE)
		return true;
	if (r->ncomments >= PSSC_SCAN_MAX_COMMENTS || len > em->budget)
	{
		r->truncated = true;
		em->stopped = true;
		return false;
	}
	em->budget -= len;
	r->comments[r->ncomments].offset = offset;
	r->comments[r->ncomments].len = len;
	r->ncomments++;
	return true;
}

/*
 * i is at a slash-star. Returns the index just past the matching star-slash
 * (comments nest, as in scan.l's xc state), or UNTERMINATED.
 */
static size_t
skip_block_comment(const Lexer *lx, size_t i)
{
	size_t		p = i + 2;
	size_t		depth = 0;

	while (p < lx->end)
	{
		unsigned char c = at(lx, p);

		if (c == '*' && p + 1 < lx->end && at(lx, p + 1) == '/')
		{
			if (depth == 0)
				return p + 2;
			depth--;
			p += 2;
		}
		else if (c == '/' && p + 1 < lx->end && at(lx, p + 1) == '*')
		{
			depth++;
			p += 2;
		}
		else
			p++;
	}
	return UNTERMINATED;
}

/* i is just past "--". Returns the index of the terminating newline or end. */
static size_t
skip_line_comment(const Lexer *lx, size_t i)
{
	while (i < lx->end && !is_newline(at(lx, i)))
		i++;
	return i;
}

/*
 * scan.l's xqs state: p is just past a closing quote. If what follows is
 * whitespace and "--" comments containing at least one newline, then a quote,
 * the literal continues; return the index of that quote. Otherwise return
 * UNTERMINATED (meaning "no continuation"). *saw_comment reports whether a
 * "--" comment was crossed.
 */
static size_t
continuation_quote(const Lexer *lx, size_t p, bool *saw_comment)
{
	bool		saw_newline = false;

	*saw_comment = false;
	while (p < lx->end)
	{
		unsigned char c = at(lx, p);

		if (is_space(c))
		{
			if (is_newline(c))
				saw_newline = true;
			p++;
		}
		else if (c == '-' && p + 1 < lx->end && at(lx, p + 1) == '-')
		{
			/* the comment must end in a newline, which the loop then sees */
			p = skip_line_comment(lx, p + 2);
			*saw_comment = true;
		}
		else if (c == '\'' && saw_newline)
			return p;
		else
			break;
	}
	return UNTERMINATED;
}

/* Report the "--" comments in [p, q), which holds only whitespace and them. */
static bool
emit_line_comments(const Lexer *lx, Emitter *em, size_t p, size_t q)
{
	while (p < q)
	{
		if (at(lx, p) == '-')
		{
			size_t		e = skip_line_comment(lx, p + 2);

			if (!emit(em, p, e - p, false))
				return false;
			p = e;
		}
		else
			p++;
	}
	return true;
}

/*
 * p is just past the opening quote of a string literal. Returns the index
 * just past the closing quote (after any continuations), or UNTERMINATED.
 * Comments crossed by continuations are emitted; if that hits a limit,
 * em->stopped is set and the return value is meaningless.
 */
static size_t
skip_string(const Lexer *lx, Emitter *em, size_t p, StrKind kind)
{
	while (p < lx->end)
	{
		unsigned char c = at(lx, p);

		if (c == '\\' && kind == STR_XE)
		{
			/* any escape is \ plus one or more non-quote bytes */
			if (p + 1 >= lx->end)
				return UNTERMINATED;
			p += 2;
		}
		else if (c == '\'')
		{
			size_t		q;
			bool		saw_comment;

			if (kind != STR_XB && p + 1 < lx->end && at(lx, p + 1) == '\'')
			{
				p += 2;			/* '' */
				continue;
			}
			p++;
			q = continuation_quote(lx, p, &saw_comment);
			if (q == UNTERMINATED)
				return p;
			if (saw_comment && em && !emit_line_comments(lx, em, p, q))
				return p;
			p = q + 1;			/* same literal, same kind */
		}
		else
			p++;
	}
	return UNTERMINATED;
}

/* p is just past an opening double quote; "" is an escaped double quote. */
static size_t
skip_quoted_ident(const Lexer *lx, size_t p)
{
	while (p < lx->end)
	{
		if (at(lx, p) == '"')
		{
			if (p + 1 < lx->end && at(lx, p + 1) == '"')
				p += 2;
			else
				return p + 1;
		}
		else
			p++;
	}
	return UNTERMINATED;
}

/*
 * If a scan.l {dolqdelim} ($ tag? $) starts at the $ at index i, return the
 * index of its closing $; otherwise UNTERMINATED. The tag's first character
 * cannot be a digit.
 */
static size_t
dolq_delim_end(const Lexer *lx, size_t i)
{
	size_t		q = i + 1;

	if (q < lx->end && is_ident_start(at(lx, q)))
	{
		q++;
		while (q < lx->end && is_dolq_cont(at(lx, q)))
			q++;
	}
	if (q < lx->end && at(lx, q) == '$')
		return q;
	return UNTERMINATED;
}

/*
 * Dollar-quoted body. The opening delimiter's tag is s[tag, tag + taglen) and
 * p is just past the opening delimiter. Like scan.l's xdolq state, each $ that
 * starts a {dolqdelim} is compared with the opening one; on a mismatch its
 * final $ is rescanned ($a$ ... $x$a$ closes at the second $a$). Returns the
 * index just past the closing delimiter, or UNTERMINATED.
 */
static size_t
skip_dollar_body(const Lexer *lx, size_t tag, size_t taglen, size_t p)
{
	while (p < lx->end)
	{
		size_t		q = p + 1,
					k = 0;
		bool		match = true;
		unsigned char c;

		if (at(lx, p) != '$')
		{
			p++;
			continue;
		}
		if (q >= lx->end)
			break;
		/* candidate tag, compared with the opening tag as it is read */
		c = at(lx, q);
		if (is_ident_start(c))
		{
			for (;;)
			{
				if (match && (k >= taglen || at(lx, tag + k) != c))
					match = false;
				k++;
				if (++q >= lx->end)
					return UNTERMINATED;
				c = at(lx, q);
				if (!is_dolq_cont(c))
					break;
			}
		}
		if (c == '$' && match && k == taglen)
			return q + 1;
		/* rescan a mismatched delimiter's final $; skip any other byte */
		p = (c == '$') ? q : q + 1;
	}
	return UNTERMINATED;
}

static void
clear_result(PsscScanResult *result)
{
	result->ncomments = 0;
	result->truncated = false;
	result->unterminated = false;
	result->heuristic = false;
}

/*
 * Lex lx->s[start, end) from scan.l's INITIAL state, passing each complete
 * comment to emit(). Sets result->unterminated (the caller cleared result).
 */
static void
lex(Lexer lx, Emitter *em, bool standard_conforming_strings)
{
	PsscScanResult *result = em->result;
	StrKind		plain = standard_conforming_strings ? STR_XQ : STR_XE;
	size_t		i = lx.start;
	size_t		end = lx.end;

	while (i < end)
	{
		unsigned char c = at(&lx, i);
		size_t		next;
		bool		comment = false;

		if (c == '/' && i + 1 < end && at(&lx, i + 1) == '*')
		{
			/* also inside an operator: scan.l's {operator} stops at it */
			next = skip_block_comment(&lx, i);
			if (next == UNTERMINATED)
				break;
			if (!emit(em, i, next - i, true))
				return;
			comment = true;
		}
		else if (c == '-' && i + 1 < end && at(&lx, i + 1) == '-')
		{
			next = skip_line_comment(&lx, i + 2);
			if (!emit(em, i, next - i, false))
				return;
			comment = true;
		}
		else if (c == '\'')
			next = skip_string(&lx, em, i + 1, plain);
		else if (c == '"')
			next = skip_quoted_ident(&lx, i + 1);
		else if (c == '$')
		{
			size_t		delim_end;

			if (i + 1 < end && is_digit(at(&lx, i + 1)))
			{
				/* $n parameter: digits only (PG18 rejects trailing junk) */
				next = i + 2;
				while (next < end && is_digit(at(&lx, next)))
					next++;
			}
			else if ((delim_end = dolq_delim_end(&lx, i)) != UNTERMINATED)
				next = skip_dollar_body(&lx, i + 1, delim_end - (i + 1),
										delim_end + 1);
			else
				next = i + 1;	/* lone $, or $tag without closing $ */
		}
		else if (is_ident_start(c))
		{
			/* string prefixes count only at the start of a token */
			unsigned char c1 = i + 1 < end ? at(&lx, i + 1) : 0;

			if ((c == 'E' || c == 'e') && c1 == '\'')
				next = skip_string(&lx, em, i + 2, STR_XE);
			else if ((c == 'B' || c == 'b' || c == 'X' || c == 'x') && c1 == '\'')
				next = skip_string(&lx, em, i + 2, STR_XB);
			else if ((c == 'U' || c == 'u') && c1 == '&' && i + 2 < end &&
					 (at(&lx, i + 2) == '\'' || at(&lx, i + 2) == '"'))
			{
				if (at(&lx, i + 2) == '\'')
					next = skip_string(&lx, em, i + 3, STR_XQ);
				else
					next = skip_quoted_ident(&lx, i + 3);
			}
			else
			{
				/* identifier; {ident_cont} includes $, so a$b$ is one token */
				next = i + 1;
				while (next < end &&
					   (is_dolq_cont(at(&lx, next)) || at(&lx, next) == '$'))
					next++;
			}
		}
		else
		{
			/*
			 * Anything else, digits included, is a one-byte token for our
			 * purposes. For numbers this matches PG14, where 1E'..' is 1 then
			 * an E-string and 0x'..' is 0 then a hex string; PG15+ reject
			 * such "trailing junk" outright.
			 */
			next = i + 1;
		}

		if (em->stopped)
			return;
		if (next == UNTERMINATED)
		{
			result->unterminated = true;
			return;
		}
		if (!comment && !is_space(c))
		{
			/* ';' never starts a longer token */
			em->token_end = next;
			if (c == ';')
				em->semi_end = next;
		}
		i = next;
	}
	if (i < end)
		result->unterminated = true;	/* unterminated block comment */
}

void
pssc_scan_comments(const char *s, size_t start, size_t end,
				   bool standard_conforming_strings,
				   size_t max_comment_bytes, PsscScanResult *result)
{
	Lexer		lx;
	Emitter		em;

	lx.s = s;
	lx.start = start;
	lx.end = end;
	memset(&em, 0, sizeof(em));
	em.result = result;
	em.mode = EMIT_ALL;
	em.budget = max_comment_bytes;
	clear_result(result);
	lex(lx, &em, standard_conforming_strings);
}

bool
pssc_scan_only_trivia(const char *s, size_t start, size_t end)
{
	Lexer		lx;
	size_t		i = start;

	lx.s = s;
	lx.start = start;
	lx.end = end;
	while (i < end)
	{
		unsigned char c = at(&lx, i);

		if (c == ';' || is_space(c))
			i++;
		else if (c == '/' && i + 1 < end && at(&lx, i + 1) == '*')
		{
			i = skip_block_comment(&lx, i);
			if (i == UNTERMINATED)
				return false;
		}
		else if (c == '-' && i + 1 < end && at(&lx, i + 1) == '-')
			i = skip_line_comment(&lx, i + 2);
		else
			return false;
	}
	return true;
}

/* ---------------- statement ranges and positional scans ---------------- */

PsscStmtRange
pssc_stmt_range(const char *query, int stmt_location, int stmt_len)
{
	PsscStmtRange r;

	if (stmt_location < 0)
	{
		/* unknown location: the whole string, as in CleanQuerytext() */
		r.start = 0;
		r.end = strlen(query);
	}
	else
	{
		r.start = (size_t) stmt_location;
		if (stmt_len <= 0)
			r.end = r.start + strlen(query + r.start);
		else
			r.end = r.start + (size_t) stmt_len;
	}
	return r;
}

size_t
pssc_stmt_owned_start(const char *s, size_t from, size_t stmt_start,
					  size_t max_bytes, bool standard_conforming_strings)
{
	Lexer		lx;
	Emitter		em;
	PsscScanResult discard;
	size_t		none = from == 0 ? 0 : SIZE_MAX;

	if (from >= stmt_start || stmt_start - from > max_bytes)
		return stmt_start;

	lx.s = s;
	lx.start = from;
	lx.end = stmt_start;
	memset(&em, 0, sizeof(em));
	em.result = &discard;
	em.mode = EMIT_NONE;
	em.semi_end = none;
	em.token_end = none;
	clear_result(&discard);
	lex(lx, &em, standard_conforming_strings);

	/*
	 * The grammar puts only ';' tokens, whitespace and comments between two
	 * statements; anything else means from was not a statement boundary.
	 */
	if (discard.unterminated || em.semi_end == SIZE_MAX ||
		em.token_end != em.semi_end)
		return stmt_start;
	return em.semi_end;
}

static inline bool
is_space_or_semi(unsigned char c)
{
	return c == ';' || is_space(c);
}

static bool
only_space_or_semi(const Lexer *lx, size_t p, size_t q)
{
	for (; p < q; p++)
		if (!is_space_or_semi(at(lx, p)))
			return false;
	return true;
}

/* True if lx->s[p, q) is only whitespace, or also ';' if semi. */
static bool
only_sep(const Lexer *lx, size_t p, size_t q, bool semi)
{
	for (; p < q; p++)
	{
		unsigned char c = at(lx, p);

		if (!is_space(c) && !(semi && c == ';'))
			return false;
	}
	return true;
}

static void
add_span(PsscScanResult *result, size_t offset, size_t len)
{
	result->comments[result->ncomments].offset = offset;
	result->comments[result->ncomments].len = len;
	result->ncomments++;
}

/*
 * PREPEND: the leading run of lx's range. Only ';' and whitespace may precede
 * its first comment; the run's comments (block or line) are separated only by
 * whitespace, or also ';' if semi_sep. Every comment must end by hend
 * (start <= hend <= end); a line comment that reaches hend counts only if the
 * range ends there or a newline follows, so a comment cut by the window is
 * never reported. At most PSSC_SCAN_MAX_COMMENTS; truncated is set if another
 * comment of the run would follow.
 */
static void
scan_first(const Lexer *lx, size_t hend, bool semi_sep,
		   PsscScanResult *result)
{
	Lexer		head = *lx;
	size_t		i = lx->start;
	size_t		q;

	head.end = hend;
	while (i < hend && is_space_or_semi(at(&head, i)))
		i++;
	while (i + 1 < hend)
	{
		if (at(&head, i) == '/' && at(&head, i + 1) == '*')
		{
			q = skip_block_comment(&head, i);
			if (q == UNTERMINATED)
				break;			/* unterminated, or crosses the window end */
		}
		else if (at(&head, i) == '-' && at(&head, i + 1) == '-')
		{
			q = skip_line_comment(&head, i + 2);
			if (q == hend && hend < lx->end && !is_newline(at(lx, hend)))
				break;			/* cut by the window */
		}
		else
			break;
		if (result->ncomments == PSSC_SCAN_MAX_COMMENTS)
		{
			result->truncated = true;
			break;
		}
		add_span(result, i, q - i);
		i = q;
		while (i < hend && only_sep(&head, i, i + 1, semi_sep))
			i++;
	}
}

/*
 * APPEND, exact: lex the whole range forwards, keeping the latest run of
 * block comments (see emit_run()), and report it if only ';' and whitespace
 * follow it. A trailing line comment yields nothing, as on the tail path.
 */
static void
scan_last_exact(const Lexer *lx, bool semi_sep,
				bool standard_conforming_strings, PsscScanResult *result)
{
	Emitter		em;

	memset(&em, 0, sizeof(em));
	em.result = result;
	em.mode = EMIT_RUN;
	em.lx = lx;
	em.semi_sep = semi_sep;
	lex(*lx, &em, standard_conforming_strings);
	if (result->ncomments > 0)
	{
		PsscCommentSpan *c = &result->comments[result->ncomments - 1];

		if (result->unterminated ||
			!only_space_or_semi(lx, c->offset + c->len, lx->end))
			result->ncomments = 0;
	}
	result->truncated = result->ncomments > 0 && em.run_dropped;
	result->unterminated = false;
}

/*
 * Tail path helper: s[p - 2, p) is a star-slash inside the window [wstart,
 * end). Walk backwards to the matching slash-star while tracking nesting
 * depth, and return its offset, or UNTERMINATED if the walk reaches the window
 * start (the comment crosses it), if the candidate is not one well-formed
 * comment when lexed forwards (the backward walk can pair ambiguous runs such
 * as slash-star-slash differently), or if it directly follows a '*' (in
 * star-slash-star the forward lexer may have read the star-slash as the end
 * of an earlier comment). That '*' may be the byte just before the window,
 * which lies within the range since range_start < wstart.
 */
static size_t
tail_open(const char *s, size_t range_start, size_t wstart, size_t p)
{
	Lexer		lx;
	size_t		q = p - 2;
	size_t		depth = 1;
	unsigned char b = 0;
	bool		have_b = false;

	lx.s = s;
	lx.start = wstart;
	lx.end = p;

	/* examine the byte pair s[q - 2], s[q - 1], moving left */
	for (;;)
	{
		unsigned char a;

		if (q - wstart < 2)
			return UNTERMINATED;	/* reached the window start */
		if (!have_b)
			b = at(&lx, q - 1);
		a = at(&lx, q - 2);
		have_b = false;
		if (a == '*' && b == '/')
		{
			depth++;
			q -= 2;
		}
		else if (a == '/' && b == '*')
		{
			q -= 2;
			if (--depth == 0)
				break;
		}
		else
		{
			q--;
			b = a;
			have_b = true;
		}
	}

	lx.start = range_start;
	if (at(&lx, q - 1) == '*')
		return UNTERMINATED;
	lx.start = q;
	if (skip_block_comment(&lx, q) != p)
		return UNTERMINATED;
	return q;
}

/*
 * True if "--" occurs in s[lo, o), scanning back from o and stopping at a
 * newline. *hit_newline reports whether the scan stopped at one.
 */
static bool
dashes_before(const Lexer *lx, size_t lo, size_t o, bool *hit_newline)
{
	size_t		i;

	*hit_newline = false;
	for (i = o; i > lo; i--)
	{
		unsigned char c = at(lx, i - 1);

		if (is_newline(c))
		{
			*hit_newline = true;
			return false;
		}
		if (c == '-' && i - 1 > lo && at(lx, i - 2) == '-')
			return true;
	}
	return false;
}

/*
 * APPEND, heuristic tail path for a range longer than the window
 * (DESIGN.md §6.2). Only s[wstart, end) is read (plus the one byte before it,
 * see tail_open()), and the lexer state at wstart is unknown, so this can
 * only look at comment delimiters. Trim trailing ';' and whitespace, then
 * collect block comments from the end backwards with tail_open() while only
 * whitespace separates them, stopping at the first one that fails (that one
 * and any before it are not part of the run).
 *
 * A candidate may still lie inside a line comment that began earlier on its
 * line. Going through the candidates in source order: the earliest is
 * rejected if "--" occurs between the preceding newline (or wstart) and it.
 * A later one is accepted if the one before it was (only whitespace lies
 * between them); otherwise it is accepted only if a newline lies between the
 * two and no "--" between that newline and it. The run is the accepted
 * suffix. Each byte of the window is examined O(1) times.
 */
static void
scan_last_tail(const char *s, size_t start, size_t wstart, size_t end,
			   PsscScanResult *result)
{
	Lexer		lx;
	PsscCommentSpan cand[PSSC_SCAN_MAX_COMMENTS + 1];
	int			n = 0;
	int			k;
	int			first = -1;
	size_t		p = end;
	bool		good = false;

	lx.s = s;
	lx.start = wstart;
	lx.end = end;
	result->heuristic = true;

	while (p > wstart && is_space_or_semi(at(&lx, p - 1)))
		p--;
	while (n <= PSSC_SCAN_MAX_COMMENTS)
	{
		size_t		o;

		if (p - wstart < 4 || at(&lx, p - 1) != '/' || at(&lx, p - 2) != '*')
			break;
		o = tail_open(s, start, wstart, p);
		if (o == UNTERMINATED)
			break;
		cand[n].offset = o;
		cand[n].len = p - o;
		n++;
		p = o;
		while (p > wstart && is_space(at(&lx, p - 1)))
			p--;
	}

	/* cand[] runs from the end backwards; check in source order */
	for (k = n - 1; k >= 0; k--)
	{
		bool		nl;

		if (k == n - 1)
			good = !dashes_before(&lx, wstart, cand[k].offset, &nl);
		else if (!good)
			good = !dashes_before(&lx, cand[k + 1].offset, cand[k].offset, &nl) &&
				nl;
		if (good && first < 0)
			first = k;
	}
	if (first < 0)
		return;
	if (first == PSSC_SCAN_MAX_COMMENTS)
	{
		first--;
		result->truncated = true;
	}
	for (k = first; k >= 0; k--)
		add_span(result, cand[k].offset, cand[k].len);
}

void
pssc_scan_statement(const char *s, size_t start, size_t end,
					PsscPosition position, size_t scan_window,
					bool standard_conforming_strings, PsscScanResult *result)
{
	Lexer		lx;
	bool		fits;

	clear_result(result);
	if (end < start)
		return;
	lx.s = s;
	lx.start = start;
	lx.end = end;
	fits = end - start <= scan_window;

	switch (position)
	{
		case PSSC_POS_APPEND:
			if (fits)
				scan_last_exact(&lx, false, standard_conforming_strings,
								result);
			else
				scan_last_tail(s, start, end - scan_window, end, result);
			break;
		case PSSC_POS_PREPEND:
			scan_first(&lx, fits ? end : start + scan_window, false, result);
			break;
		case PSSC_POS_ANY:
		default:
			pssc_scan_comments(s, start, end, standard_conforming_strings,
							   scan_window, result);
			break;
	}
}

void
pssc_scan_footer(const char *s, size_t stmt_end, PsscPosition position,
				 size_t scan_window, bool standard_conforming_strings,
				 PsscScanResult *result)
{
	Lexer		lx;
	size_t		n;

	clear_result(result);
	/* strnlen's bound must not exceed the maximum object size */
	if (scan_window >= PTRDIFF_MAX)
		n = strlen(s + stmt_end);
	else
		n = strnlen(s + stmt_end, scan_window + 1);
	if (n > scan_window)
		return;
	if (!pssc_scan_only_trivia(s, stmt_end, stmt_end + n))
		return;

	/* the footer is fully lexable, so every mode is exact */
	lx.s = s;
	lx.start = stmt_end;
	lx.end = stmt_end + n;
	switch (position)
	{
		case PSSC_POS_APPEND:
			scan_last_exact(&lx, true, standard_conforming_strings, result);
			break;
		case PSSC_POS_PREPEND:
			scan_first(&lx, lx.end, true, result);
			break;
		case PSSC_POS_ANY:
		default:
			pssc_scan_comments(s, lx.start, lx.end,
							   standard_conforming_strings, scan_window,
							   result);
			break;
	}
}
