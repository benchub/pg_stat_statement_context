/*
 * pssc_extract_test.c
 *		TEST-ONLY module that runs pg_stat_statement_context's tag-set
 *		pipeline (src/extract.h) on given text for test/t/005_extract.pl.
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It reaches the main library's exported
 * functions and variables through load_external_function(), so
 * pg_stat_statement_context must be in shared_preload_libraries.
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "parser/parser.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "extract.h"
#include "guc.h"
#include "scan.h"

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"
#define GUARD_BYTES 64
#define GUARD 0xA5

typedef void (*extract_fn) (const char *, size_t, size_t, char *, size_t,
							PsscExtractResult *);
typedef uint32 (*hash_fn) (const char *, size_t);
typedef void (*take_fn) (PsscTagsetStats *);
typedef void (*set_regex_fn) (PsscRegexExtractFn, void *);

static void *
main_sym(const char *name)
{
	return (void *) load_external_function(MAIN_LIB, name, true, NULL);
}

static int
key_cmp(const char *a, size_t alen, const char *b, size_t blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	if (c != 0)
		return c;
	return alen < blen ? -1 : alen > blen ? 1 : 0;
}

PG_FUNCTION_INFO_V1(pssc_extract_test);
Datum
pssc_extract_test(PG_FUNCTION_ARGS)
{
	bytea	   *q = PG_GETARG_BYTEA_PP(0);
	int			stmt_location = PG_GETARG_INT32(1);
	int			stmt_len = PG_GETARG_INT32(2);
	int			bufsize = PG_GETARG_INT32(3);
	size_t		qlen = VARSIZE_ANY_EXHDR(q);
	char	   *s;
	char	   *buf;
	PsscStmtRange r;
	PsscExtractResult res;
	PsscTagsetStats st = {0, 0, 0};
	extract_fn extract = (extract_fn) main_sym("pssc_extract_tags");
	hash_fn hash = (hash_fn) main_sym("pssc_tagset_hash");
	take_fn take = (take_fn) main_sym("pssc_extract_take_stats");
	int			max_bytes = *(int *) main_sym("pssc_max_tagset_bytes");
	int			max_tags = *(int *) main_sym("pssc_max_tags");
	int			max_value = *(int *) main_sym("pssc_max_tag_value_len");
	int			scan_window = *(int *) main_sym("pssc_scan_window");
	TupleDesc	tupdesc;
	Datum		values[10];
	bool		nulls[10] = {0};
	Datum	   *elems;
	size_t		off;
	int			n;
	const char *prevk = NULL;
	size_t		prevklen = 0;

	if (bufsize < 0)
		elog(ERROR, "bufsize must be >= 0");
	if (stmt_location > (int) qlen || (stmt_location >= 0 && stmt_len > (int) qlen - stmt_location))
		elog(ERROR, "statement out of range");
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	s = palloc(qlen + 1);
	memcpy(s, VARDATA_ANY(q), qlen);
	s[qlen] = '\0';
	r = pssc_stmt_range(s, stmt_location, stmt_len);
	/* pssc_stmt_range uses strlen for "to the end": keep embedded NULs */
	if (stmt_location < 0 || stmt_len <= 0)
		r.end = qlen;
	r.start = pssc_stmt_owned_start(s, 0, r.start, (size_t) scan_window,
									standard_conforming_strings);

	buf = palloc(bufsize + GUARD_BYTES);
	memset(buf, GUARD, bufsize + GUARD_BYTES);

	take(&st);					/* discard earlier counters */
	memset(&st, 0, sizeof(st));
	memset(&res, 0x5A, sizeof(res));
	extract(s, r.start, r.end, buf, (size_t) bufsize, &res);
	take(&st);

	/* invariants */
	for (int i = 0; i < GUARD_BYTES; i++)
		if ((unsigned char) buf[bufsize + i] != GUARD)
			elog(ERROR, "buffer overrun");
	if (res.len > (size_t) Min(bufsize, max_bytes))
		elog(ERROR, "tag set of %zu bytes exceeds limit", res.len);
	if (res.ntags < 0 || res.ntags > max_tags)
		elog(ERROR, "bad ntags %d", res.ntags);
	if (res.oom && (res.len != 0 || res.ntags != 0))
		elog(ERROR, "oom with tags");
	if (res.hash != hash(buf, res.len))
		elog(ERROR, "hash mismatch");

	elems = palloc(sizeof(Datum) * (res.ntags + 1));
	n = 0;
	for (off = 0; off < res.len;)
	{
		const char *k = buf + off;
		size_t		klen = strnlen(k, res.len - off);
		const char *v;
		size_t		vlen;
		char	   *kv;

		if (off + klen >= res.len)
			elog(ERROR, "unterminated key");
		v = k + klen + 1;
		vlen = strnlen(v, res.len - off - klen - 1);
		if (off + klen + 1 + vlen >= res.len)
			elog(ERROR, "unterminated value");
		if (klen == 0 || klen > PSSC_MAX_KEY_LEN)
			elog(ERROR, "bad key length %zu", klen);
		if ((int) vlen > max_value)
			elog(ERROR, "value of %zu bytes exceeds max_tag_value_len", vlen);
		if (!pg_verify_mbstr(GetDatabaseEncoding(), k, (int) klen, true) ||
			!pg_verify_mbstr(GetDatabaseEncoding(), v, (int) vlen, true))
			elog(ERROR, "invalid encoding in tag set");
		if (prevk != NULL && key_cmp(prevk, prevklen, k, klen) >= 0)
			elog(ERROR, "keys not strictly sorted");
		prevk = k;
		prevklen = klen;
		if (n >= res.ntags)
			elog(ERROR, "more tags than ntags");
		kv = psprintf("%s=%s", k, v);
		elems[n++] = PointerGetDatum(cstring_to_text(kv));
		off += klen + vlen + 2;
	}
	if (n != res.ntags)
		elog(ERROR, "ntags %d but %d tags serialized", res.ntags, n);

	values[0] = PointerGetDatum(construct_array(elems, n, TEXTOID, -1, false,
												TYPALIGN_INT));
	{
		bytea	   *b = palloc(VARHDRSZ + res.len);

		SET_VARSIZE(b, VARHDRSZ + res.len);
		memcpy(VARDATA(b), buf, res.len);
		values[1] = PointerGetDatum(b);
	}
	values[2] = Int32GetDatum((int) res.len);
	values[3] = Int64GetDatum((int64) res.hash);
	values[4] = Int32GetDatum(res.ntags);
	values[5] = BoolGetDatum(res.footer);
	values[6] = BoolGetDatum(res.oom);
	values[7] = Int64GetDatum((int64) st.invalid_tags);
	values[8] = Int64GetDatum((int64) st.dropped_tags);
	values[9] = Int64GetDatum((int64) st.heuristic_scans);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_extract_test_hash);
