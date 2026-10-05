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

typedef struct Emitter
{
	PsscScanResult *result;
	size_t		budget;			/* comment bytes still allowed */
	bool		stopped;
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

/*
 * Record a complete comment, honoring the count cap and the byte budget.
 * Returns false (and sets truncated) when it does not fit; scanning stops.
 */
static bool
emit(Emitter *em, size_t offset, size_t len)
{
	PsscScanResult *r = em->result;

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

			if (!emit(em, p, e - p))
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

void
pssc_scan_comments(const char *s, size_t start, size_t end,
				   bool standard_conforming_strings,
				   size_t max_comment_bytes, PsscScanResult *result)
{
	Lexer		lx;
	Emitter		em;
	StrKind		plain = standard_conforming_strings ? STR_XQ : STR_XE;
	size_t		i = start;

	lx.s = s;
	lx.start = start;
	lx.end = end;
	em.result = result;
	em.budget = max_comment_bytes;
	em.stopped = false;
	result->ncomments = 0;
	result->truncated = false;
	result->unterminated = false;

	while (i < end)
	{
		unsigned char c = at(&lx, i);
		size_t		next;

		if (c == '/' && i + 1 < end && at(&lx, i + 1) == '*')
		{
			/* also inside an operator: scan.l's {operator} stops at it */
			next = skip_block_comment(&lx, i);
			if (next == UNTERMINATED)
				break;
			if (!emit(&em, i, next - i))
				return;
		}
		else if (c == '-' && i + 1 < end && at(&lx, i + 1) == '-')
		{
			next = skip_line_comment(&lx, i + 2);
			if (!emit(&em, i, next - i))
				return;
		}
		else if (c == '\'')
			next = skip_string(&lx, &em, i + 1, plain);
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
				next = skip_string(&lx, &em, i + 2, STR_XE);
			else if ((c == 'B' || c == 'b' || c == 'X' || c == 'x') && c1 == '\'')
				next = skip_string(&lx, &em, i + 2, STR_XB);
			else if ((c == 'U' || c == 'u') && c1 == '&' && i + 2 < end &&
					 (at(&lx, i + 2) == '\'' || at(&lx, i + 2) == '"'))
			{
				if (at(&lx, i + 2) == '\'')
					next = skip_string(&lx, &em, i + 3, STR_XQ);
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

		if (em.stopped)
			return;
		if (next == UNTERMINATED)
		{
			result->unterminated = true;
			return;
		}
		i = next;
	}
	if (i < end)
		result->unterminated = true;	/* unterminated block comment */
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
