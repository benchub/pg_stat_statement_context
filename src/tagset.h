/*
 * tagset.h
 *		Tag-set canonicalization pipeline and extractor chain (DESIGN.md §3.1
 *		item 2, §4.2, §6.5, §6.11): turn one statement's comments into a
 *		canonical, bounded, serialized tag set.
 *
 * Backend-independent like scan.c and pairs.c: no palloc, elog or encoding
 * functions. Everything the backend supplies (scratch memory, encoding
 * validation, character-boundary clipping, the regex extractor) comes in
 * through PsscTagsetEnv, so test/unit and fuzz/ build this file standalone
 * (with -DPSSC_STANDALONE). src/extract.c is the backend glue. The pipeline
 * never fails: malformed input only produces fewer tags (and counters), and
 * an allocation failure produces an empty tag set with out->oom set.
 *
 * Pipeline (DESIGN.md §6.11). For each extractor, in configuration order:
 *	- find its comments with pssc_scan_statement() for its position (scans
 *	  are shared between extractors with the same position) and parse each
 *	  comment body with its format's parser (regex: env->regex);
 *	- run every pair through, in order:
 *	  1. decode (done by the parser);
 *	  2. reject keys and values that contain NUL or fail env->verify;
 *	  3. the extractor's "keys" allowlist, matching ORIGINAL key names
 *		 (sqlcommenter and marginalia only; a regex extractor's keys name
 *		 its capture groups);
 *	  4. the extractor's "rename" (a renamed key is checked with env->verify
 *		 like step 2: rename targets are config bytes, possibly in another
 *		 encoding than this database);
 *	  5. the global allowlist ("tags"), or the denylist ("exclude_tags")
 *		 when tags = '*';
 *	  6. drop keys longer than PSSC_MAX_KEY_LEN (63) bytes;
 *	  7. truncate the value to max_tag_value_len bytes with env->cliplen.
 * Chain: an extractor "produces" tags if at least one of its pairs survives
 * steps 2-7. Extractors run in order until one produces tags; after that,
 * only extractors with merge=on still run, and their tags are added. When
 * the same final key (after rename) occurs more than once, the first
 * occurrence wins: an earlier extractor beats a later one, and within one
 * extractor the earlier comment, then the earlier pair, wins.
 * Footer fallback (§6.5): if the statement's own range yields no tag, the
 * whole chain is run again on the comments after the range, found with
 * pssc_scan_footer() (only reported if the statement is provably the last).
 * Serialization: tags are taken in priority order, and each is kept if it
 * still fits both max_tags and max_tagset_bytes, otherwise dropped (counted
 * in dropped_tags); a lower-priority tag that fits is kept after a
 * higher-priority one that did not. Priority is
 * the position of the key in the global allowlist, or the sorted key order
 * when tags = '*' (so overflow is dropped in reverse sorted-key order). The
 * kept tags are sorted by key (bytewise, a prefix sorts first) and written
 * as key \0 value \0 ..., so the same tags in any order serialize to the same
 * bytes.
 */
#ifndef PSSC_TAGSET_H
#define PSSC_TAGSET_H

#include <stddef.h>
#include <stdint.h>
#ifndef true					/* c.h (with stdbool.h) may already define bool */
#include <stdbool.h>
#endif

#include "pairs.h"
#include "scan.h"

/* guc.h */
struct PsscExtractorList;
struct PsscExtractor;
struct PsscTagList;

/*
 * Regex extractor hook (backlog item 20261005-091225-10). Called once per
 * comment of a regex extractor, with the comment body (delimiters
 * stripped, see pssc_comment_body()). index is the extractor's position in
 * list, so an implementation can find its compiled regex. It reports pairs
 * like the pair parsers do, in out->pairs (at most out->max_pairs); unlike
 * them, it may point keys and values anywhere that stays valid until
 * pssc_tagset_build() returns (into body, into list, or into out->buf,
 * which holds out->bufsize scratch bytes). Pairs flagged
 * PSSC_PAIR_KEY_NUL / PSSC_PAIR_VALUE_NUL are rejected, as are keys and
 * values that contain a NUL byte anyway. Must not fail or throw.
 */
