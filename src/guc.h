/*
 * guc.h
 *		Configuration (DESIGN.md §3.1 item 6, §4.1): GUC variables and the
 *		parsed forms of the list-valued settings, for the other modules.
 *
 * All GUCs are defined by pssc_guc_define(), which _PG_init calls only while
 * shared_preload_libraries is being processed. Plain variables are owned by
 * guc.c and updated by the GUC machinery. Parsed settings (tags,
 * exclude_tags, and later extractors) are flat, pointer-free blobs built by
 * a check_hook and installed by an assign_hook that cannot fail; each
 * effective change bumps the backend-local config generation, so caches
 * derived from the config (e.g. compiled regexes) can tell they are stale.
 *
 * Never keep a PsscTagList pointer across statements: the GUC machinery
 * frees a blob once it is replaced. Fetch it again with pssc_guc_tags() /
 * pssc_guc_exclude_tags() each time it is needed.
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

/* sighup (PGC_SIGHUP) */
extern PGDLLEXPORT int pssc_scan_window;	/* bytes */
extern PGDLLEXPORT char *pssc_extractors;	/* raw DSL text; parsed by backlog item -8 */
extern PGDLLEXPORT char *pssc_tags;			/* raw text; use pssc_guc_tags() */
extern PGDLLEXPORT char *pssc_exclude_tags; /* raw text; use pssc_guc_exclude_tags() */
extern PGDLLEXPORT int pssc_untagged;		/* PsscUntagged */

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

/*
 * Backend-local config generation: bumped whenever the effective value of
 * extractors, tags or exclude_tags changes in this process.
 */
extern PGDLLEXPORT uint64 pssc_guc_config_generation(void);

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