Datum
pssc_extract_test_hash(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	hash_fn		hash = (hash_fn) main_sym("pssc_tagset_hash");

	PG_RETURN_INT64((int64) hash(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)));
}

static int	fake_regex_calls;

static bool
is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void
fake_regex(void *arg, int index, const PsscExtractorList *list,
		   const char *body, size_t len, const PsscPairOut *out,
		   PsscPairResult *result)
{
	const PsscExtractor *e = &list->extractors[index];
	const PsscBlobStr *keys = pssc_extractor_keys(list, e);
	size_t		p = 0;

	Assert(arg == &fake_regex_calls);
	fake_regex_calls++;
	memset(result, 0, sizeof(*result));
	for (uint32 i = 0; i < e->nkeys && result->npairs < out->max_pairs; i++)
	{
		size_t		w;
		PsscPair   *pr;

		while (p < len && is_space(body[p]))
			p++;
		if (p == len)
			break;
		for (w = p; w < len && !is_space(body[w]); w++)
			;
		pr = &out->pairs[result->npairs++];
		pr->key = pssc_blob_str(list, keys[i]);
		pr->keylen = keys[i].len;
		pr->value = body + p;
		pr->valuelen = w - p;
		pr->flags = 0;
		p = w;
	}
}

PG_FUNCTION_INFO_V1(pssc_extract_test_fake_regex);
Datum
pssc_extract_test_fake_regex(PG_FUNCTION_ARGS)
{
	set_regex_fn set = (set_regex_fn) main_sym("pssc_extract_set_regex_hook");

	if (PG_GETARG_BOOL(0))
		set(fake_regex, &fake_regex_calls);
	else
		set(NULL, NULL);
	PG_RETURN_VOID();
}
