/*
 * pairs.h
 *		SQLCommenter and marginalia pair parsers: turn one comment body into
 *		raw (key, value) pairs (DESIGN.md §4.2, §6.11 step 1, §9 fuzzing).
 *
 * Backend-independent like scan.c: no postgres.h, no palloc, no elog and no
 * heap allocation, so test/unit and fuzz/ build it standalone. The parsers
 * write only into the caller's pair array and byte buffer, read only
 * body[0 .. len - 1] (the body need not be NUL-terminated; NUL bytes in it
 * are ordinary bytes), run in O(len) time and never fail: a pair that cannot
 * be parsed is skipped and counted in nmalformed, and a pair that does not fit
 * the caller's arrays is skipped and counted in ndropped.
 *
 * Input: one comment BODY, without delimiters. Callers holding a scanner span
 * (scan.h's PsscCommentSpan, delimiters included) strip them with
 * pssc_comment_body() first. A body-only API also serves inputs that never had
 * delimiters (the roadmap appname extractor, DESIGN.md §8).
 *
 * Common rules (both formats):
 *	- ASCII whitespace (space, \t, \n, \r, \f, \v) is trimmed around the body,
 *	  around each pair, and around each key and unquoted value.
 *	- Empty or whitespace-only segments between separators (a trailing ','
 *	  or ",,") are ignored and not counted.
 *	- A key must be non-empty and contain no ASCII whitespace (as written,
 *	  before any decoding). Otherwise the pair is malformed. This keeps prose
 *	  such as marginalia's with_annotation text ("Request from user: 5") from
 *	  turning into pairs.
 *	- A segment with no key/value separator ("custom annotation") is
 *	  malformed.
 *	- Pairs are reported in source order. Duplicate keys are all reported; the
 *	  extractor chain (DESIGN.md §6.11) decides which wins.
 *	- Decoding never lengthens text, so a buffer of len bytes always holds
 *	  every pair, and (len + 1) / 2 + 1 pairs is more than any body can
 *	  produce. Smaller arrays are allowed: a pair that does not fit (pair
 *	  array full, or its decoded key + value exceed the buffer space left) is
 *	  dropped whole, never cut, and later pairs are still tried.
 *	- Decoded keys and values are not NUL-terminated. A NUL byte (decoded
 *	  %00, or a raw NUL in the body) is kept and flagged with
 *	  PSSC_PAIR_KEY_NUL / PSSC_PAIR_VALUE_NUL so the pipeline can reject the
 *	  pair (DESIGN.md §6.11 step 2). Encoding validity is not checked here.
 *
 * SQLCommenter (https://google.github.io/sqlcommenter/spec/):
 *	key='value',key2='value2'
 *	- Pairs are separated by ','. The key is the text before the first '=',
 *	  and the value must be a single-quoted string, optionally surrounded by
 *	  whitespace; anything else after the closing quote, before the next ','
 *	  or the end, makes the pair malformed. An unquoted value is malformed.
 *	  After such an error, parsing resumes after the next ',' (not
 *	  quote-aware). A pair whose key is invalid is still parsed to its end
 *	  (quotes respected) before it is counted as malformed.
 *	- Quote-aware: a ',' inside a quoted value does not end the pair (emitters
 *	  that do not URL-encode values still parse).
 *	- Backslash escapes, in keys and values, applied first: \' -> ' and
 *	  \\ -> \. A backslash before any other byte is kept as-is (so
 *	  '%5C\d' keeps "\d"). In a value, \' does not close the quote.
 *	- Then, if url_decode is on, both the key and the value are
 *	  percent-decoded, as the spec's parsing section requires (keys are
 *	  URL-encoded by emitters too): %XX with two hex digits (either case)
 *	  becomes that byte. An invalid escape ("%zz", "%4", a trailing "%") is
 *	  kept literally and flagged PSSC_PAIR_BAD_ESCAPE. A raw '+' becomes a
 *	  space (form encoding: Go's url.QueryEscape and Java's URLEncoder
 *	  encode a space as '+'), and %2B becomes '+'. Emitters that encode a
 *	  space as %20 (Python urllib quote, JS encodeURIComponent, Rails
 *	  ERB::Util.url_encode) also encode a literal '+' as %2B, so a raw '+'
 *	  never means a literal plus from a conforming emitter, and every
 *	  emitter yields the same decoded value.
 *	- With url_decode off, keys and values are only backslash-unescaped
 *	  ('+' and %XX stay as written).
 *	- An unterminated quote makes the pair malformed; parsing resumes after
 *	  the next ',' following the opening quote.
 *
 * marginalia (basecamp/marginalia, Rails ActiveRecord::QueryLogs legacy
 * format):
 *	application:Foo,controller:users,action:show
 *	- Segments are split on every pair_sep (default ","), scanning left to
 *	  right, and each segment on the FIRST kv_sep (default ":") that lies
 *	  wholly inside it, so line:app/models/u.rb:12 yields key "line" and
 *	  value "app/models/u.rb:12". Pair boundaries win when separators
 *	  overlap: with kv_sep "=>" and pair_sep ">", "a=>b" is the two
 *	  malformed segments "a=" and "b".
 *	- No quoting, escaping or decoding: keys and values are the raw, trimmed
 *	  bytes.
 *	- Separators are byte strings of 1 to PSSC_MAX_SEP_LEN bytes. If either
 *	  length is 0 or too long, the defaults are used for that separator (the
 *	  DSL's check_hook rejects such configurations, so this only guards
 *	  against misuse).
 */
