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
 *	  6. drop keys longer than PSSC_MAX_KEY_LEN (63) bytes, then normalize
 *		 the value with the rules for its (final) key (env->normalize, the
 *		 "normalize" setting); a failure drops the pair and every later
 *		 occurrence of its key in this pass, so a raw value cannot win;
 *		 later occurrences of a normalized key are not normalized again
 *		 (they lose to the first occurrence anyway);
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
 * Cardinality caps (step 8, env->cap; NULL: off): each deduplicated tag
 * that serialization considers is first offered to env->cap, which keeps
 * its value or collapses it to null (counted in capped_tags, and also in
 * cap_table_full when the tracking table was full). The value is only
 * admitted (env->cap with admit = true) when it is about to be stored as a
 * string, so a tag that serialization drops never uses up a distinct
 * value; when the string would not fit, env->cap is only asked (admit =
 * false) whether the value would collapse, and the tag is then kept as
 * null if that fits, like steps 8 and 9 in sequence.
 * Serialization: tags are taken in priority order, and each is kept if it
 * still fits both max_tags and max_tagset_bytes, otherwise dropped (counted
 * in dropped_tags); a lower-priority tag that fits is kept after a
 * higher-priority one that did not. Priority is
 * the position of the key in the global allowlist, or the sorted key order
 * when tags = '*' (so overflow is dropped in reverse sorted-key order). The
 * kept tags are sorted by key (bytewise, a prefix sorts first) and written
 * as key \0 value \0 ..., so the same tags in any order serialize to the same
 * bytes. A null value (step 8) is written as key \0 \0 \0: keys are never
 * empty, so two NULs right after a key's terminator cannot start an empty
 * string value (whose terminator is followed by the next key or the end),
 * and a null never serializes, hashes or compares like any string. Read
 * stored sets with pssc_tagset_next().
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

/*
 * Value normalization hook (step 6 of the pipeline; backlog item
 * 20261005-091225-41). Called with a final key (1..PSSC_MAX_KEY_LEN bytes)
 * and its value (valid in the encoding, no NUL). Returns NO_RULES if no
 * rule names key (the value is kept as is), DONE with the result in *out /
 * *outlen (at most limit bytes, valid until pssc_tagset_build() returns,
 * e.g. allocated with env->alloc), or FAILED (the pair is dropped). limit is
 * at least vlen. Must not fail or throw otherwise.
 */
typedef enum PsscNormalizeResult
{
	PSSC_NORMALIZE_NO_RULES = 0,
	PSSC_NORMALIZE_DONE,
	PSSC_NORMALIZE_FAILED
} PsscNormalizeResult;

typedef PsscNormalizeResult (*PsscNormalizeFn) (void *arg,
												const char *key, size_t klen,
												const char *val, size_t vlen,
												size_t limit,
												const char **out, size_t *outlen);

/*
 * Cardinality cap hook (step 8; backlog item 20261005-091225-32). Called
 * with a final key (1..PSSC_MAX_KEY_LEN bytes) and its value after steps
 * 1-7 (no NUL). Returns KEEP to keep the value, NULL to collapse it to null
 * (its key is over its cap), or NULL_FULL to collapse it because the
 * structure that tracks the distinct values is full (fail closed). With
 * admit, a value that is kept must be counted as one of the key's distinct
 * values from then on; without, nothing may change (the answer is what
 * admit would return). Must not fail or throw.
 */
typedef enum PsscCapResult
{
	PSSC_CAP_KEEP = 0,
	PSSC_CAP_NULL,
	PSSC_CAP_NULL_FULL
} PsscCapResult;

