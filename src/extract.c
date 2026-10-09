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
 *
 * The tags_override result (DESIGN.md §8) is cached the same way, keyed by the parsed setting (the check_hook's blob): a
 * statement without an override costs one pointer test, one with an
 * unchanged override a generation comparison, and one whose override was
 * set again to the same value (SET LOCAL in every transaction) a memcmp of
 * the blob.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "cardcap.h"
#include "extract.h"
#include "guc.h"
#include "regex_runtime.h"
#include "store.h"

static MemoryContext extract_cxt = NULL;
static PsscTagsetStats pending_stats;
static PsscRegexExtractFn regex_hook = NULL;
static void *regex_hook_arg = NULL;

/*
 * A cached source result (appname or tags_override); valid only if valid.
 * Its inputs are the configuration (generation), the limits, the source
 * text (application_name, or the bytes of the override blob), the database
 * encoding (fixed per backend) and the regex hook
 * (pssc_extract_set_regex_hook() invalidates the caches); results that also
 * depend on transient conditions are not cached.
 */
typedef struct SourceCache
{
	MemoryContext cxt;
	bool		valid;
	uint64		generation;
	PsscTagsetLimits limits;
	char	   *str;
	size_t		len;
	PsscSourceTags tags;
	uint64		src_generation; /* tags_override: pssc_guc_override_generation() */
} SourceCache;

static SourceCache appname_cache;
static SourceCache override_cache;

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
		a->standard_conforming_strings == b->standard_conforming_strings &&
		a->exemplar_value_len == b->exemplar_value_len &&
		a->capture_exemplars == b->capture_exemplars;
}

static char *
cache_copy(SourceCache *cache, const char *s, size_t len)
{
	char	   *p;

	if (len > MaxAllocSize)
		return NULL;
	p = MemoryContextAllocExtended(cache->cxt, Max(len, 1), MCXT_ALLOC_NO_OOM);
	if (p != NULL && len > 0)
		memcpy(p, s, len);
	return p;
}

/* True if cache holds the result for these inputs. */
static bool
cache_hit(const SourceCache *cache, const char *str, size_t len,
		  uint64 generation, const PsscTagsetLimits *limits)
{
	return cache->valid && cache->generation == generation &&
		same_limits(&cache->limits, limits) &&
		cache->len == len && memcmp(cache->str, str, len) == 0;
}

/* Stores built (allocated in extract_cxt) as the cached result for str. */
static void
cache_store(SourceCache *cache, const char *str, size_t len, uint64 generation,
			const PsscTagsetLimits *limits, const PsscSourceTags *built)
{
	PsscTagCandidate *tags = NULL;

	MemoryContextReset(cache->cxt);
	cache->valid = false;
	cache->str = cache_copy(cache, str, len);
	if (cache->str == NULL)
		return;
	if (built->ntags > 0)
	{
		if (built->ntags > MaxAllocSize / sizeof(PsscTagCandidate))
			return;
		tags = MemoryContextAllocExtended(cache->cxt,
										  sizeof(PsscTagCandidate) * built->ntags,
										  MCXT_ALLOC_NO_OOM);
		if (tags == NULL)
			return;
		for (size_t i = 0; i < built->ntags; i++)
		{
			tags[i] = built->tags[i];
			tags[i].key = cache_copy(cache, built->tags[i].key, built->tags[i].klen);
			tags[i].val = cache_copy(cache, built->tags[i].val, built->tags[i].vlen);
			if (tags[i].key == NULL || tags[i].val == NULL)
				return;
		}
	}
	cache->len = len;
	cache->generation = generation;
	cache->limits = *limits;
	cache->tags = *built;
	cache->tags.tags = tags;
	for (int i = 0; i < PSSC_MAX_EXEMPLAR_KEYS; i++)
	{
		if (!built->exemplars[i].set)
			continue;
		cache->tags.exemplars[i].val = cache_copy(cache, built->exemplars[i].val,
												  built->exemplars[i].vlen);
		if (cache->tags.exemplars[i].val == NULL)
			return;
	}
	cache->valid = true;
}

/* True if built may be cached: it depends on its inputs only. */
static bool
cacheable(const PsscSourceTags *built, uint64 transient, uint64 generation)
{
	return !built->oom && built->stats.normalize_failures == 0 &&
		pssc_regex_transient_failures() == transient &&
		pssc_guc_config_generation() == generation;
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
	if (cache_hit(&appname_cache, name, len, generation, limits))
		return &appname_cache.tags;

	transient = pssc_regex_transient_failures();
	pssc_appname_tags_build(name, len, extractors, pssc_guc_tags(),
							pssc_guc_exclude_tags(), limits, env, built);
	if (cacheable(built, transient, generation))
		cache_store(&appname_cache, name, len, generation, limits, built);
	else
		appname_cache.valid = false;
	return built;
}

/*
 * Returns the tags of the current tags_override (NULL if it has no pair),
 * from the cache or built into extract_cxt (valid until it is reset).
 */
