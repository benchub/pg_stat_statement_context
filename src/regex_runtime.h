/*
 * regex_runtime.h
 *		Runtime of the regex extractor (DESIGN.md §4.2, §6.11; backlog item
 *		20261005-091225-10): per-backend lazy compilation of the configured
 *		patterns and matching of comment bodies.
 *
 * _PG_init installs pssc_regex_extract() as the pipeline's regex hook
 * (pssc_extract_set_regex_hook()). It is called once per comment body of a
 * regex extractor and reports capture group n as a pair with key n.
 *
 * Match semantics: the pattern is applied to the comment body (delimiters
 * stripped) repeatedly, like regexp_matches(..., 'g'): every non-overlapping
 * match, left to right (after an empty match the search resumes one
 * character later). A capture group that did not participate in a match
 * produces no pair; a group that matched the empty string produces an empty
 * value. Only the first participating occurrence of each key in a comment is
 * reported (the pipeline's "first occurrence wins" rule would discard the
 * others anyway, because nothing after step 1 of DESIGN.md §6.11 rejects a
 * regex pair based on its value), and matching stops once every key has a
 * value, so a comment yields at most one pair per key.
 *
 * Never fails the statement: compile failures disable the extractor for this
 * backend until the next config generation (counted in the backend's
 * PsscTagsetStats.regex_compile_failures); match errors yield no pairs for
 * that comment. Query cancel and other interrupts are still honored (they
 * propagate as the usual ERROR / FATAL).
 */
#ifndef PSSC_REGEX_RUNTIME_H
#define PSSC_REGEX_RUNTIME_H

#include "tagset.h"

/* Creates the memory contexts and installs the hook; called by _PG_init. */
extern void pssc_regex_init(void);

/*
 * Releases the compiled regexes of an older config generation (also done
 * lazily by the hook). Called by pssc_extract_tags() before every pipeline
 * run so that a reload that removes all regex extractors frees them too.
 * Never throws.
 */
extern void pssc_regex_release_stale(void);

/* The regex extractor hook (PsscRegexExtractFn); arg is unused. */
extern PGDLLEXPORT void pssc_regex_extract(void *arg, int index,
										   const struct PsscExtractorList *list,
										   const char *body, size_t len,
										   const PsscPairOut *out,
										   PsscPairResult *result);

/*
 * TEST-ONLY fault injection (test/modules/pssc_extract_test). When set, it
 * is called right before creating the memory context of a pattern to
 * compile (phase PSSC_REGEX_TEST_CONTEXT), before each pg_regcomp
 * (PSSC_REGEX_TEST_COMPILE) and before each pg_regexec (PSSC_REGEX_TEST_EXEC)
 * of extractor index, inside the same error handling as the engine call. Returning REG_OKAY lets the engine run;
 * any other value is used as if the engine had returned it. It may also
 * throw (ereport), as the engine can.
 */
#define PSSC_REGEX_TEST_COMPILE	0
#define PSSC_REGEX_TEST_EXEC	1
#define PSSC_REGEX_TEST_CONTEXT	2
typedef int (*PsscRegexTestHook) (int phase, int index);
extern PGDLLEXPORT PsscRegexTestHook pssc_regex_test_hook;

/* Backend-local bookkeeping, for tests. */
typedef struct PsscRegexDebugStats
{
	uint64		compiles;		/* successful pg_regcomp calls */
	uint64		frees;			/* pg_regfree calls */
	int			live;			/* compiled regexes currently held */
	int			failed;			/* extractors disabled by compile failure */
} PsscRegexDebugStats;

extern PGDLLEXPORT void pssc_regex_debug_stats(PsscRegexDebugStats *stats);

#endif							/* PSSC_REGEX_RUNTIME_H */