typedef PsscCapResult (*PsscCapFn) (void *arg, const char *key, size_t klen,
									const char *val, size_t vlen, bool admit);

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

	/* Value normalization (step 6); NULL means no rules. */
	PsscNormalizeFn normalize;

	/* Per-key cardinality caps (step 8); NULL means no caps. */
	PsscCapFn	cap;
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

	/*
	 * Step 6: tags whose value the normalize rules changed, and pairs
	 * dropped because normalizing their value failed (an engine error, out
	 * of memory, or a rule disabled because its lazy compilation failed --
	 * the compilation itself counts once in regex_compile_failures). Not in
	 * _info().
	 */
	uint64_t	normalized_tags;
	uint64_t	normalize_failures;

	/*
	 * Step 8: tags whose value was collapsed to null by a cardinality cap,
	 * and among them those collapsed because the tracking structure was
	 * full (PSSC_CAP_NULL_FULL). Both in _info().
	 */
	uint64_t	capped_tags;
	uint64_t	cap_table_full;
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
 * One tag after steps 1-7, with its step 9 drop priority (lower is kept
 * first).
 */
typedef struct PsscTagCandidate
{
	const char *key;
	size_t		klen;
	const char *val;
	size_t		vlen;
	size_t		prio;
} PsscTagCandidate;

/*
 * Tags derived from application_name by the appname extractors (DESIGN.md
 * §4.2; backlog item 20261005-091225-38), built by pssc_appname_tags_build()
 * and passed to pssc_tagset_build(). The appname extractors form a chain of
 * their own, with the comment chain's rules: in configuration order, the
 * first one that produces wins and later ones run only with merge=on;
 * tags are in chain order (first occurrence first), not deduplicated. stats
 * holds the counters of the pass, which pssc_tagset_build() adds to its
 * own every time it uses the result, so a cached result counts like a fresh
 * one. Self-contained values: the backend may copy the tags elsewhere and
 * reuse them while the configuration and application_name are unchanged.
 * The tags_override pass (pssc_override_tags_build()) fills the same
 * structure.
 */
typedef struct PsscSourceTags
{
	size_t		ntags;
	const PsscTagCandidate *tags;
	PsscTagsetStats stats;
	bool		oom;			/* env->alloc failed: no tags */
} PsscSourceTags;

typedef PsscSourceTags PsscAppnameTags;

/* True if list has at least one appname extractor. */
extern bool pssc_extractors_have_appname(const struct PsscExtractorList *list);

/*
 * Runs the appname extractors of extractors on appname[0, len) (the whole
 * string is the "comment body"; for regex, env->regex is called with it)
 * through steps 1-7, into *out (zeroed first; tags allocated with
 * env->alloc, keys and values may point into appname, the blob or scratch).
 * Never fails.
 */
extern void pssc_appname_tags_build(const char *appname, size_t len,
									const struct PsscExtractorList *extractors,
									const struct PsscTagList *tags,
									const struct PsscTagList *exclude_tags,
									const PsscTagsetLimits *limits,
									const PsscTagsetEnv *env,
									PsscAppnameTags *out);

/*
 * Runs the pairs of the tags_override setting (backlog item
 * 20261005-091225-30; parsed and URL-decoded by the GUC's check_hook with
 * pssc_parse_sqlcommenter()) through steps 2 and 4-7, into *out (zeroed
 * first; tags allocated with env->alloc, keys and values point into pairs'
 * text, the blob or scratch). There is no extractor, so no step 3 ("keys");
 * step 4 applies the "rename" lists of the comment sqlcommenter extractors,
 * in configuration order, the first rule whose "from" matches the key
 * winning (no rename without such an extractor). Pairs flagged
 * PSSC_PAIR_KEY_NUL / PSSC_PAIR_VALUE_NUL are rejected like a comment's.
 * Every pair is kept (first occurrence first, not deduplicated). Never
 * fails.
 */
extern void pssc_override_tags_build(const PsscPair *pairs, size_t npairs,
									 const struct PsscExtractorList *extractors,
									 const struct PsscTagList *tags,
									 const struct PsscTagList *exclude_tags,
									 const PsscTagsetLimits *limits,
									 const PsscTagsetEnv *env,
									 PsscSourceTags *out);

