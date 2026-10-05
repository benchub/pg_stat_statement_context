/*
 * extract.c
 *		Backend glue for the tag-set pipeline: see extract.h and tagset.h.
 *
 * Scratch memory comes from one long-lived context (a child of
 * TopMemoryContext, created by _PG_init so that the per-statement path
 * cannot fail to create it) that is reset after every call, so the per-statement
 * cost is a few block allocations and one reset, and nothing leaks into the
 * caller's context. Allocations use MCXT_ALLOC_NO_OOM and are refused above
 * MaxAllocSize, so running out of memory yields an empty tag set instead of
 * an ERROR in the executor hooks.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "parser/parser.h"
#include "utils/memutils.h"

#include "extract.h"
#include "guc.h"
#include "regex_runtime.h"

static MemoryContext extract_cxt = NULL;
static PsscTagsetStats pending_stats;
static PsscRegexExtractFn regex_hook = NULL;
static void *regex_hook_arg = NULL;

static void *
env_alloc(void *arg, size_t size)
{
	if (size > MaxAllocSize)
		return NULL;
	return MemoryContextAllocExtended(extract_cxt, Max(size, 1),
									  MCXT_ALLOC_NO_OOM);
}

static bool
env_verify(void *arg, const char *s, size_t len)
{
	if (len > (size_t) PG_INT32_MAX)
		return false;
	return pg_verify_mbstr(GetDatabaseEncoding(), s, (int) len, true);
}

static size_t
env_cliplen(void *arg, const char *s, size_t len, size_t limit)
{
	if (len > (size_t) PG_INT32_MAX)
		len = PG_INT32_MAX;
	return (size_t) pg_mbcliplen(s, (int) len, (int) limit);
}

/* Passes the hook's own argument instead of the env's. */
static void
env_regex(void *arg, int index, const PsscExtractorList *list,
		  const char *body, size_t len, const PsscPairOut *out,
		  PsscPairResult *result)
{
	regex_hook(regex_hook_arg, index, list, body, len, out, result);
}

void
pssc_extract_tags(const char *s, size_t start, size_t end, char *buf,
				  size_t bufsize, PsscExtractResult *result)
{
	PsscTagsetEnv env;
	PsscTagsetLimits limits;
	PsscTagsetOut out;

	memset(result, 0, sizeof(*result));
	if (extract_cxt == NULL)	/* not preloaded: pssc_extract_init() not run */
	{
		result->oom = true;
		result->hash = pssc_tagset_hash(buf, 0);
		return;
	}

	pssc_regex_release_stale();

	env.arg = NULL;
	env.alloc = env_alloc;
	env.verify = env_verify;
	env.cliplen = env_cliplen;
	env.regex = regex_hook != NULL ? env_regex : NULL;

	limits.max_tags = pssc_max_tags;
	limits.max_tag_value_len = pssc_max_tag_value_len;
	limits.max_tagset_bytes = (int) Min((size_t) pssc_max_tagset_bytes, bufsize);
	limits.scan_window = pssc_scan_window;
	limits.standard_conforming_strings = standard_conforming_strings;

	memset(&out, 0, sizeof(out));
	out.buf = buf;
	pssc_tagset_build(s, start, end, pssc_guc_extractors(), pssc_guc_tags(),
					  pssc_guc_exclude_tags(), &limits, &env, &out,
					  &pending_stats);
	MemoryContextReset(extract_cxt);

	result->len = out.len;
	result->ntags = out.ntags;
	result->footer = out.footer;
	result->oom = out.oom;
	result->hash = pssc_tagset_hash(buf, out.len);
}

void
pssc_extract_init(void)
{
	if (extract_cxt == NULL)
		extract_cxt = AllocSetContextCreate(TopMemoryContext,
											"pg_stat_statement_context extract",
											ALLOCSET_DEFAULT_SIZES);
}

uint32
pssc_tagset_hash(const char *buf, size_t len)
{
	return hash_bytes((const unsigned char *) buf, (int) len);
}

void
pssc_extract_take_stats(PsscTagsetStats *stats)
{
	stats->invalid_tags += pending_stats.invalid_tags;
	stats->dropped_tags += pending_stats.dropped_tags;
	stats->heuristic_scans += pending_stats.heuristic_scans;
	stats->regex_compile_failures += pending_stats.regex_compile_failures;
	memset(&pending_stats, 0, sizeof(pending_stats));
}

void
pssc_extract_note_regex_compile_failure(void)
{
	pending_stats.regex_compile_failures++;
}

void
pssc_extract_set_regex_hook(PsscRegexExtractFn fn, void *arg)
{
	regex_hook = fn;
	regex_hook_arg = arg;
}
