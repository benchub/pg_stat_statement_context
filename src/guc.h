/*
 * guc.h
 *		Configuration (DESIGN.md §3.1 item 6, §4.1): GUC variables and the
 *		parsed forms of the list-valued settings, for the other modules.
 *
 * All GUCs are defined by pssc_guc_define(), which _PG_init calls only while
 * shared_preload_libraries is being processed. Plain variables are owned by
 * guc.c and updated by the GUC machinery. Parsed settings (tags,
 * exclude_tags, extractors and normalize) are flat, pointer-free blobs built by
 * a check_hook and installed by an assign_hook that cannot fail; each
 * effective change bumps the backend-local config generation, so caches
 * derived from the config (e.g. compiled regexes) can tell they are stale.
 *
 * Never keep a PsscTagList or PsscExtractorList pointer across statements:
 * the GUC machinery frees a blob once it is replaced. Fetch it again with
 * pssc_guc_tags(), pssc_guc_exclude_tags() or pssc_guc_extractors() each
 * time it is needed.
 */
#ifndef PSSC_GUC_H
#define PSSC_GUC_H

/* GUC prefix (reserved once all GUCs are defined). */
#define PSSC_GUC_PREFIX "pg_stat_statement_context"

/* Tag keys are limited to this many bytes (DESIGN.md §4.1, §6.11). */
#define PSSC_MAX_KEY_LEN 63

/*
 * tags and exclude_tags accept at most this many entries (counted before
 * duplicates are removed). This bounds the parsed blob to about 73 kB.
 */
#define PSSC_MAX_TAG_LIST_ENTRIES 1024

/* pg_stat_statement_context.track */
typedef enum PsscTrackLevel
{
	PSSC_TRACK_NONE,
	PSSC_TRACK_TOP,
	PSSC_TRACK_ALL
} PsscTrackLevel;

/* pg_stat_statement_context.nested_tags */
typedef enum PsscNestedTags
{
	PSSC_NESTED_INHERIT,		/* nested statements use the top-level tags */
	PSSC_NESTED_SCAN,			/* scan the nested statement's own source */
	PSSC_NESTED_NONE			/* nested statements get no tags */
} PsscNestedTags;

/* pg_stat_statement_context.untagged */
typedef enum PsscUntagged
{
	PSSC_UNTAGGED_SKIP,			/* don't record statements without tags */
	PSSC_UNTAGGED_RECORD		/* record them with an empty tag set */
} PsscUntagged;

/*
 * Variables and accessors are PGDLLEXPORT so the TEST-ONLY module
 * test/modules/pssc_guc_test can reach them through load_external_function.
 */

/* superuser (PGC_SUSET) */
extern PGDLLEXPORT bool pssc_enabled;
extern PGDLLEXPORT int pssc_track;			/* PsscTrackLevel */
extern PGDLLEXPORT bool pssc_track_utility;
extern PGDLLEXPORT int pssc_nested_tags;	/* PsscNestedTags */

/* postmaster (PGC_POSTMASTER): fixed after startup */
extern PGDLLEXPORT int pssc_max_entries;
extern PGDLLEXPORT int pssc_bucket_count;
extern PGDLLEXPORT int pssc_bucket_interval;	/* seconds */
extern PGDLLEXPORT int pssc_max_tags;
extern PGDLLEXPORT int pssc_max_tag_value_len; /* bytes */
extern PGDLLEXPORT int pssc_max_tagset_bytes;
extern PGDLLEXPORT bool pssc_reclaim_worker;
extern PGDLLEXPORT char *pssc_exemplar_keys;	/* raw text; use pssc_guc_exemplar_keys() */
extern PGDLLEXPORT int pssc_exemplar_memory;	/* kB */

/* sighup (PGC_SIGHUP) */
extern PGDLLEXPORT int pssc_scan_window;	/* bytes */
extern PGDLLEXPORT int pssc_reclaim_worker_interval;	/* ms */
extern PGDLLEXPORT bool pssc_save;
extern PGDLLEXPORT char *pssc_extractors;	/* raw DSL text; use pssc_guc_extractors() */
extern PGDLLEXPORT char *pssc_tags;			/* raw text; use pssc_guc_tags() */
extern PGDLLEXPORT char *pssc_exclude_tags; /* raw text; use pssc_guc_exclude_tags() */
extern PGDLLEXPORT int pssc_untagged;		/* PsscUntagged */
extern PGDLLEXPORT char *pssc_normalize;	/* raw text; use pssc_guc_normalize() */
extern PGDLLEXPORT char *pssc_tags_override;	/* raw text; use pssc_guc_override() */