#ifndef PSSC_PAIRS_H
#define PSSC_PAIRS_H

#include <stddef.h>
#include <stdint.h>
#ifndef true					/* c.h (with stdbool.h) may already define bool */
#include <stdbool.h>
#endif

#define PSSC_MAX_SEP_LEN		8

/* PsscPair.flags */
#define PSSC_PAIR_KEY_NUL		0x01	/* decoded key contains a NUL byte */
#define PSSC_PAIR_VALUE_NUL		0x02	/* decoded value contains a NUL byte */
#define PSSC_PAIR_BAD_ESCAPE	0x04	/* an invalid %-escape was kept as-is */

/* One decoded pair. key and value point into the caller's buffer. */
typedef struct PsscPair
{
	const char *key;
	size_t		keylen;
	const char *value;
	size_t		valuelen;
	unsigned int flags;			/* PSSC_PAIR_* */
} PsscPair;

/* Caller-provided output storage. */
typedef struct PsscPairOut
{
	PsscPair   *pairs;			/* room for max_pairs pairs */
	size_t		max_pairs;
	char	   *buf;			/* decoded keys and values */
	size_t		bufsize;
} PsscPairOut;

typedef struct PsscPairResult
{
	size_t		npairs;			/* valid entries in out->pairs */
	size_t		nmalformed;		/* segments skipped as malformed */
	size_t		ndropped;		/* well-formed pairs that did not fit */
	size_t		bufused;		/* bytes of out->buf written */
} PsscPairResult;

typedef struct PsscMarginaliaOpts
{
	const char *kv_sep;
	size_t		kv_sep_len;
	const char *pair_sep;
	size_t		pair_sep_len;
} PsscMarginaliaOpts;

/*
 * Strip the delimiters from a complete comment span as reported by the
 * scanner: span[0 .. len - 1] is either a block comment (slash-star ...
 * star-slash) or a line comment ("--" ...). Sets *body_off and *body_len so
 * that the body is span[*body_off .. *body_off + *body_len - 1]: for a block
 * comment everything between the outermost delimiters (a nested comment stays
 * part of the body), for a line comment everything after "--". Returns false,
 * with *body_off = *body_len = 0, if the span is neither.
 */
extern bool pssc_comment_body(const char *span, size_t len,
							  size_t *body_off, size_t *body_len);

/* Parse a SQLCommenter comment body (see above). */
extern void pssc_parse_sqlcommenter(const char *body, size_t len,
									bool url_decode,
									const PsscPairOut *out,
									PsscPairResult *result);

/* Parse a marginalia comment body (see above). opts may be NULL (defaults). */
extern void pssc_parse_marginalia(const char *body, size_t len,
								  const PsscMarginaliaOpts *opts,
								  const PsscPairOut *out,
								  PsscPairResult *result);

#ifdef PSSC_PAIRS_CHECKED
/*
 * Test-only build (test/unit): every body byte read asserts it is inside
 * [0, len) and increments this counter, so tests can check parsing is linear.
 */
extern unsigned long pssc_pairs_reads;
#endif

#endif							/* PSSC_PAIRS_H */