static const PsscSourceTags *
override_tags_get(const PsscTagsetLimits *limits, const PsscTagsetEnv *env,
				  PsscSourceTags *built)
{
	const PsscOverrideList *ov = pssc_guc_override();
	uint64		ovgen;
	uint64		generation;
	uint64		transient;
	PsscPair   *pairs;

	if (ov == NULL)
		return NULL;
	ovgen = pssc_guc_override_generation();
	generation = pssc_guc_config_generation();
	if (override_cache.valid && override_cache.src_generation == ovgen &&
		override_cache.generation == generation &&
		same_limits(&override_cache.limits, limits))
		return &override_cache.tags;
	if (cache_hit(&override_cache, (const char *) ov, ov->size, generation, limits))
	{
		override_cache.src_generation = ovgen;
		return &override_cache.tags;
	}

	pairs = env_alloc(NULL, (size_t) ov->npairs * sizeof(PsscPair));
	if (pairs == NULL)
	{
		memset(built, 0, sizeof(*built));
		built->oom = true;
		override_cache.valid = false;
		return built;
	}
	for (uint32 i = 0; i < ov->npairs; i++)
	{
		pairs[i].key = pssc_override_str(ov, ov->pairs[i].key);
		pairs[i].keylen = ov->pairs[i].key.len;
		pairs[i].value = pssc_override_str(ov, ov->pairs[i].value);
		pairs[i].valuelen = ov->pairs[i].value.len;
		pairs[i].flags = 0;
	}
	transient = pssc_regex_transient_failures();
	pssc_override_tags_build(pairs, ov->npairs, pssc_guc_extractors(),
							 pssc_guc_tags(), pssc_guc_exclude_tags(), limits,
							 env, built);
	if (cacheable(built, transient, generation))
	{
		cache_store(&override_cache, (const char *) ov, ov->size, generation,
					limits, built);
		override_cache.src_generation = ovgen;
	}
	else
		override_cache.valid = false;
	return built;
}

/*
 * Serializes the captured exemplars (§6.13) into ex: per value, a uint8
 * slot, a native uint16 length and the bytes. Returns the length.
 */
static size_t
exemplars_serialize(const PsscExemplar *exemplars, char *ex, size_t exsize)
{
	size_t		pos = 0;

	for (int i = 0; i < PSSC_MAX_EXEMPLAR_KEYS; i++)
	{
		uint16		len;

		if (!exemplars[i].set || exemplars[i].vlen > PSSC_EXEMPLAR_VALUE_MAX)
			continue;
		if (pos + 1 + sizeof(uint16) + exemplars[i].vlen > exsize)
			break;
		len = (uint16) exemplars[i].vlen;
		ex[pos] = (char) i;
		memcpy(ex + pos + 1, &len, sizeof(uint16));
		if (len > 0)
			memcpy(ex + pos + 1 + sizeof(uint16), exemplars[i].val, len);
		pos += 1 + sizeof(uint16) + len;
	}
	return pos;
}

/*
 * Copies out->cands to cands (candsize bytes). They always fit a buffer of
 * PSSC_TAGSET_CANDS_MAX(max_tagset_bytes); if not, the result is emptied
 * as on oom, so no tag set escapes a later recap.
 */
static void
cands_copy(PsscTagsetOut *out, char *cands, size_t candsize,
		   size_t *cands_len)
{
	if (out->cands_len > candsize)
	{
		Assert(false);
		out->len = 0;
		out->ntags = 0;
		out->oom = true;
		return;
	}
	memcpy(cands, out->cands, out->cands_len);
	*cands_len = out->cands_len;
}

