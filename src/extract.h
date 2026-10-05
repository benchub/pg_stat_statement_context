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
