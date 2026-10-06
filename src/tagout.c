/*
 * tagout.c
 *		Output of stored tag sets as jsonb: see tagout.h.
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "mb/pg_wchar.h"

#include "extract.h"
#include "tagout.h"
#include "tagset.h"

/* One key or value of a stored tag set, or its output text. */
typedef struct TagText
{
	const char *s;
	size_t		len;
} TagText;

/*
 * Splits tags[0, len) ("k\0v\0...") into its keys and values, alternating,
 * into a palloc'd array; returns their number (always even: a trailing key
 * without a value is ignored).
 */
static int
tags_split(const char *tags, size_t len, TagText **out)
{
	size_t		nul = 0;
	size_t		off = 0;
	int			n = 0;
	TagText    *t;

	for (size_t i = 0; i < len; i++)
		nul += (tags[i] == '\0');
	t = palloc(sizeof(TagText) * (nul + 2));
	while (off < len)
	{
		const char *k = tags + off;
		size_t		klen = strnlen(k, len - off);
		const char *v = k + klen + 1;
		size_t		vlen;

		if (off + klen + 1 >= len)
			break;				/* malformed: no value */
		vlen = strnlen(v, len - (off + klen + 1));
		t[n].s = k;
		t[n++].len = klen;
		t[n].s = v;
		t[n++].len = vlen;
		off += klen + 1 + vlen + 1;
	}
	*out = t;
	return n;
}

/* The SQL_ASCII output of s[0, len): see pssc_tag_escape(). */
static TagText
tag_escaped(const char *s, size_t len)
{
	char	   *out = palloc(pssc_tag_escaped_len(s, len) + 1);
	TagText		r;

	r.len = pssc_tag_escape(s, len, out);
	out[r.len] = '\0';
	r.s = out;
	return r;
}

/* s[0, len) as text of the server encoding (ERROR if not convertible). */
static TagText
tag_text(const char *s, size_t len, int encoding)
{
	TagText		r;
	char	   *out;

	if (encoding == PG_SQL_ASCII)
		return tag_escaped(s, len);
	out = pg_any_to_server(s, (int) len, encoding);
	r.s = out;
	r.len = (out == s) ? len : strlen(out);
	return r;
}

/*
 * pg_any_to_server() without raising an error for data that cannot be
 * converted (or is invalid): false then. The same cases, in the same order;
 * encoding is not PG_SQL_ASCII (the caller escapes those).
 */
static bool
tag_text_noerror(const char *s, size_t len, int encoding, TagText *r)
{
	int			server = GetDatabaseEncoding();
	Oid			proc;
	char	   *buf;
	int			converted;

	Assert(encoding != PG_SQL_ASCII);
	r->s = s;
	r->len = len;
	if (len == 0)
		return true;
	if (len > PSSC_TAGSET_BYTES_MAX)
		return false;			/* cannot happen: tags are that small */
	if (encoding == server)
		return pg_verify_mbstr(server, s, (int) len, true);
	if (server == PG_SQL_ASCII)
	{
		/* as pg_any_to_server(): validate, no conversion possible */
		if (PG_VALID_BE_ENCODING(encoding))
			return pg_verify_mbstr(encoding, s, (int) len, true);
		for (size_t i = 0; i < len; i++)
			if (IS_HIGHBIT_SET(s[i]))
				return false;
		return true;
	}

	proc = FindDefaultConversionProc(encoding, server);
	if (!OidIsValid(proc))
		return false;
	buf = palloc(len * MAX_CONVERSION_GROWTH + 1);
	converted = pg_do_encoding_conversion_buf(proc, encoding, server,
											  (unsigned char *) s, (int) len,
											  (unsigned char *) buf,
											  (int) (len * MAX_CONVERSION_GROWTH + 1),
											  true);
	if (converted != (int) len)
	{
		pfree(buf);
		return false;
	}
	r->s = buf;
	r->len = strlen(buf);
	return true;
}

/* Pushes the strings as one object; returns it (pushJsonbValue's result). */
static JsonbValue *
push_pairs(JsonbParseState **st, const TagText *t, int n)
{
	(void) pushJsonbValue(st, WJB_BEGIN_OBJECT, NULL);
	for (int i = 0; i < n; i++)
	{
		JsonbValue	v;

		v.type = jbvString;
		v.val.string.val = (char *) t[i].s;
		v.val.string.len = (int) t[i].len;
		(void) pushJsonbValue(st, (i % 2 == 0) ? WJB_KEY : WJB_VALUE, &v);
	}
	return pushJsonbValue(st, WJB_END_OBJECT, NULL);
}

void
pssc_tags_push_jsonb(JsonbParseState **st, const char *tags, size_t len,
					 int encoding)
{
	TagText    *t;
	int			n = tags_split(tags, len, &t);

	for (int i = 0; i < n; i++)
		t[i] = tag_text(t[i].s, t[i].len, encoding);
	(void) push_pairs(st, t, n);
	pfree(t);
}

Jsonb *
pssc_tags_jsonb_noerror(const char *tags, size_t len, int encoding,
						bool *escaped_fallback)
{
	JsonbParseState *st = NULL;
	TagText    *t;
	TagText    *conv;
	int			n = tags_split(tags, len, &t);
	bool		ok = true;
	Jsonb	   *result;

	conv = palloc(sizeof(TagText) * (n + 1));
	if (encoding != PG_SQL_ASCII)
	{
		for (int i = 0; ok && i < n; i++)
			ok = tag_text_noerror(t[i].s, t[i].len, encoding, &conv[i]);
	}
	if (encoding == PG_SQL_ASCII || !ok)
	{
		for (int i = 0; i < n; i++)
			conv[i] = tag_escaped(t[i].s, t[i].len);
	}
	result = JsonbValueToJsonb(push_pairs(&st, conv, n));
	pfree(t);
	pfree(conv);
	if (escaped_fallback != NULL)
		*escaped_fallback = !ok;
	return result;
}
