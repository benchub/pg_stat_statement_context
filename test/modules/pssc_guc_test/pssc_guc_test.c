/*
 * pssc_guc_test.c
 *		TEST-ONLY module that exposes pg_stat_statement_context's parsed
 *		configuration (src/guc.h) to SQL for test/t/003_guc.pl, and its
 *		shared_preload_libraries load-order matcher (src/utility.h) for
 *		test/t/014_load_order.pl.
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It reaches the main library's exported
 * accessors and variables through load_external_function(), so
 * pg_stat_statement_context must be in shared_preload_libraries.
 */
#include "postgres.h"

/*
 * Sources of this backend's malloc'd bytes in use: mallinfo2() (arrived in
 * glibc 2.33; features.h came in via postgres.h) or, on macOS,
 * malloc_zone_statistics() over all zones.
 */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
#define PSSC_HAVE_MALLINFO2 1
#include <malloc.h>
#endif
#elif defined(__APPLE__)
#define PSSC_HAVE_MALLOC_ZONE 1
#include <malloc/malloc.h>
#endif

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "guc.h"
#include "scan.h"
#include "utility.h"

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"

typedef const PsscTagList *(*list_getter_fn) (void);
typedef bool (*match_all_fn) (const PsscTagList *);
typedef int (*count_fn) (const PsscTagList *);
typedef const char *(*key_fn) (const PsscTagList *, int, int *);
typedef int (*find_fn) (const PsscTagList *, const char *, int);
typedef uint64 (*generation_fn) (void);
typedef bool (*blob_size_fn) (size_t, size_t, size_t *);
typedef const PsscExtractorList *(*extractors_fn) (void);

static void *
main_sym(const char *name)
{
	return (void *) load_external_function(MAIN_LIB, name, true, NULL);
}

static const PsscTagList *
get_list(text *which)
{
	char	   *w = text_to_cstring(which);
	list_getter_fn fn;

	if (strcmp(w, "tags") == 0)
		fn = (list_getter_fn) main_sym("pssc_guc_tags");
	else if (strcmp(w, "exclude_tags") == 0)
		fn = (list_getter_fn) main_sym("pssc_guc_exclude_tags");
	else
		elog(ERROR, "unknown list \"%s\"", w);
	return fn();
}

PG_FUNCTION_INFO_V1(pssc_guc_test_list);
Datum
pssc_guc_test_list(PG_FUNCTION_ARGS)
{
	const PsscTagList *list = get_list(PG_GETARG_TEXT_PP(0));
	match_all_fn match_all = (match_all_fn) main_sym("pssc_tag_list_match_all");
	count_fn count = (count_fn) main_sym("pssc_tag_list_count");
	key_fn key = (key_fn) main_sym("pssc_tag_list_key");
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	int			n;
	Datum	   *elems;

	if (list == NULL)
		elog(ERROR, "list is NULL");
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	n = count(list);
	elems = palloc(sizeof(Datum) * (n > 0 ? n : 1));
	for (int i = 0; i < n; i++)
	{
		int			len = -1;
		const char *k = key(list, i, &len);

		/* The reported length must match the NUL-terminated string. */
		if (len != (int) strlen(k))
			elog(ERROR, "key %d: len %d but strlen %d", i, len, (int) strlen(k));
		elems[i] = PointerGetDatum(cstring_to_text_with_len(k, len));
	}

	values[0] = BoolGetDatum(match_all(list));
	values[1] = Int32GetDatum(n);
	values[2] = PointerGetDatum(construct_array(elems, n, TEXTOID, -1, false,
												TYPALIGN_INT));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(pssc_guc_test_find);
Datum
pssc_guc_test_find(PG_FUNCTION_ARGS)
{
	const PsscTagList *list = get_list(PG_GETARG_TEXT_PP(0));
	text	   *key = PG_GETARG_TEXT_PP(1);
	find_fn find = (find_fn) main_sym("pssc_tag_list_find");

	PG_RETURN_INT32(find(list, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key)));
}

PG_FUNCTION_INFO_V1(pssc_guc_test_generation);
Datum
pssc_guc_test_generation(PG_FUNCTION_ARGS)
{
	generation_fn gen = (generation_fn) main_sym("pssc_guc_config_generation");

	PG_RETURN_INT64((int64) gen());
}

static const char *
track_name(int v)
{
	switch (v)
	{
		case PSSC_TRACK_NONE:
			return "none";
		case PSSC_TRACK_TOP:
			return "top";
		case PSSC_TRACK_ALL:
			return "all";
	}
	return "?";
}

static const char *
nested_name(int v)
{
	switch (v)
	{
		case PSSC_NESTED_INHERIT:
			return "inherit";
		case PSSC_NESTED_SCAN:
			return "scan";
		case PSSC_NESTED_NONE:
			return "none";
	}
	return "?";
}

