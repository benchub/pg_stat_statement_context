/*
 * tagout.c
 *		Output of stored tag sets as jsonb: see tagout.h.
 */
#include "postgres.h"

#include "mb/pg_wchar.h"

#include "tagout.h"
#include "tagset.h"

/* s[0, len) as text of the server encoding; sets *outlen. */
static const char *
tag_text(const char *s, size_t len, int encoding, size_t *outlen)
{
	char	   *out;

	if (encoding == PG_SQL_ASCII)
	{
		out = palloc(pssc_tag_escaped_len(s, len) + 1);
		*outlen = pssc_tag_escape(s, len, out);
		out[*outlen] = '\0';
		return out;
	}
	out = pg_any_to_server(s, (int) len, encoding);
	*outlen = (out == s) ? len : strlen(out);
	return out;
}

static void
push_text(JsonbParseState **st, JsonbIteratorToken tok, const char *s,
		  size_t len, int encoding)
{
	JsonbValue	v;
	size_t		outlen;

	v.type = jbvString;
	v.val.string.val = (char *) tag_text(s, len, encoding, &outlen);
	v.val.string.len = (int) outlen;
	(void) pushJsonbValue(st, tok, &v);
}

void
pssc_tags_push_jsonb(JsonbParseState **st, const char *tags, size_t len,
					 int encoding)
{
	size_t		off = 0;

	(void) pushJsonbValue(st, WJB_BEGIN_OBJECT, NULL);
	while (off < len)
	{
		const char *k = tags + off;
		size_t		klen = strnlen(k, len - off);
		const char *v = k + klen + 1;
		size_t		vlen;

		if (off + klen + 1 >= len)
			break;				/* malformed: no value */
		vlen = strnlen(v, len - (off + klen + 1));
		push_text(st, WJB_KEY, k, klen, encoding);
		push_text(st, WJB_VALUE, v, vlen, encoding);
		off += klen + 1 + vlen + 1;
	}
	(void) pushJsonbValue(st, WJB_END_OBJECT, NULL);
}