/* pssc_extract_tags_ex() with the pipeline counters added to *stats. */
static void
extract_tags(const char *s, size_t start, size_t end, char *buf,
			 size_t bufsize, PsscExtractResult *result,
			 PsscTagsetStats *stats, bool peek_caps,
			 char *ex, size_t exsize, size_t *exlen,
			 char *cands, size_t candsize, size_t *cands_len)
{
	const PsscTagList *exemplar_keys = pssc_guc_exemplar_keys();

	PsscTagsetEnv env;
	PsscTagsetLimits limits;
	PsscTagsetOut out;
	PsscAppnameTags built;
	const PsscAppnameTags *appname;
	PsscSourceTags obuilt;
	const PsscSourceTags *override;

	memset(result, 0, sizeof(*result));
	if (exlen != NULL)
		*exlen = 0;
	if (cands_len != NULL)
		*cands_len = 0;
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
	/*
	 * caps apply after the cached appname/override passes (steps 1-7), in
	 * the scope of the role and database the frame records (context.c)
	 */
	env.cap = pssc_cap_hook(peek_caps, GetUserId(), MyDatabaseId);
	/* only when the store has exemplar slots (and the caller wants them) */
	env.exemplar_keys = NULL;
	limits.exemplar_value_len = 0;
	limits.capture_exemplars = false;
	if (ex != NULL && pssc_tag_list_count(exemplar_keys) > 0)
	{
		env.exemplar_keys = exemplar_keys;
		limits.exemplar_value_len = pssc_store_exemplar_value_len();
		limits.capture_exemplars = true;
	}

	limits.max_tags = pssc_max_tags;
	limits.max_tag_value_len = pssc_max_tag_value_len;
	limits.max_tagset_bytes = (int) Min((size_t) pssc_max_tagset_bytes, bufsize);
	limits.scan_window = pssc_scan_window;
	limits.standard_conforming_strings = standard_conforming_strings;

	appname = appname_tags_get(&limits, &env, &built);
	override = override_tags_get(&limits, &env, &obuilt);

	memset(&out, 0, sizeof(out));
	out.buf = buf;
	/* only role-scoped caps depend on the identity (DESIGN.md §6.1) */
	out.want_cands = cands != NULL && env.cap != NULL &&
		pssc_cardinality_cap_scope == PSSC_CAP_SCOPE_ROLE;
	pssc_tagset_build_with_override(s, start, end, pssc_guc_extractors(),
									pssc_guc_tags(), pssc_guc_exclude_tags(),
									&limits, &env, appname, override, &out,
									stats);
	if (ex != NULL && !out.oom)
		*exlen = exemplars_serialize(out.exemplars, ex, exsize);
	if (out.cands != NULL)
		cands_copy(&out, cands, candsize, cands_len);
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
	extract_tags(s, start, end, buf, bufsize, result, &pending_stats, false,
				 NULL, 0, NULL, NULL, 0, NULL);
}

void
pssc_extract_tags_ex(const char *s, size_t start, size_t end, char *buf,
					 size_t bufsize, PsscExtractResult *result,
					 char *ex, size_t exsize, size_t *exlen,
					 char *cands, size_t candsize, size_t *cands_len)
{
	extract_tags(s, start, end, buf, bufsize, result, &pending_stats, false,
				 ex, exsize, exlen, cands, candsize, cands_len);
}

void
pssc_extract_recap(const char *tags, size_t tags_len, const char *cands,
				   size_t cands_len, Oid userid, char *buf, size_t bufsize,
				   PsscExtractResult *result, char *newcands,
				   size_t candsize, size_t *newcands_len)
{
	PsscTagsetEnv env;
	PsscTagsetLimits limits;
	PsscTagsetOut out;
	PsscTagsetStats stats;

	memset(result, 0, sizeof(*result));
	*newcands_len = 0;
	if (extract_cxt == NULL)
	{
		result->oom = true;
		result->hash = pssc_tagset_hash(buf, 0);
		return;
	}
	memset(&env, 0, sizeof(env));
	env.alloc = env_alloc;
	env.cap = pssc_cap_hook(false, userid, MyDatabaseId);
	memset(&limits, 0, sizeof(limits));
	limits.max_tags = pssc_max_tags;
	limits.max_tagset_bytes = (int) Min((size_t) pssc_max_tagset_bytes, bufsize);
	memset(&out, 0, sizeof(out));
	out.buf = buf;
	out.want_cands = true;
	memset(&stats, 0, sizeof(stats));
	pssc_tagset_recap(tags, tags_len, cands, cands_len, &limits, &env, &out,
					  &stats);
	if (out.cands != NULL)
		cands_copy(&out, newcands, candsize, newcands_len);
	MemoryContextReset(extract_cxt);
	/* the statement's dropped tags were counted when it was extracted */
	pending_stats.capped_tags += stats.capped_tags;
	pending_stats.cap_table_full += stats.cap_table_full;

	result->len = out.len;
	result->ntags = out.ntags;
	result->oom = out.oom;
	result->hash = pssc_tagset_hash(buf, out.len);
}

void
pssc_extract_tags_debug(const char *s, size_t start, size_t end, char *buf,
						size_t bufsize, PsscExtractResult *result,
						PsscTagsetStats *stats)
{
	uint64		compile_failures = pending_stats.regex_compile_failures;

	memset(stats, 0, sizeof(*stats));
	extract_tags(s, start, end, buf, bufsize, result, stats, true,
				 NULL, 0, NULL, NULL, 0, NULL);
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
	if (appname_cache.cxt == NULL)
		appname_cache.cxt = AllocSetContextCreate(TopMemoryContext,
												  "pg_stat_statement_context appname",
												  ALLOCSET_SMALL_SIZES);
	if (override_cache.cxt == NULL)
		override_cache.cxt = AllocSetContextCreate(TopMemoryContext,
												   "pg_stat_statement_context tags_override",
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
	stats->capped_tags += pending_stats.capped_tags;
	stats->cap_table_full += pending_stats.cap_table_full;
	stats->exemplars_dropped += pending_stats.exemplars_dropped;
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
	{
		appname_cache.valid = false;
		override_cache.valid = false;
	}
	regex_hook = fn;
	regex_hook_arg = arg;
}
