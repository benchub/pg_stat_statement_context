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
 * Does not fail the statement because of a pattern: compile failures,
 * including a compile aborted at the compile time limit
 * (PSSC_REGEX_COMPILE_LIMIT_MS), disable the extractor for this backend
 * until the next regex generation (counted in the backend's
 * PsscTagsetStats.regex_compile_failures); match errors yield no pairs for
 * that comment. Query cancel and other interrupts are still honored (they
 * propagate as the usual ERROR / FATAL), as are errors outside a short
 * allowlist (out of memory, program limit, invalid regex; see
 * regex_runtime.c).
 */
#ifndef PSSC_REGEX_RUNTIME_H
#define PSSC_REGEX_RUNTIME_H

#include "mb/pg_wchar.h"
#include "regex/regex.h"
#include "utils/guc.h"
#include "tagset.h"

/* Creates the memory contexts and installs the hook; called by _PG_init. */
extern void pssc_regex_init(void);

/*
 * Releases the compiled regexes of an older regex generation (also done
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
 * Value normalization (DESIGN.md §6.11 step 6; backlog item
 * 20261005-091225-41): applies, in order, every rule of list whose key is
 * key[0, klen) to val[0, vlen) (valid in the database encoding, no NUL),
 * each to the previous rule's output. A rule is equivalent to
 * regexp_replace(value COLLATE "C", pattern, replacement, 'g') (same engine
 * and flags as the regex extractor: REG_ADVANCED, C collation, no
 * back-references; \1..\9, \& and \\ in the replacement), except that
 * its output is cut on a character boundary at limit bytes (>= vlen), and
 * matching stops there. Rules are compiled lazily per backend like regex
 * extractors (a compile failure disables the rule until the next regex
 * generation and counts in regex_compile_failures). Each result is
 * allocated with alloc(alloc_arg, ...). Returns PSSC_NORMALIZE_NO_RULES if
 * no rule names key, PSSC_NORMALIZE_FAILED if a rule is disabled or failed
 * (engine error, out of memory), else PSSC_NORMALIZE_DONE. Interrupts
 * propagate as ERROR, as for the regex extractor.
 */
extern PGDLLEXPORT PsscNormalizeResult pssc_regex_normalize(const struct PsscNormalizeList *list,
															const char *key, size_t klen,
															const char *val, size_t vlen,
															size_t limit,
															void *(*alloc) (void *arg, size_t size),
															void *alloc_arg,
															const char **out, size_t *outlen);

/*
 * Number of regex extractor failures in this backend so far that may not
 * happen again on the same input: a match that failed (engine error, out of
 * memory, a swallowed ERROR) and a compile put off because interrupts were
 * held off. Used to decide whether a result may be cached (src/extract.c).
 * Normalize failures are visible as PsscTagsetStats.normalize_failures.
 */
extern uint64 pssc_regex_transient_failures(void);

/*
 * TEST-ONLY fault injection (test/modules/pssc_extract_test). When set, it
 * is called right before creating the memory context of a pattern to
 * compile (phase PSSC_REGEX_TEST_CONTEXT), before each pg_regcomp
 * (PSSC_REGEX_TEST_COMPILE) and before each pg_regexec (PSSC_REGEX_TEST_EXEC)
 * of extractor index, inside the same error handling as the engine call;
 * the PSSC_REGEX_TEST_NORM_* phases are the same for normalize rule index.
 * PSSC_REGEX_TEST_CHECK is the check hooks' test compile. The *COMPILE and
 * CHECK phases run inside the compile time limit. Returning REG_OKAY lets
 * the engine run;
 * any other value is used as if the engine had returned it. It may also
 * throw (ereport), as the engine can.
 */
#define PSSC_REGEX_TEST_COMPILE	0
#define PSSC_REGEX_TEST_EXEC	1
#define PSSC_REGEX_TEST_CONTEXT	2
#define PSSC_REGEX_TEST_NORM_CONTEXT	3
#define PSSC_REGEX_TEST_NORM_COMPILE	4
#define PSSC_REGEX_TEST_NORM_EXEC		5
#define PSSC_REGEX_TEST_CHECK	6	/* check hook test compile; index -1 */
/*
 * Called (with the *COMPILE or CHECK phase's index) by a compile attempt
 * handling an error after its limit fired, right before the one step there
 * that allocates (and so can fail); it may throw, as that step can.
 */
#define PSSC_REGEX_TEST_COMPILE_CATCH	7
typedef int (*PsscRegexTestHook) (int phase, int index);
extern PGDLLEXPORT PsscRegexTestHook pssc_regex_test_hook;

/*
 * TEST-ONLY: for the *COMPILE and CHECK phases, called with the engine's
 * return code when the engine returns (not when it throws), still inside
 * the compile time limit. Lets tests tell an engine call that was
 * interrupted from one that completed.
 */