/*
 * Build the canonical tag set of statement s[start, end) (end <= strlen(s);
 * s must be NUL-terminated, the footer fallback reads past end). tags and
 * exclude_tags are the parsed global lists; exclude_tags is only consulted
 * when tags is '*'. appname (NULL: none) holds the appname extractors'
 * tags; they are added after the comment chain's (own range or footer), so
 * a comment tag wins a key conflict whatever the configuration order, and
 * they do not count as tags of the statement's range for the footer
 * fallback. Comment extractors ignore appname extractors entirely (an
 * appname extractor that produced does not stop a later comment
 * extractor). Never fails.
 */
extern void pssc_tagset_build(const char *s, size_t start, size_t end,
							  const struct PsscExtractorList *extractors,
							  const struct PsscTagList *tags,
							  const struct PsscTagList *exclude_tags,
							  const PsscTagsetLimits *limits,
							  const PsscTagsetEnv *env,
							  const PsscAppnameTags *appname,
							  PsscTagsetOut *out,
							  PsscTagsetStats *stats);

/*
 * pssc_tagset_build() with the tags_override result override (NULL: none),
 * whose tags are added before every other source, so the override wins
 * every key conflict: override > comment (own range or footer) >
 * application_name. Override tags do not count as tags of the statement's
 * range for the footer fallback. Its counters are added to stats every
 * time, and its oom makes the result empty, like appname's.
 */
extern void pssc_tagset_build_with_override(const char *s, size_t start,
											size_t end,
											const struct PsscExtractorList *extractors,
											const struct PsscTagList *tags,
											const struct PsscTagList *exclude_tags,
											const PsscTagsetLimits *limits,
											const PsscTagsetEnv *env,
											const PsscAppnameTags *appname,
											const PsscSourceTags *override,
											PsscTagsetOut *out,
											PsscTagsetStats *stats);

/*
 * Output escaping of tag text stored from a SQL_ASCII database (DESIGN.md
 * §6.11): such bytes have no known encoding, so on output every byte >= 0x80
 * becomes the four characters \xHH (lowercase hex) and '\' becomes "\\",
 * making the escaping reversible and keeping distinct keys distinct. ASCII
 * other than '\' is copied. Applies to keys and values alike; the stored
 * (canonical) bytes, hash and sizes are unaffected.
 *
 * pssc_tag_escaped_len() returns the escaped length of s[0, len) (at most
 * 4 * len); pssc_tag_escape() writes it to dst, which must have room for
 * that many bytes (no NUL is added), and returns it.
 */
extern size_t pssc_tag_escaped_len(const char *s, size_t len);
extern size_t pssc_tag_escape(const char *s, size_t len, char *dst);

/* One tag of a serialized set, as pssc_tagset_next() returns it. */
typedef struct PsscTagView
{
	const char *key;
	size_t		klen;
	const char *val;			/* NULL for a null value */
	size_t		vlen;			/* 0 for a null value */
	bool		isnull;
} PsscTagView;

/*
 * Reads the tag at *off of the serialized set buf[0, len) into *t and
 * advances *off past it. Returns false at the end, or on bytes that are not
 * a well-formed tag (an empty key, or a key or value without its
 * terminator); the rest is then ignored. A null value is key \0 \0 \0.
 * Inline, so test modules and standalone builds can use it.
 */
static inline bool
pssc_tagset_next(const char *buf, size_t len, size_t *off, PsscTagView *t)
{
	size_t		o = *off;
	size_t		k = o;
	size_t		v;

	while (k < len && buf[k] != '\0')
		k++;
	if (k >= len || k == o)
		return false;
	t->key = buf + o;
	t->klen = k - o;
	v = k + 1;
	if (v + 1 < len && buf[v] == '\0' && buf[v + 1] == '\0')
	{
		t->val = NULL;
		t->vlen = 0;
		t->isnull = true;
		*off = v + 2;
		return true;
	}
	o = v;
	while (v < len && buf[v] != '\0')
		v++;
	if (v >= len)
		return false;
	t->val = buf + o;
	t->vlen = v - o;
	t->isnull = false;
	*off = v + 1;
	return true;
}

#endif							/* PSSC_TAGSET_H */
