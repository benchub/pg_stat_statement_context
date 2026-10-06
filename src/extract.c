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
 *
 * The appname extractors' result (DESIGN.md §4.2) is cached per backend:
 * application_name rarely changes, so a statement usually costs one
 * comparison of the string and the configuration instead of a parse. The
 * cache holds a deep copy of the tags plus the counters of the parse, which
 * pssc_tagset_build() adds again on every use, so the counters are as if
 * nothing were cached. A result that depends on more than its inputs (out
 * of memory, a normalize or regex failure that may not recur) is not cached.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "parser/parser.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "extract.h"
#include "guc.h"
#include "regex_runtime.h"

static MemoryContext extract_cxt = NULL;
static PsscTagsetStats pending_stats;
static PsscRegexExtractFn regex_hook = NULL;
static void *regex_hook_arg = NULL;

/*
 * Cached appname result; valid only if appname_valid. Its inputs are the
 * configuration (generation), the limits, application_name, the database
 * encoding (fixed per backend) and the regex hook (pssc_extract_set_regex_hook()
 * invalidates the cache); results that also depend on transient conditions
 * are not cached.
 */
static MemoryContext appname_cxt = NULL;
static bool appname_valid = false;
static uint64 appname_generation;
static PsscTagsetLimits appname_limits;
static char *appname_str;
static size_t appname_len;
static PsscAppnameTags appname_tags;

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

static PsscNormalizeResult
env_normalize(void *arg, const char *key, size_t klen, const char *val,
			  size_t vlen, size_t limit, const char **out, size_t *outlen)
{
	return pssc_regex_normalize((const PsscNormalizeList *) arg, key, klen,
								val, vlen, limit, env_alloc, NULL, out, outlen);
}

static bool
same_limits(const PsscTagsetLimits *a, const PsscTagsetLimits *b)
{
	return a->max_tags == b->max_tags &&
		a->max_tag_value_len == b->max_tag_value_len &&
		a->max_tagset_bytes == b->max_tagset_bytes &&
		a->scan_window == b->scan_window &&
		a->standard_conforming_strings == b->standard_conforming_strings;
}

static char *
cache_copy(const char *s, size_t len)
{
	char	   *p = MemoryContextAllocExtended(appname_cxt, Max(len, 1),
											   MCXT_ALLOC_NO_OOM);

	if (p != NULL && len > 0)
		memcpy(p, s, len);
	return p;
}

/* Stores built (allocated in extract_cxt) as the cached result for name. */
static void
appname_cache_store(const char *name, size_t len, uint64 generation,
					const PsscTagsetLimits *limits, const PsscAppnameTags *built)
{
	PsscTagCandidate *tags = NULL;

	MemoryContextReset(appname_cxt);
	appname_valid = false;
	appname_str = cache_copy(name, len);
	if (appname_str == NULL)
		return;
	if (built->ntags > 0)
	{
		if (built->ntags > MaxAllocSize / sizeof(PsscTagCandidate))
			return;
		tags = MemoryContextAllocExtended(appname_cxt,
										  sizeof(PsscTagCandidate) * built->ntags,
										  MCXT_ALLOC_NO_OOM);
		if (tags == NULL)
			return;
		for (size_t i = 0; i < built->ntags; i++)
		{
			tags[i] = built->tags[i];
			tags[i].key = cache_copy(built->tags[i].key, built->tags[i].klen);
			tags[i].val = cache_copy(built->tags[i].val, built->tags[i].vlen);
			if (tags[i].key == NULL || tags[i].val == NULL)
				return;
		}
	}
	appname_len = len;
	appname_generation = generation;
	appname_limits = *limits;
	appname_tags = *built;
	appname_tags.tags = tags;
	appname_valid = true;
}

/*
 * Returns the appname extractors' tags for the current application_name,
 * from the cache or built into extract_cxt (valid until it is reset).
 */