/*
 * Parsed tag key list (tags / exclude_tags). Keys are kept in list order
 * (the allowlist order decides which tags are dropped first, §4.1), with
 * surrounding whitespace trimmed and later duplicates removed. Keys are
 * case-sensitive, 1..PSSC_MAX_KEY_LEN bytes, and NUL-terminated.
 * Opaque; use the accessors below.
 */
typedef struct PsscTagList PsscTagList;


/* True for tags = '*' (keep every tag; exclude_tags then applies). */
extern PGDLLEXPORT bool pssc_tag_list_match_all(const PsscTagList *list);

/* Number of keys (0 for '*' and for an empty list). */
extern PGDLLEXPORT int pssc_tag_list_count(const PsscTagList *list);

/* Key i (0-based, < count); its length in bytes is stored in *len if non-NULL. */
extern PGDLLEXPORT const char *pssc_tag_list_key(const PsscTagList *list, int i,
												 int *len);

/*
 * Position of key[0..len) in the list, or -1 if absent. Exact byte
 * comparison; does not consider match_all.
 */
extern PGDLLEXPORT int pssc_tag_list_find(const PsscTagList *list,
										  const char *key, int len);

/* Current parsed tags / exclude_tags. Never NULL once the GUCs are defined. */
extern PGDLLEXPORT const PsscTagList *pssc_guc_tags(void);
extern PGDLLEXPORT const PsscTagList *pssc_guc_exclude_tags(void);
/* Parsed exemplar_keys (at most PSSC_MAX_EXEMPLAR_KEYS keys). */
extern PGDLLEXPORT const PsscTagList *pssc_guc_exemplar_keys(void);

/*
 * Parsed pg_stat_statement_context.extractors (DESIGN.md §4.2).
 *
 * One flat, pointer-free blob: the header, then the extractors, then the
 * PsscBlobStr / PsscBlobRename arrays they reference, then the string bytes.
 * Every reference is a byte offset from the start of the blob, so the blob
 * can be copied or moved as a whole. Strings are NUL-terminated; a PsscBlobStr
 * with len 0 is absent (the parser rejects empty values). The blob is built
 * deterministically (zero-filled first), so equivalent settings give
 * byte-identical blobs.
 *
 * Every field is filled in: omitted parameters get their defaults (position:
 * append for sqlcommenter and marginalia, any for regex; merge off;
 * url_decode on for sqlcommenter; kv_sep ":" and pair_sep "," for
 * marginalia). appname extractors (source APPNAME) have the defaults of
 * their format, except position, which they do not take (stored as any).
 */
#define PSSC_MAX_EXTRACTORS			16
#define PSSC_MAX_REGEX_PATTERN_LEN	1024	/* bytes */

typedef enum PsscExtractorKind
{
	PSSC_EXTRACTOR_SQLCOMMENTER,
	PSSC_EXTRACTOR_MARGINALIA,
	PSSC_EXTRACTOR_REGEX
} PsscExtractorKind;

/*
 * What an extractor parses (backlog item 20261005-091225-38). An
 * appname(format=F, ...) extractor is stored with kind F and source
 * APPNAME: it parses application_name with F's rules and parameters instead
 * of comments, and has no position.
 */
typedef enum PsscExtractorSource
{
	PSSC_SOURCE_COMMENT = 0,
	PSSC_SOURCE_APPNAME
} PsscExtractorSource;

typedef struct PsscBlobStr
{
	uint32		off;			/* from the start of the blob */
	uint32		len;			/* bytes, excluding the NUL */
} PsscBlobStr;

typedef struct PsscBlobRename
{
	PsscBlobStr from;
	PsscBlobStr to;
} PsscBlobRename;

typedef struct PsscExtractor
{
	uint8		kind;			/* PsscExtractorKind */
	uint8		position;		/* PsscPosition (scan.h) */
	bool		merge;
	bool		url_decode;		/* sqlcommenter only; false otherwise */

	/*
	 * keys: for sqlcommenter and marginalia, the per-extractor allowlist of
	 * original key names, active only if has_keys. For regex, always present:
	 * keys[i] names capture group i + 1 (one key per group).
	 */
	bool		has_keys;
	uint8		source;			/* PsscExtractorSource */
	uint32		nkeys;
	uint32		keys_off;		/* PsscBlobStr[nkeys] */
	uint32		nrename;
	uint32		rename_off;		/* PsscBlobRename[nrename], distinct "from" */
	PsscBlobStr kv_sep;			/* marginalia only */
	PsscBlobStr pair_sep;		/* marginalia only */
	PsscBlobStr pattern;		/* regex only */
} PsscExtractor;

typedef struct PsscExtractorList
{
	uint32		size;			/* total blob size in bytes */
	uint32		nextractors;
	PsscExtractor extractors[FLEXIBLE_ARRAY_MEMBER];
} PsscExtractorList;

