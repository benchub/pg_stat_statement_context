/*
 * pairs.c
 *		SQLCommenter and marginalia pair parsers. See pairs.h for the API and
 *		the accepted syntax, and DESIGN.md §4.2, §6.11, §9.
 *
 * Backend-independent on purpose (no postgres.h, palloc or elog) so that
 * test/unit and fuzz/ can build it standalone; test/unit's check-alloc
 * target verifies the object calls no allocator. Every body byte read goes
 * through at(), which the PSSC_PAIRS_CHECKED test build bounds-checks.
 *
 * Linear time: each pair is scanned once to find its extent and read once
 * more to decode it into the caller's buffer. An unterminated SQLCommenter
 * quote makes the search for its closing quote run to the end of the body,
 * and parsing resumes after the next ',' after the opening quote, so those
 * bytes are read again, but only once: that search treated every later '
 * as part of a \' escape (otherwise it would have closed the value), while
 * a value's opening quote follows '=' or whitespace, never a backslash, so
 * no later pair can start another quoted value.
 */
#include <string.h>

#include "pairs.h"

#ifdef PSSC_PAIRS_CHECKED
#include <stdio.h>
#include <stdlib.h>

unsigned long pssc_pairs_reads;
#endif

typedef struct Input
{
	const char *s;
	size_t		len;
} Input;

static inline unsigned char
at(const Input *in, size_t i)
{
#ifdef PSSC_PAIRS_CHECKED
	if (i >= in->len)
	{
		fprintf(stderr, "pairs.c: read at %zu outside [0, %zu)\n", i, in->len);
		abort();
	}
	pssc_pairs_reads++;
#endif
	return (unsigned char) in->s[i];
}

static inline bool
is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
		c == '\v';
}

static inline int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* ---------------- output ---------------- */

/*
 * Decodes one pair into out->buf at result->bufused. Bytes past bufsize are
 * counted but not written; commit() then drops the pair whole.
 */
typedef struct Writer
{
	const PsscPairOut *out;
	PsscPairResult *result;
	size_t		pos;			/* next byte of out->buf */
	size_t		key_start;
	size_t		value_start;
	unsigned int flags;
	unsigned int nul_flag;		/* flag a NUL byte sets: key or value */
} Writer;

static void
writer_begin(Writer *w)
{
	w->pos = w->result->bufused;
	w->key_start = w->pos;
	w->value_start = w->pos;
	w->flags = 0;
	w->nul_flag = PSSC_PAIR_KEY_NUL;
}

static void
writer_start_value(Writer *w)
{
	w->value_start = w->pos;
	w->nul_flag = PSSC_PAIR_VALUE_NUL;
}

static inline void
put(Writer *w, unsigned char c)
{
	if (c == '\0')
		w->flags |= w->nul_flag;
	if (w->pos < w->out->bufsize)
		w->out->buf[w->pos] = (char) c;
	w->pos++;
}

static void
commit(Writer *w)
{
	PsscPairResult *r = w->result;
	PsscPair   *p;

	if (w->pos > w->out->bufsize || r->npairs >= w->out->max_pairs)
	{
		r->ndropped++;
		return;
	}
	p = &w->out->pairs[r->npairs++];
	p->key = w->out->buf + w->key_start;
	p->keylen = w->value_start - w->key_start;
	p->value = w->out->buf + w->value_start;
	p->valuelen = w->pos - w->value_start;
	p->flags = w->flags;
	r->bufused = w->pos;
}

static void
result_init(PsscPairResult *result)
{
	result->npairs = 0;
	result->nmalformed = 0;
	result->ndropped = 0;
	result->bufused = 0;
}

/* ---------------- pssc_comment_body ---------------- */

bool
pssc_comment_body(const char *span, size_t len, size_t *body_off,
				  size_t *body_len)
{
	if (len >= 4 && span[0] == '/' && span[1] == '*' &&
		span[len - 2] == '*' && span[len - 1] == '/')
	{
		*body_off = 2;
		*body_len = len - 4;
		return true;
	}
	if (len >= 2 && span[0] == '-' && span[1] == '-')
	{
		*body_off = 2;
		*body_len = len - 2;
		return true;
	}
	*body_off = 0;
	*body_len = 0;
	return false;
}

