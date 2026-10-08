/*
 * extract_fn.c
 *		pg_stat_statement_context_extract(query, stmt_location, stmt_len):
 *		the debug function of DESIGN.md §7 and §9.
 *
 * Runs the tag-set pipeline of the executor hooks (pssc_extract_tags_debug()
 * in src/extract.c, the same scanner, extractor chain, limits and encoding
 * checks) on one statement of a given query string, with the current GUCs,
 * and returns the tags with this call's diagnostics as jsonb. It records
 * nothing in the store and adds nothing to the _info() counters (except a
 * regex compile failure, which is per-backend state; see extract.h). It
 * works whatever "enabled" says: it is a debugging tool. EXECUTE is revoked
 * from PUBLIC by the install script, because it runs the regex engine on
 * arbitrary input and reveals the extractor configuration. The cardinality
 * caps (§6.11 step 8) are only peeked at: a value that would collapse shows
 * as null (counted in capped_tags), but no value is admitted, so calling it
 * does not use up any key's cap.
 *
 * The statement is located as the parser reports it: stmt_location -1
 * means the whole string, stmt_len 0 means to the end; both are in bytes.
 * Its owned leading trivia (DESIGN.md §6.5) is found by the hooks' own
 * policy, pssc_stmt_owned_range() in src/context.c, including the
 * scan_window budget; so for the first statement of a string the result is
 * the hooks'. Only the hooks' per-string boundary cache is missing: a later
 * statement of a multi-statement string that starts more than scan_window
 * bytes in may own leading comments for the hooks but not here.
 *
 * Tags are output as the views output them (pssc_tags_push_jsonb(),
 * src/tagout.c): in a SQL_ASCII database, non-ASCII bytes and '\' of keys
 * and values are escaped (\xHH, \\); tagset_bytes and the counts describe
 * the stored bytes.
 */
#include "postgres.h"

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

#include "compat.h"
#include "context.h"
#include "extract.h"
#include "guc.h"
#include "scan.h"
#include "tagout.h"

PG_FUNCTION_INFO_V1(pg_stat_statement_context_extract);

static void
push_key(JsonbParseState **st, const char *key)
{
	JsonbValue	v;

	v.type = jbvString;
	v.val.string.val = (char *) key;
	v.val.string.len = (int) strlen(key);
	(void) pushJsonbValue(st, WJB_KEY, &v);
}

static void
push_bool(JsonbParseState **st, const char *key, bool b)
{
	JsonbValue	v;

	push_key(st, key);
	v.type = jbvBool;
	v.val.boolean = b;
	(void) pushJsonbValue(st, WJB_VALUE, &v);
}

static void
push_int(JsonbParseState **st, const char *key, int64 n)
{
	JsonbValue	v;

	push_key(st, key);
	v.type = jbvNumeric;
	v.val.numeric = int64_to_numeric(n);
	(void) pushJsonbValue(st, WJB_VALUE, &v);
}

Datum
pg_stat_statement_context_extract(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			stmt_location = PG_GETARG_INT32(1);
	int			stmt_len = PG_GETARG_INT32(2);
	size_t		qlen = strlen(query);
	PsscStmtRange r;
	PsscExtractResult res;
	PsscTagsetStats stats;
	char	   *buf;
	JsonbParseState *st = NULL;
	JsonbValue *result;

	if (!pssc_extract_available())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statement_context must be loaded via \"shared_preload_libraries\"")));
	if (stmt_location < -1 || (stmt_location >= 0 && (size_t) stmt_location > qlen))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("stmt_location %d is out of range", stmt_location),
				 errdetail("It must be -1 (the whole string) or a byte offset from 0 to the query length, %zu.",
						   qlen)));
	if (stmt_len < 0 ||
		(stmt_location >= 0 && (size_t) stmt_len > qlen - (size_t) stmt_location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("stmt_len %d is out of range", stmt_len),
				 errdetail("It must be 0 (to the end of the string) or a byte count that ends within the query.")));

	r = pssc_stmt_owned_range(query, stmt_location, stmt_len);

	buf = palloc(PSSC_TAGSET_BYTES_MAX);
	pssc_extract_tags_debug(query, r.start, r.end, buf, PSSC_TAGSET_BYTES_MAX,
							&res, &stats);

	(void) pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);

	push_key(&st, "tags");
	pssc_tags_push_jsonb(&st, buf, res.len, GetDatabaseEncoding());

	push_int(&st, "ntags", res.ntags);
	push_int(&st, "tagset_bytes", (int64) res.len);
	push_bool(&st, "footer", res.footer);
	push_bool(&st, "heuristic", stats.heuristic_scans > 0);
	push_bool(&st, "oom", res.oom);
	push_int(&st, "stmt_start", (int64) r.start);
	push_int(&st, "stmt_end", (int64) r.end);
	push_int(&st, "invalid_tags", (int64) stats.invalid_tags);
	push_int(&st, "dropped_tags", (int64) stats.dropped_tags);
	push_int(&st, "heuristic_scans", (int64) stats.heuristic_scans);
	push_int(&st, "regex_compile_failures", (int64) stats.regex_compile_failures);
	push_int(&st, "normalized_tags", (int64) stats.normalized_tags);
	push_int(&st, "normalize_failures", (int64) stats.normalize_failures);
	push_int(&st, "capped_tags", (int64) stats.capped_tags);

	result = pushJsonbValue(&st, WJB_END_OBJECT, NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(result));
}