static inline const char *
pssc_blob_str(const PsscExtractorList *list, PsscBlobStr s)
{
	return (const char *) list + s.off;
}

static inline const PsscBlobStr *
pssc_extractor_keys(const PsscExtractorList *list, const PsscExtractor *e)
{
	return (const PsscBlobStr *) ((const char *) list + e->keys_off);
}

static inline const PsscBlobRename *
pssc_extractor_renames(const PsscExtractorList *list, const PsscExtractor *e)
{
	return (const PsscBlobRename *) ((const char *) list + e->rename_off);
}

/* Current parsed extractors. Never NULL once the GUCs are defined. */
extern PGDLLEXPORT const PsscExtractorList *pssc_guc_extractors(void);

/*
 * Parsed pg_stat_statement_context.normalize (DESIGN.md §6.11 step 6):
 * "key: 'pattern' => 'replacement', ..." regex-replace rules for tag values,
 * applied in list order. Flat and pointer-free like PsscExtractorList:
 * header, rules, then the NUL-terminated strings (offsets from the start of
 * the blob). The key and pattern are never empty; an empty replacement is
 * {0, 0}. max_ref is the highest \N back-reference in the replacement (0 if
 * none); the check hook guarantees the pattern has that many groups.
 */
#define PSSC_MAX_NORMALIZE_RULES			32
#define PSSC_MAX_NORMALIZE_REPLACEMENT_LEN	1024	/* bytes */

typedef struct PsscNormalizeRule
{
	PsscBlobStr key;
	PsscBlobStr pattern;
	PsscBlobStr replacement;
	uint32		max_ref;
} PsscNormalizeRule;

typedef struct PsscNormalizeList
{
	uint32		size;			/* total blob size in bytes */
	uint32		nrules;
	PsscNormalizeRule rules[FLEXIBLE_ARRAY_MEMBER];
} PsscNormalizeList;

static inline const char *
pssc_normalize_str(const PsscNormalizeList *list, PsscBlobStr s)
{
	return s.len == 0 ? "" : (const char *) list + s.off;
}

/* Current parsed normalize rules. Never NULL once the GUCs are defined. */
extern PGDLLEXPORT const PsscNormalizeList *pssc_guc_normalize(void);

/*
 * Backend-local config generation: bumped whenever the effective value of
 * extractors, tags, exclude_tags or normalize changes in this process.
 */
extern PGDLLEXPORT uint64 pssc_guc_config_generation(void);

/*
 * Parsed pg_stat_statement_context.tags_override (backlog item
 * 20261005-091225-30): the sqlcommenter pairs of the setting, URL-decoded,
 * in order (duplicates kept; the pipeline keeps the first). Flat and
 * pointer-free like PsscNormalizeList: header, pairs, then the
 * NUL-terminated strings. Keys are 1..PSSC_MAX_KEY_LEN bytes; an empty value
 * is {0, 0}. Neither contains a NUL byte.
 */
typedef struct PsscOverridePair
{
	PsscBlobStr key;
	PsscBlobStr value;
} PsscOverridePair;

typedef struct PsscOverrideList
{
	uint32		size;			/* total blob size in bytes */
	uint32		npairs;			/* >= 1 */
	PsscOverridePair pairs[FLEXIBLE_ARRAY_MEMBER];
} PsscOverrideList;

static inline const char *
pssc_override_str(const PsscOverrideList *list, PsscBlobStr s)
{
	return s.len == 0 ? "" : (const char *) list + s.off;
}

/* Current parsed tags_override; NULL when it holds no pair. */
extern PGDLLEXPORT const PsscOverrideList *pssc_guc_override(void);

/*
 * Backend-local generation of tags_override: bumped whenever its parsed
 * value changes (SET, SET LOCAL, RESET, the end of a transaction or of a
 * function's SET clause that restores another value).
 */
extern PGDLLEXPORT uint64 pssc_guc_override_generation(void);

/*
 * Size in bytes of a PsscTagList blob holding nkeys keys whose bytes,
 * including one NUL per key, total keybytes. Returns false, without
 * computing anything that could overflow, if nkeys exceeds
 * PSSC_MAX_TAG_LIST_ENTRIES or keybytes exceeds nkeys * (PSSC_MAX_KEY_LEN + 1).
 * Exported for the TEST-ONLY module's boundary checks.
 */
extern PGDLLEXPORT bool pssc_tag_list_blob_size(size_t nkeys, size_t keybytes,
											   size_t *size);

/* Define all GUCs and reserve the prefix. Call from _PG_init while preloading. */
extern void pssc_guc_define(void);

#endif							/* PSSC_GUC_H */
