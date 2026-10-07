/*
 * extract.h
 *		Backend glue for the tag-set pipeline (src/tagset.h): runs
 *		pssc_tagset_build() with the current configuration, the database
 *		encoding, a reusable scratch memory context and the tag-set hash
 *		(DESIGN.md §3.1 item 2, §6.11).
 *
 * Nothing here throws: malformed input only yields fewer tags, and running
 * out of memory yields an empty tag set (result->oom).
 */
#ifndef PSSC_EXTRACT_H
#define PSSC_EXTRACT_H

#include "tagset.h"

/* Upper bound of pg_stat_statement_context.max_tagset_bytes. */
#define PSSC_TAGSET_BYTES_MAX	8192

typedef struct PsscExtractResult
{
	size_t		len;			/* bytes of serialized tag set in buf */
	int			ntags;
	uint32		hash;			/* pssc_tagset_hash(buf, len) */
	bool		footer;			/* tags came from the trailing footer */
	bool		oom;			/* scratch allocation failed: empty set */
} PsscExtractResult;

/* Creates the scratch memory context; called by _PG_init. */
extern void pssc_extract_init(void);

/*
 * Canonical tag set of the statement s[start, end) (s NUL-terminated; see
 * pssc_tagset_build()) under the current GUCs, written to buf, which holds
 * bufsize bytes (the set is limited to min(max_tagset_bytes, bufsize);
 * PSSC_TAGSET_BYTES_MAX always suffices). The caller resolves the range
 * with pssc_stmt_range() and pssc_stmt_owned_start(). Counters go to the
 * backend-local pending stats (pssc_extract_take_stats()).
 */
extern PGDLLEXPORT void pssc_extract_tags(const char *s, size_t start,
										  size_t end, char *buf,
										  size_t bufsize,
										  PsscExtractResult *result);

/*
 * Bytes pssc_extract_tags_ex() may write to its exemplar buffer: per key a
 * uint8 slot, a uint16 length and at most PSSC_EXEMPLAR_VALUE_MAX bytes.
 */
#define PSSC_EXEMPLARS_BUF_MAX \
	(PSSC_MAX_EXEMPLAR_KEYS * (1 + 2 + PSSC_EXEMPLAR_VALUE_MAX))

/* Bytes pssc_extract_tags_ex() and pssc_extract_recap() may write to cands. */
#define PSSC_CANDS_BUF_MAX	PSSC_TAGSET_CANDS_MAX(PSSC_TAGSET_BYTES_MAX)

/*
 * pssc_extract_tags(), and also the exemplar values (§6.13) of the
 * statement written to ex (exsize bytes, PSSC_EXEMPLARS_BUF_MAX suffices)
 * in the format pssc_store_record_ex() takes; *exlen is their length (0:
 * none, as when exemplar_keys is empty or the store is not set up).
 *
 * When cardinality caps applied under cardinality_cap_scope = role (the
 * only scope that depends on the identity, GetUserId()), the input of the
 * caps is written to cands (candsize bytes, PSSC_CANDS_BUF_MAX suffices;
 * NULL: not wanted) for pssc_extract_recap(), and *cands_len is its
 * length; otherwise *cands_len is 0 and the tag set never needs a recap.
 */
extern PGDLLEXPORT void pssc_extract_tags_ex(const char *s, size_t start,
											 size_t end, char *buf,
											 size_t bufsize,
											 PsscExtractResult *result,
											 char *ex, size_t exsize,
											 size_t *exlen, char *cands,
											 size_t candsize,
											 size_t *cands_len);

/*
 * Re-applies the cardinality caps to a tag set from pssc_extract_tags_ex()
 * (or an earlier recap), tags[0, tags_len) with its cands, in the scope of
 * userid (DESIGN.md §6.1 "Identity"): the result, in buf (bufsize bytes, at
 * least the original's, not overlapping the inputs), is the tag set the
 * extraction would have produced under userid's caps as configured now.
 * Its cands (*newcands_len bytes, possibly 0) go to newcands, as for
 * pssc_extract_tags_ex(). Counts collapses in the pending stats.
 */
extern PGDLLEXPORT void pssc_extract_recap(const char *tags, size_t tags_len,
										   const char *cands,
										   size_t cands_len, Oid userid,
										   char *buf, size_t bufsize,
										   PsscExtractResult *result,
										   char *newcands, size_t candsize,
										   size_t *newcands_len);

/*
 * pssc_extract_tags() for the debug function pg_stat_statement_context_extract()
 * (src/extract_fn.c): *stats is zeroed and receives this call's counters
 * instead of the backend-local pending stats, so a debug call never shows
 * up in _info(). The exception is regex_compile_failures: a lazy compile
 * failure disables the extractor for the backend (the hooks will not fail,
 * and count, again), so it stays counted in the pending stats as well and
 * *stats reports it too. Cardinality caps (step 8) are only peeked at: a
 * value is shown as null if it would collapse, but nothing is admitted.
 */
extern void pssc_extract_tags_debug(const char *s, size_t start, size_t end,
									char *buf, size_t bufsize,
									PsscExtractResult *result,
									PsscTagsetStats *stats);

/* False if the library was not preloaded (pssc_extract_init() not run). */
extern bool pssc_extract_available(void);

/*
 * Hash of a serialized tag set: core hash_bytes() (32 bits, the width of the
 * shared hash key's tags_hash). Stable within a server binary; not
 * persisted, so endianness does not matter.
 */
extern PGDLLEXPORT uint32 pssc_tagset_hash(const char *buf, size_t len);

/*
 * Adds the counters accumulated since the last call to *stats and zeroes
 * them (the recording path flushes them into the shared _info() counters).
 */
extern PGDLLEXPORT void pssc_extract_take_stats(PsscTagsetStats *stats);

/*
 * Counts a regex compile failure (PsscTagsetStats.regex_compile_failures);
 * called by the regex runtime.
 */
extern void pssc_extract_note_regex_compile_failure(void);

/*
 * Installs the regex extractor hook. _PG_init installs the regex runtime
 * (src/regex_runtime.h); NULL uninstalls it, and regex extractors then
 * produce no tags. arg is passed
 * to fn as its first argument. See PsscRegexExtractFn for the contract.
 */
extern PGDLLEXPORT void pssc_extract_set_regex_hook(PsscRegexExtractFn fn,
													void *arg);

#endif							/* PSSC_EXTRACT_H */