/* ---------------- SQLCommenter ---------------- */

/*
 * Write s[from, to) with backslash escapes (\' and \\) undone and, if
 * url_decode, %XX escapes decoded and '+' turned into a space. Doing both in one left-to-right pass is
 * the same as unescaping first and then decoding (as the spec orders it),
 * because unescaping only yields ' and \, which never form or break a %XX
 * or a '+'.
 */
static void
sc_decode(const Input *in, size_t from, size_t to, bool url_decode, Writer *w)
{
	size_t		i = from;

	while (i < to)
	{
		unsigned char c = at(in, i);

		if (c == '\\' && i + 1 < to)
		{
			unsigned char n = at(in, i + 1);

			if (n == '\'' || n == '\\')
			{
				put(w, n);
				i += 2;
				continue;
			}
		}
		else if (c == '%' && url_decode)
		{
			int			hi = i + 1 < to ? hexval(at(in, i + 1)) : -1;
			int			lo = hi >= 0 && i + 2 < to ? hexval(at(in, i + 2)) : -1;

			if (lo >= 0)
			{
				put(w, (unsigned char) (hi << 4 | lo));
				i += 3;
				continue;
			}
			w->flags |= PSSC_PAIR_BAD_ESCAPE;
		}
		else if (c == '+' && url_decode)
		{
			put(w, ' ');
			i++;
			continue;
		}
		put(w, c);
		i++;
	}
}

/* Index of the next ',' at or after i, or in->len. */
static size_t
skip_to_comma(const Input *in, size_t i)
{
	while (i < in->len && at(in, i) != ',')
		i++;
	return i;
}

void
pssc_parse_sqlcommenter(const char *body, size_t len, bool url_decode,
						const PsscPairOut *out, PsscPairResult *result)
{
	Input		in = {body, len};
	Writer		w = {out, result, 0, 0, 0, 0, 0};
	size_t		i = 0;

	result_init(result);
	while (i < len)
	{
		size_t		key_start,
					key_end,
					q,
					close;
		bool		key_ok = true;
		bool		gap = false;	/* whitespace after key text so far */
		unsigned char c = 0;

		/* leading whitespace; an empty segment is not a pair */
		while (i < len && is_space(c = at(&in, i)))
			i++;
		if (i >= len)
			break;
		if (c == ',')
		{
			i++;
			continue;
		}

		/* key: up to the first '=', trimmed; no inner whitespace */
		key_start = key_end = i;
		while (i < len && (c = at(&in, i)) != '=' && c != ',')
		{
			if (is_space(c))
				gap = true;
			else
			{
				if (gap)
					key_ok = false;
				key_end = i + 1;
			}
			i++;
		}
		if (i >= len || c == ',')
		{
			/* no '=': free text such as a with_annotation comment */
			result->nmalformed++;
			i = i < len ? i + 1 : len;
			continue;
		}
		if (key_end == key_start)
			key_ok = false;
		i++;					/* '=' */

		/* value: a single-quoted string */
		while (i < len && is_space(c = at(&in, i)))
			i++;
		if (i >= len || c != '\'')
		{
			result->nmalformed++;
			i = skip_to_comma(&in, i);
			continue;
		}
		q = i++;
		close = len;
		while (i < len)
		{
			c = at(&in, i);
			if (c == '\\' && i + 1 < len)
			{
				unsigned char n = at(&in, i + 1);

				if (n == '\'' || n == '\\')
				{
					i += 2;
					continue;
				}
			}
			else if (c == '\'')
			{
				close = i;
				break;
			}
			i++;
		}
		if (close == len)
		{
			result->nmalformed++;
			i = skip_to_comma(&in, q + 1);
			continue;
		}

		/* only whitespace may follow, up to ',' or the end */
		i = close + 1;
		while (i < len && is_space(c = at(&in, i)))
			i++;
		if (i < len && c != ',')
		{
			result->nmalformed++;
			i = skip_to_comma(&in, i);
			continue;
		}
		if (!key_ok)
		{
			result->nmalformed++;
			continue;
		}

		writer_begin(&w);
		sc_decode(&in, key_start, key_end, url_decode, &w);
		writer_start_value(&w);
		sc_decode(&in, q + 1, close, url_decode, &w);
		commit(&w);
	}
}