typedef void (*PsscRegexTestEngineHook) (int phase, int index, int rc);
extern PGDLLEXPORT PsscRegexTestEngineHook pssc_regex_test_engine_hook;

/*
 * Compile time limit per pattern, in milliseconds (backlog
 * 20261006-021334-1): some patterns take seconds or minutes to compile
 * (e.g. ((?:(?:$)|\Zda|(?<!1)|\S){0,255}) ). pssc_regex_compile_limit_ms
 * is the limit in effect: PSSC_REGEX_COMPILE_LIMIT_MS, changed only by
 * tests (<= 0: no limit). In a client backend the limit is wall-clock
 * time per attempt, and an attempt that used less than half of it in CPU
 * time (it was descheduled: a stall, not a slow pattern) is retried, up to
 * PSSC_REGEX_COMPILE_ATTEMPTS attempts; elsewhere it is CPU time.
 */
#define PSSC_REGEX_COMPILE_LIMIT_MS 100
#define PSSC_REGEX_COMPILE_ATTEMPTS 3
extern PGDLLEXPORT int pssc_regex_compile_limit_ms;

/*
 * TEST-ONLY (test/modules/pssc_extract_test): while the compile time limit
 * of a client backend's compile attempt is armed (e.g. from the test hook's
 * *COMPILE or CHECK phase), makes it expire ms from now instead, so that the
 * engine itself is interrupted mid-compile. No-op when not armed.
 */
extern PGDLLEXPORT void pssc_regex_test_expire_in(int ms);

/* pssc_regex_compile() results besides the engine's return codes. */
#define PSSC_REGEX_COMPILE_TOO_SLOW	(-1)
#define PSSC_REGEX_COMPILE_DEFERRED	(-2)

/*
 * Compiles pat[0, len) into re (REG_ADVANCED, C collation; with
 * pssc_regcomp() in a new child of cxt, released with cxt or by
 * pssc_regfree()), under the compile time limit, after calling the test
 * hook for test_phase (if >= 0) with test_index. Each attempt compiles into
 * a child context of its own; one that fails or is interrupted is deleted,
 * with whatever the engine left in it, before the next attempt or
 * returning, so only a successful attempt's memory is kept. Returns REG_OKAY,
 * an engine error code (re not compiled), or:
 *	PSSC_REGEX_COMPILE_TOO_SLOW  the compile took longer than the limit (re
 *		not compiled). In a client backend the compile is aborted at the
 *		limit (and retried after a stall, see above); in other processes
 *		(no timer) it runs to completion, and only strict callers get this
 *		result, by CPU time (non-strict ones keep the regex).
 *		A non-strict compile that completes just after the limit is kept.
 *	PSSC_REGEX_COMPILE_DEFERRED  (!strict only) a client backend with
 *		interrupts held off cannot bound the compile; try again later.
 * strict is for the GUC check hooks, which reject over-limit patterns.
 * A genuine cancel, statement_timeout or other interrupt arriving during
 * the compile propagates as usual (ERROR, or REG_CANCEL on PG14/15 with
 * the interrupt still pending); other errors are thrown too.
 */
extern int	pssc_regex_compile(MemoryContext cxt, regex_t *re, const pg_wchar *pat,
							   size_t len, bool strict, int test_phase,
							   int test_index);

/*
 * The GUC check hooks' test compile (test phase PSSC_REGEX_TEST_CHECK) of
 * the pattern of what (e.g. "extractor \"regex\""), for a value from
 * source: strict for a value a statement sets (ALTER SYSTEM, see
 * pssc_regex_note_alter_system()); when reading the configuration file
 * never PSSC_REGEX_COMPILE_TOO_SLOW, but PSSC_REGEX_COMPILE_UNCHECKED if
 * the compile was stopped at the limit, or not run (parallel workers):
 * accept the value without the checks that need the compiled regex (the
 * lazy compile repeats them).
 */
#define PSSC_REGEX_COMPILE_UNCHECKED	(-3)

extern int	pssc_regex_check_compile(MemoryContext cxt, regex_t *re,
									 const pg_wchar *pat, size_t len,
									 GucSource source, const char *what);

/* Called by the ProcessUtility hook around ALTER SYSTEM. */
extern void pssc_regex_note_alter_system(bool running);

/* Backend-local bookkeeping, for tests. */
typedef struct PsscRegexDebugStats
{
	uint64		compiles;		/* successful pg_regcomp calls (extractors and rules) */
	uint64		frees;			/* pg_regfree calls */
	int			live;			/* compiled regexes currently held */
	int			failed;			/* extractors and rules disabled by compile failure */
} PsscRegexDebugStats;

extern PGDLLEXPORT void pssc_regex_debug_stats(PsscRegexDebugStats *stats);

#endif							/* PSSC_REGEX_RUNTIME_H */