static const char *
untagged_name(int v)
{
	switch (v)
	{
		case PSSC_UNTAGGED_SKIP:
			return "skip";
		case PSSC_UNTAGGED_RECORD:
			return "record";
	}
	return "?";
}

#define BOOL_VAR(n) (*(bool *) main_sym(n) ? "on" : "off")
#define INT_VAR(n) (*(int *) main_sym(n))
#define STR_VAR(n) (*(char **) main_sym(n))

PG_FUNCTION_INFO_V1(pssc_guc_test_vars);
Datum
pssc_guc_test_vars(PG_FUNCTION_ARGS)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "enabled=%s\n", BOOL_VAR("pssc_enabled"));
	appendStringInfo(&buf, "track=%s\n", track_name(INT_VAR("pssc_track")));
	appendStringInfo(&buf, "track_utility=%s\n", BOOL_VAR("pssc_track_utility"));
	appendStringInfo(&buf, "nested_tags=%s\n", nested_name(INT_VAR("pssc_nested_tags")));
	appendStringInfo(&buf, "max_entries=%d\n", INT_VAR("pssc_max_entries"));
	appendStringInfo(&buf, "bucket_count=%d\n", INT_VAR("pssc_bucket_count"));
	appendStringInfo(&buf, "bucket_interval=%d\n", INT_VAR("pssc_bucket_interval"));
	appendStringInfo(&buf, "max_tags=%d\n", INT_VAR("pssc_max_tags"));
	appendStringInfo(&buf, "max_tag_value_len=%d\n", INT_VAR("pssc_max_tag_value_len"));
	appendStringInfo(&buf, "max_tagset_bytes=%d\n", INT_VAR("pssc_max_tagset_bytes"));
	appendStringInfo(&buf, "scan_window=%d\n", INT_VAR("pssc_scan_window"));
	appendStringInfo(&buf, "extractors=%s\n", STR_VAR("pssc_extractors"));
	appendStringInfo(&buf, "tags=%s\n", STR_VAR("pssc_tags"));
	appendStringInfo(&buf, "exclude_tags=%s\n", STR_VAR("pssc_exclude_tags"));
	appendStringInfo(&buf, "untagged=%s\n", untagged_name(INT_VAR("pssc_untagged")));
	appendStringInfo(&buf, "normalize=%s\n", STR_VAR("pssc_normalize"));
	appendStringInfo(&buf, "tags_override=%s", STR_VAR("pssc_tags_override"));
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * A bigint argument as size_t: -1 is SIZE_MAX; other negative values are an
 * error. Returns false for values above SIZE_MAX (possible where size_t is
 * 32 bits), which must not be truncated into small, valid sizes.
 */
static bool
arg_to_size(int64 v, size_t *out)
{
	if (v == -1)
	{
		*out = SIZE_MAX;
		return true;
	}
	if (v < 0)
		elog(ERROR, "-1 is the only negative argument allowed (got " INT64_FORMAT ")", v);
	if ((uint64) v > (uint64) SIZE_MAX)
		return false;
	*out = (size_t) v;
	return true;
}

/*
 * pssc_tag_list_blob_size() for arbitrary inputs; NULL when rejected,
 * including arguments that do not fit in size_t.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_blob_size);
Datum
pssc_guc_test_blob_size(PG_FUNCTION_ARGS)
{
	blob_size_fn fn = (blob_size_fn) main_sym("pssc_tag_list_blob_size");
	size_t		nkeys;
	size_t		keybytes;
	size_t		size = 0;

	if (!arg_to_size(PG_GETARG_INT64(0), &nkeys) ||
		!arg_to_size(PG_GETARG_INT64(1), &keybytes) ||
		!fn(nkeys, keybytes, &size))
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) size);
}

/*
 * "major.minor" of the glibc this module was compiled against, NULL when not
 * compiled against glibc. The malloc probe below exists exactly from 2.33 on.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_glibc_version);
Datum
pssc_guc_test_glibc_version(PG_FUNCTION_ARGS)
{
#ifdef __GLIBC__
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d.%d", __GLIBC__, __GLIBC_MINOR__)));
#else
	PG_RETURN_NULL();
#endif
}

/*
 * Bytes of malloc'd memory in use in this backend (glibc >= 2.33, which has
 * mallinfo2(), and macOS), NULL elsewhere.
 * GUC "extra" blobs are malloc'd on PG14/15 and live in GUCMemoryContext,
 * itself malloc-backed, on PG16+, so a leak of them shows up here.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_malloc_used);
Datum
pssc_guc_test_malloc_used(PG_FUNCTION_ARGS)
{
#if defined(PSSC_HAVE_MALLINFO2)
	struct mallinfo2 mi = mallinfo2();

	PG_RETURN_INT64((int64) (mi.uordblks + mi.hblkhd));
#elif defined(PSSC_HAVE_MALLOC_ZONE)
	malloc_statistics_t st;

	malloc_zone_statistics(NULL, &st);
	PG_RETURN_INT64((int64) st.size_in_use);
#else
	PG_RETURN_NULL();
#endif
}

/* ---------------- parsed extractors ---------------- */