static const PsscAppnameTags *
appname_tags_get(const PsscTagsetLimits *limits, const PsscTagsetEnv *env,
				 PsscAppnameTags *built)
{
	const PsscExtractorList *extractors = pssc_guc_extractors();
	const char *name = application_name != NULL ? application_name : "";
	size_t		len = strlen(name);
	uint64		generation = pssc_guc_config_generation();
	uint64		transient;

	if (!pssc_extractors_have_appname(extractors))
		return NULL;
	if (appname_valid && appname_generation == generation &&
		same_limits(&appname_limits, limits) &&
		appname_len == len && memcmp(appname_str, name, len) == 0)
		return &appname_tags;

	transient = pssc_regex_transient_failures();
	pssc_appname_tags_build(name, len, extractors, pssc_guc_tags(),
							pssc_guc_exclude_tags(), limits, env, built);
	if (!built->oom && built->stats.normalize_failures == 0 &&
		pssc_regex_transient_failures() == transient &&
		pssc_guc_config_generation() == generation)
		appname_cache_store(name, len, generation, limits, built);
	else
		appname_valid = false;
	return built;
}

/* pssc_extract_tags() with the pipeline counters added to *stats. */
static void
extract_tags(const char *s, size_t start, size_t end, char *buf,
			 size_t bufsize, PsscExtractResult *result,
			 PsscTagsetStats *stats)
{
	PsscTagsetEnv env;
	PsscTagsetLimits limits;
	PsscTagsetOut out;
	PsscAppnameTags built;
	const PsscAppnameTags *appname;

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
	env.normalize = NULL;
	if (pssc_guc_normalize()->nrules > 0)
	{
		/* env_normalize() is the only callback that uses arg */
		env.arg = (void *) pssc_guc_normalize();
		env.normalize = env_normalize;
	}

	limits.max_tags = pssc_max_tags;
	limits.max_tag_value_len = pssc_max_tag_value_len;
	limits.max_tagset_bytes = (int) Min((size_t) pssc_max_tagset_bytes, bufsize);
	limits.scan_window = pssc_scan_window;
	limits.standard_conforming_strings = standard_conforming_strings;

	appname = appname_tags_get(&limits, &env, &built);

	memset(&out, 0, sizeof(out));
	out.buf = buf;
	pssc_tagset_build(s, start, end, pssc_guc_extractors(), pssc_guc_tags(),
					  pssc_guc_exclude_tags(), &limits, &env, appname, &out,
					  stats);
	MemoryContextReset(extract_cxt);

	result->len = out.len;
	result->ntags = out.ntags;
	result->footer = out.footer;
	result->oom = out.oom;
	result->hash = pssc_tagset_hash(buf, out.len);
}

void
pssc_extract_tags(const char *s, size_t start, size_t end, char *buf,
				  size_t bufsize, PsscExtractResult *result)
{
	extract_tags(s, start, end, buf, bufsize, result, &pending_stats);
}

void
pssc_extract_tags_debug(const char *s, size_t start, size_t end, char *buf,
						size_t bufsize, PsscExtractResult *result,
						PsscTagsetStats *stats)
{
	uint64		compile_failures = pending_stats.regex_compile_failures;

	memset(stats, 0, sizeof(*stats));
	extract_tags(s, start, end, buf, bufsize, result, stats);
	stats->regex_compile_failures =
		pending_stats.regex_compile_failures - compile_failures;
}

bool
pssc_extract_available(void)
{
	return extract_cxt != NULL;
}

void
pssc_extract_init(void)
{
	if (extract_cxt == NULL)
		extract_cxt = AllocSetContextCreate(TopMemoryContext,
											"pg_stat_statement_context extract",
											ALLOCSET_DEFAULT_SIZES);
	if (appname_cxt == NULL)
		appname_cxt = AllocSetContextCreate(TopMemoryContext,
											"pg_stat_statement_context appname",
											ALLOCSET_SMALL_SIZES);
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
	if (fn != regex_hook || arg != regex_hook_arg)
		appname_valid = false;
	regex_hook = fn;
	regex_hook_arg = arg;
}