/* ---------------- marginalia ---------------- */

/* Does sep (seplen > 0) occur at i, given that at(in, i) == c? */
static inline bool
sep_at(const Input *in, size_t i, unsigned char c, const char *sep,
	   size_t seplen)
{
	size_t		k;

	if (c != (unsigned char) sep[0] || seplen > in->len - i)
		return false;
	for (k = 1; k < seplen; k++)
		if (at(in, i + k) != (unsigned char) sep[k])
			return false;
	return true;
}

/*
 * Does sep start at any of i + 1 .. i + n - 1? A kv_sep at i must not
 * contain the start of a pair_sep: the segment ends there, so a kv_sep
 * crossing it is not inside the segment.
 */
static bool
sep_inside(const Input *in, size_t i, size_t n, const char *sep, size_t seplen)
{
	size_t		k;

	for (k = 1; k < n; k++)
		if (sep_at(in, i + k, at(in, i + k), sep, seplen))
			return true;
	return false;
}

void
pssc_parse_marginalia(const char *body, size_t len,
					  const PsscMarginaliaOpts *opts,
					  const PsscPairOut *out, PsscPairResult *result)
{
	Input		in = {body, len};
	Writer		w = {out, result, 0, 0, 0, 0, 0};
	const char *kv = ":";
	size_t		kvlen = 1;
	const char *ps = ",";
	size_t		pslen = 1;
	size_t		i = 0;

	if (opts && opts->kv_sep && opts->kv_sep_len > 0 &&
		opts->kv_sep_len <= PSSC_MAX_SEP_LEN)
	{
		kv = opts->kv_sep;
		kvlen = opts->kv_sep_len;
	}
	if (opts && opts->pair_sep && opts->pair_sep_len > 0 &&
		opts->pair_sep_len <= PSSC_MAX_SEP_LEN)
	{
		ps = opts->pair_sep;
		pslen = opts->pair_sep_len;
	}

	result_init(result);
	while (i < len)
	{
		/*
		 * One pass over the segment, up to the next pair_sep, collecting the
		 * trimmed key [key_start, key_end) before the first kv_sep and the
		 * trimmed value [val_start, val_end) after it. A pair_sep starting
		 * at a position ends the segment even where a kv_sep also matches,
		 * and a kv_sep match is rejected if a pair_sep starts inside it, so
		 * the result is the same as splitting on pair_sep first and then
		 * searching each segment for kv_sep.
		 */
		size_t		key_start = len,
					key_end = 0,
					val_start = 0,
					val_end = 0;
		bool		have_kv = false;
		bool		blank = true;
		bool		key_ok = true;
		bool		gap = false;
		size_t		k;

		while (i < len)
		{
			unsigned char c = at(&in, i);

			if (sep_at(&in, i, c, ps, pslen))
				break;
			if (!is_space(c))
				blank = false;
			if (!have_kv)
			{
				if (sep_at(&in, i, c, kv, kvlen) &&
					!sep_inside(&in, i, kvlen, ps, pslen))
				{
					have_kv = true;
					i += kvlen;
					val_start = val_end = i;
					continue;
				}
				if (is_space(c))
				{
					if (key_start < len)
						gap = true;
				}
				else
				{
					if (key_start == len)
						key_start = i;
					else if (gap)
						key_ok = false;
					key_end = i + 1;
				}
			}
			else if (!is_space(c))
			{
				if (val_end == val_start)
					val_start = i;
				val_end = i + 1;
			}
			i++;
		}
		if (i < len)
			i += pslen;			/* the pair_sep */

		if (blank)
			continue;
		if (!have_kv || key_start == len || !key_ok)
		{
			result->nmalformed++;
			continue;
		}

		writer_begin(&w);
		for (k = key_start; k < key_end; k++)
			put(&w, at(&in, k));
		writer_start_value(&w);
		for (k = val_start; k < val_end; k++)
			put(&w, at(&in, k));
		commit(&w);
	}
}