/* Check that s lies inside the blob and is NUL-terminated there. */
static const char *
blob_str(const PsscExtractorList *list, PsscBlobStr s, const char *what)
{
	if (s.len == 0)
		elog(ERROR, "%s: empty string in blob", what);
	if ((uint64) s.off + s.len + 1 > list->size || s.off < offsetof(PsscExtractorList, extractors))
		elog(ERROR, "%s: string [%u, +%u] outside blob of %u bytes", what, s.off, s.len, list->size);
	if (pssc_blob_str(list, s)[s.len] != '\0' || strlen(pssc_blob_str(list, s)) != s.len)
		elog(ERROR, "%s: string not NUL-terminated at its length", what);
	return pssc_blob_str(list, s);
}

static void
check_array(const PsscExtractorList *list, uint32 off, uint32 n, size_t elem, const char *what)
{
	if (n > 0 && ((uint64) off + (uint64) n * elem > list->size || off % sizeof(uint32) != 0))
		elog(ERROR, "%s: array [%u, %u x %zu] outside blob or misaligned", what, off, n, elem);
}

static void
append_quoted(StringInfo buf, const char *s)
{
	appendStringInfoChar(buf, '\'');
	for (; *s; s++)
	{
		if (*s == '\'')
			appendStringInfoChar(buf, '\'');
		appendStringInfoChar(buf, *s);
	}
	appendStringInfoChar(buf, '\'');
}

static const char *
position_name(int p)
{
	switch (p)
	{
		case PSSC_POS_ANY:
			return "any";
		case PSSC_POS_APPEND:
			return "append";
		case PSSC_POS_PREPEND:
			return "prepend";
	}
	elog(ERROR, "bad position %d", p);
	return NULL;
}

/*
 * Canonical text of a parsed extractor list, one extractor per line, with
 * every field the parser filled in. Validates the blob's internal
 * consistency (every offset inside the blob, strings NUL-terminated, fields
 * only set for the kinds that use them) and errors out otherwise.
 */
static char *
render_extractors(const PsscExtractorList *list)
{
	StringInfoData buf;

	initStringInfo(&buf);
	if (list->size < offsetof(PsscExtractorList, extractors) +
		(uint64) list->nextractors * sizeof(PsscExtractor))
		elog(ERROR, "blob of %u bytes too small for %u extractors", list->size, list->nextractors);
	for (uint32 i = 0; i < list->nextractors; i++)
	{
		const PsscExtractor *e = &list->extractors[i];
		const PsscBlobStr *keys = pssc_extractor_keys(list, e);
		const PsscBlobRename *ren = pssc_extractor_renames(list, e);

		if (i > 0)
			appendStringInfoChar(&buf, '\n');
		{
			const char *kname;

			switch (e->kind)
			{
				case PSSC_EXTRACTOR_SQLCOMMENTER:
					kname = "sqlcommenter";
					break;
				case PSSC_EXTRACTOR_MARGINALIA:
					kname = "marginalia";
					break;
				case PSSC_EXTRACTOR_REGEX:
					kname = "regex";
					break;
				default:
					elog(ERROR, "extractor %u: bad kind %d", i, e->kind);
			}
			if (e->source == PSSC_SOURCE_COMMENT)
				appendStringInfo(&buf, "%s(position=%s, merge=%s", kname,
								 position_name(e->position), e->merge ? "on" : "off");
			else if (e->source == PSSC_SOURCE_APPNAME)
			{
				/* appname takes no position; the blob holds any */
				if (e->position != PSSC_POS_ANY)
					elog(ERROR, "extractor %u: appname with position %d", i, e->position);
				appendStringInfo(&buf, "appname(format=%s, merge=%s", kname,
								 e->merge ? "on" : "off");
			}
			else
				elog(ERROR, "extractor %u: bad source %d", i, e->source);
		}
		if (e->kind == PSSC_EXTRACTOR_SQLCOMMENTER)
			appendStringInfo(&buf, ", url_decode=%s", e->url_decode ? "on" : "off");
		else if (e->url_decode)
			elog(ERROR, "extractor %u: url_decode set on a non-sqlcommenter extractor", i);
		if (e->kind == PSSC_EXTRACTOR_MARGINALIA)
		{
			appendStringInfoString(&buf, ", kv_sep=");
			append_quoted(&buf, blob_str(list, e->kv_sep, "kv_sep"));
			appendStringInfoString(&buf, ", pair_sep=");
			append_quoted(&buf, blob_str(list, e->pair_sep, "pair_sep"));
		}
		else if (e->kv_sep.len != 0 || e->pair_sep.len != 0)
			elog(ERROR, "extractor %u: separators set on a non-marginalia extractor", i);
		if (e->kind == PSSC_EXTRACTOR_REGEX)
		{
			appendStringInfoString(&buf, ", pattern=");
			append_quoted(&buf, blob_str(list, e->pattern, "pattern"));
			if (!e->has_keys || e->nkeys == 0)
				elog(ERROR, "extractor %u: regex without keys", i);
		}
		else if (e->pattern.len != 0)
			elog(ERROR, "extractor %u: pattern set on a non-regex extractor", i);

		if (!e->has_keys && e->nkeys != 0)
			elog(ERROR, "extractor %u: keys without has_keys", i);
		check_array(list, e->keys_off, e->nkeys, sizeof(PsscBlobStr), "keys");
		check_array(list, e->rename_off, e->nrename, sizeof(PsscBlobRename), "rename");
		if (e->has_keys)
		{
			appendStringInfoString(&buf, ", keys=");
			for (uint32 k = 0; k < e->nkeys; k++)
				appendStringInfo(&buf, "%s%s", k ? "|" : "", blob_str(list, keys[k], "key"));
		}
		if (e->nrename > 0)
		{
			appendStringInfoString(&buf, ", rename=");
			for (uint32 k = 0; k < e->nrename; k++)
				appendStringInfo(&buf, "%s%s:%s", k ? "|" : "",
								 blob_str(list, ren[k].from, "rename from"),
								 blob_str(list, ren[k].to, "rename to"));
		}
		appendStringInfoChar(&buf, ')');
	}
	return buf.data;
}

