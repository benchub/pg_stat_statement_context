/*
 * pssc_guc_test.c
 *		TEST-ONLY module that exposes pg_stat_statement_context's parsed
 *		configuration (src/guc.h) to SQL for test/t/003_guc.pl.
 *
 * This is not part of pg_stat_statement_context and is never installed by
 * the top-level "make install". It reaches the main library's exported
 * accessors and variables through load_external_function(), so
 * pg_stat_statement_context must be in shared_preload_libraries.
 */
#include "postgres.h"

/* mallinfo2() arrived in glibc 2.33; features.h came in via postgres.h. */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
#define PSSC_HAVE_MALLINFO2 1
#include <malloc.h>
#endif
#endif

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "guc.h"

PG_MODULE_MAGIC;

#define MAIN_LIB "$libdir/pg_stat_statement_context"

typedef const PsscTagList *(*list_getter_fn) (void);
typedef bool (*match_all_fn) (const PsscTagList *);
typedef int (*count_fn) (const PsscTagList *);
typedef const char *(*key_fn) (const PsscTagList *, int, int *);
typedef int (*find_fn) (const PsscTagList *, const char *, int);
typedef uint64 (*generation_fn) (void);
typedef bool (*blob_size_fn) (size_t, size_t, size_t *);

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
	appendStringInfo(&buf, "untagged=%s", untagged_name(INT_VAR("pssc_untagged")));
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
 * mallinfo2()), NULL elsewhere.
 * GUC "extra" blobs are malloc'd on PG14/15 and live in GUCMemoryContext,
 * itself malloc-backed, on PG16+, so a leak of them shows up here.
 */
PG_FUNCTION_INFO_V1(pssc_guc_test_malloc_used);
Datum
pssc_guc_test_malloc_used(PG_FUNCTION_ARGS)
{
#ifdef PSSC_HAVE_MALLINFO2
	struct mallinfo2 mi = mallinfo2();

	PG_RETURN_INT64((int64) (mi.uordblks + mi.hblkhd));
#else
	PG_RETURN_NULL();
#endif
}