typedef void (*PsscRegexExtractFn) (void *arg, int index,
									const struct PsscExtractorList *list,
									const char *body, size_t len,
									const PsscPairOut *out,
									PsscPairResult *result);

typedef struct PsscTagsetEnv
{
	void	   *arg;			/* passed to every callback */

	/*
	 * Scratch memory, released by the caller in bulk after
	 * pssc_tagset_build() returns (nothing is freed individually). Returns
	 * NULL on failure.
	 */
	void	   *(*alloc) (void *arg, size_t size);

	/* True if s[0, len) (never containing NUL) is valid in the encoding. */
	bool		(*verify) (void *arg, const char *s, size_t len);

	/*
	 * Length of the longest prefix of the valid string s[0, len) that is at
	 * most limit bytes and ends on a character boundary (pg_mbcliplen).
	 * Only called with len > limit.
	 */
	size_t		(*cliplen) (void *arg, const char *s, size_t len, size_t limit);

	/* Regex extractor; NULL means regex extractors produce no pairs. */
	PsscRegexExtractFn regex;
} PsscTagsetEnv;

typedef struct PsscTagsetLimits
{
	int			max_tags;		/* >= 1 */
	int			max_tag_value_len;	/* bytes, >= 1 */
	int			max_tagset_bytes;	/* bytes, >= 0 */
	int			scan_window;	/* bytes, >= 1 */
	bool		standard_conforming_strings;
} PsscTagsetLimits;

/*
 * Counters, ADDED to (never reset) by pssc_tagset_build(). The backend keeps
 * one backend-local copy that the recording path flushes into _info().
 */
typedef struct PsscTagsetStats
{
	/*
	 * Pairs rejected for their content: a key or value containing NUL or
	 * invalid in the encoding (step 2, and a renamed key in step 4), or a
	 * key longer than 63 bytes (step 6); plus the segments a parser reports
	 * as malformed in a comment from which the same parser also obtained at
	 * least one well-formed pair. Malformed segments of a comment with no
	 * well-formed pair are not counted: with several formats configured,
	 * each parser sees the other formats' comments as malformed.
	 */
	uint64_t	invalid_tags;

	/*
	 * Tags dropped because the tag set would exceed max_tags or
	 * max_tagset_bytes, plus well-formed pairs a parser could not store
	 * (does not happen with the buffers the pipeline provides).
	 */
	uint64_t	dropped_tags;

	/* Scans that used the inexact tail path (scan.h "heuristic"). */
	uint64_t	heuristic_scans;

	/*
	 * Regex extractors whose lazy per-backend compilation failed (each is
	 * then disabled until the next config generation). Never touched by
	 * pssc_tagset_build(); the regex runtime (src/regex_runtime.c) counts
	 * them in the backend-local copy.
	 */
	uint64_t	regex_compile_failures;
} PsscTagsetStats;

typedef struct PsscTagsetOut
{
	char	   *buf;			/* caller's buffer, >= max_tagset_bytes */
	size_t		len;			/* bytes written: k \0 v \0 ... */
	int			ntags;
	bool		footer;			/* tags came from the trailing footer */
	bool		oom;			/* env->alloc failed: empty result */
} PsscTagsetOut;

/*
 * Build the canonical tag set of statement s[start, end) (end <= strlen(s);
 * s must be NUL-terminated, the footer fallback reads past end). tags and
 * exclude_tags are the parsed global lists; exclude_tags is only consulted
 * when tags is '*'. Never fails.
 */
extern void pssc_tagset_build(const char *s, size_t start, size_t end,
							  const struct PsscExtractorList *extractors,
							  const struct PsscTagList *tags,
							  const struct PsscTagList *exclude_tags,
							  const PsscTagsetLimits *limits,
							  const PsscTagsetEnv *env,
							  PsscTagsetOut *out,
							  PsscTagsetStats *stats);

#endif							/* PSSC_TAGSET_H */