/*
 * The parsed extractors of this backend, rendered by render_extractors().
 * With relocate, the blob is first copied (by its recorded size only) to a
 * fresh buffer, the copy is rendered, and every aligned pointer-sized word
 * of the blob is checked not to point into the original: a blob that used
 * pointers instead of offsets would fail one of these checks.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_extractors);
Datum
pssc_guc_test_extractors(PG_FUNCTION_ARGS)
{
	extractors_fn fn = (extractors_fn) main_sym("pssc_guc_extractors");
	const PsscExtractorList *list = fn();
	bool		relocate = PG_GETARG_BOOL(0);

	if (list == NULL)
		elog(ERROR, "parsed extractors blob is NULL");
	if (relocate)
	{
		uintptr_t	lo = (uintptr_t) list;
		uintptr_t	hi = lo + list->size;
		char	   *copy = palloc0(list->size + sizeof(uintptr_t));
		char	   *result;

		memcpy(copy, list, list->size);
		for (uint32 o = 0; o + sizeof(uintptr_t) <= list->size; o += sizeof(uint32))
		{
			uintptr_t	w;

			memcpy(&w, copy + o, sizeof(w));
			if (w >= lo && w < hi)
				elog(ERROR, "blob word at offset %u points into the blob", o);
		}
		result = render_extractors((const PsscExtractorList *) copy);
		/* The original must render the same. */
		if (strcmp(result, render_extractors(list)) != 0)
			elog(ERROR, "relocated blob renders differently");
		PG_RETURN_TEXT_P(cstring_to_text(result));
	}
	PG_RETURN_TEXT_P(cstring_to_text(render_extractors(list)));
}

/* Size in bytes of this backend's parsed extractors blob. */
PG_FUNCTION_INFO_V1(pssc_guc_test_extractors_size);
Datum
pssc_guc_test_extractors_size(PG_FUNCTION_ARGS)
{
	extractors_fn fn = (extractors_fn) main_sym("pssc_guc_extractors");
	const PsscExtractorList *list = fn();

	if (list == NULL)
		elog(ERROR, "parsed extractors blob is NULL");
	PG_RETURN_INT64((int64) list->size);
}

/*
 * The libraries pg_stat_statement_context would warn about as loaded after
 * it, for the given shared_preload_libraries value (src/utility.c), joined
 * by ',' in the order pg_stat_statements, pg_stat_monitor; '' when the order
 * is right. Spellings the server cannot load here (e.g. other letter case)
 * can be checked this way.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_load_order_wrong);
Datum
pssc_guc_test_load_order_wrong(PG_FUNCTION_ARGS)
{
	bool		(*fn) (const char *) =
		(bool (*) (const char *)) main_sym("pssc_load_order_wrong");

	PG_RETURN_TEXT_P(cstring_to_text(fn(text_to_cstring(PG_GETARG_TEXT_PP(0))) ?
									 "pg_stat_statements" : ""));
}
