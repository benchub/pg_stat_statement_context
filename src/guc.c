/*
 * guc.c
 *		GUC definitions for pg_stat_statement_context (DESIGN.md §3.1 item 6,
 *		§4.1).
 *
 * List-valued settings are parsed and fully validated in their check_hook,
 * which returns one flat, pointer-free "extra" blob allocated with
 * pssc_guc_extra_alloc() (guc.c frees it, and only that block). The
 * assign_hook only installs the pointer and, if the parsed value differs from
 * the current one, bumps the backend-local config generation; it must not
 * fail, because GUC also calls it during transaction abort.
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "compat.h"
#include "guc.h"
#include "pairs.h"
#include "regex_runtime.h"
#include "scan.h"

/* Bounds (DESIGN.md §4.1). */
#define MAX_ENTRIES_MIN			100
#define MAX_ENTRIES_MAX			(INT_MAX / 2)
#define BUCKET_COUNT_MAX		10000
#define BUCKET_INTERVAL_MAX		86400	/* one day, in seconds */
#define MAX_TAGS_MAX			64
#define MAX_TAG_VALUE_LEN_MAX	4096

/*
 * The smallest tag set must hold one tag with a maximum-length key and a
 * value of the same length: "k\0v\0" = 63 + 1 + 63 + 1 bytes (§5.1).
 */
#define MAX_TAGSET_BYTES_MIN	(2 * (PSSC_MAX_KEY_LEN + 1))
#define MAX_TAGSET_BYTES_MAX	8192
#define SCAN_WINDOW_MIN			64
#define SCAN_WINDOW_MAX			(1024 * 1024)

/* ASCII whitespace, as trimmed by the pair parsers (§4.2). */
#define IS_ASCII_SPACE(c) \
	((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r' || \
	 (c) == '\f' || (c) == '\v')

bool		pssc_enabled = true;
int			pssc_track = PSSC_TRACK_TOP;
bool		pssc_track_utility = true;
int			pssc_nested_tags = PSSC_NESTED_INHERIT;
int			pssc_max_entries = 10000;
int			pssc_bucket_count = 12;
int			pssc_bucket_interval = 300;
int			pssc_max_tags = 8;
int			pssc_max_tag_value_len = 64;
int			pssc_max_tagset_bytes = 512;
int			pssc_scan_window = 2048;
char	   *pssc_extractors = NULL;
char	   *pssc_tags = NULL;
char	   *pssc_exclude_tags = NULL;
int			pssc_untagged = PSSC_UNTAGGED_SKIP;
char	   *pssc_normalize = NULL;
char	   *pssc_tags_override = NULL;

static const struct config_enum_entry track_options[] = {
	{"none", PSSC_TRACK_NONE, false},
	{"top", PSSC_TRACK_TOP, false},
	{"all", PSSC_TRACK_ALL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry nested_tags_options[] = {
	{"inherit", PSSC_NESTED_INHERIT, false},
	{"scan", PSSC_NESTED_SCAN, false},
	{"none", PSSC_NESTED_NONE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry untagged_options[] = {
	{"skip", PSSC_UNTAGGED_SKIP, false},
	{"record", PSSC_UNTAGGED_RECORD, false},
	{NULL, 0, false}
};

/*
 * Flat tag key list. keys[i] locates key i as a byte offset from the start
 * of the blob; the key bytes are followed by a NUL.
 */
typedef struct PsscTagListKey
{
	uint32		off;
	uint32		len;
} PsscTagListKey;

struct PsscTagList
{
	uint32		nkeys;
	uint32		nkeys_alloc;	/* key slots; key bytes start after them */
	bool		match_all;
	PsscTagListKey keys[FLEXIBLE_ARRAY_MEMBER];
};

static const PsscTagList empty_tag_list = {0, 0, false};

static const PsscTagList *cur_tags = &empty_tag_list;
static const PsscTagList *cur_exclude_tags = &empty_tag_list;

static const PsscExtractorList empty_extractor_list = {
	offsetof(PsscExtractorList, extractors), 0
};

/* NULL until the first assignment, so that one always bumps the generation. */
static const PsscExtractorList *cur_extractors = NULL;

static const PsscNormalizeList empty_normalize_list = {
	offsetof(PsscNormalizeList, rules), 0
};

static const PsscNormalizeList *cur_normalize = &empty_normalize_list;

static uint64 config_generation = 0;

static const PsscOverrideList *cur_override = NULL;
static uint64 override_generation = 0;

/* ---------------- tag list accessors ---------------- */

bool
pssc_tag_list_match_all(const PsscTagList *list)
{
	return list->match_all;
}

int
pssc_tag_list_count(const PsscTagList *list)
{
	return (int) list->nkeys;
}

const char *
pssc_tag_list_key(const PsscTagList *list, int i, int *len)
{
	Assert(i >= 0 && (uint32) i < list->nkeys);
	if (len)
		*len = (int) list->keys[i].len;
	return (const char *) list + list->keys[i].off;
}

int
pssc_tag_list_find(const PsscTagList *list, const char *key, int len)
{
	for (uint32 i = 0; i < list->nkeys; i++)
	{
		if (list->keys[i].len == (uint32) len &&
			memcmp((const char *) list + list->keys[i].off, key, len) == 0)
			return (int) i;
	}
	return -1;
}

const PsscTagList *
pssc_guc_tags(void)
{
	return cur_tags;
}

const PsscTagList *
pssc_guc_exclude_tags(void)
{
	return cur_exclude_tags;
}

const PsscExtractorList *
pssc_guc_extractors(void)
{
	return cur_extractors ? cur_extractors : &empty_extractor_list;
}

const PsscNormalizeList *
pssc_guc_normalize(void)
{
	return cur_normalize;
}

uint64
pssc_guc_config_generation(void)
{
	return config_generation;
}

const PsscOverrideList *
pssc_guc_override(void)
{
	return cur_override;
}

uint64
pssc_guc_override_generation(void)
{
	return override_generation;
}

static bool
tag_list_equal(const PsscTagList *a, const PsscTagList *b)
{
	if (a->match_all != b->match_all || a->nkeys != b->nkeys)
		return false;
	for (uint32 i = 0; i < a->nkeys; i++)
	{
		if (a->keys[i].len != b->keys[i].len ||
			memcmp((const char *) a + a->keys[i].off,
				   (const char *) b + b->keys[i].off, a->keys[i].len) != 0)
			return false;
	}
	return true;
}

/* ---------------- tag list parsing (check hooks) ---------------- */

bool
pssc_tag_list_blob_size(size_t nkeys, size_t keybytes, size_t *size)
{
	/* Both checks come first, so nothing below can overflow. */
	if (nkeys > PSSC_MAX_TAG_LIST_ENTRIES)
		return false;
	if (keybytes > nkeys * (PSSC_MAX_KEY_LEN + 1))
		return false;
	*size = offsetof(PsscTagList, keys) + nkeys * sizeof(PsscTagListKey) +
		keybytes;
	return true;
}

/*
 * One pass over a comma-separated tag key list.
 *
 * Whitespace around each key is trimmed; a value that is empty or only
 * whitespace is the empty list. Otherwise the list has at most
 * PSSC_MAX_TAG_LIST_ENTRIES entries, and every entry must be a key of
 * 1..PSSC_MAX_KEY_LEN bytes without whitespace or '*'. With allow_star, the
 * single entry '*' means "all keys".
 *
 * With list == NULL, only validates: on error, sets the GUC error detail and
 * returns false; otherwise stores in *nkeys and *keybytes the number of keys
 * and their total size including one NUL each (duplicates included), and in
 * *star whether the list is '*'. With a list (sized from those), appends the
 * keys, skipping later duplicates; the value must already have been
 * validated.
 */
static bool
tag_list_pass(const char *value, bool allow_star, PsscTagList *list,
			  size_t *nkeys, size_t *keybytes, bool *star)
{
	const char *p;
	bool		blank = true;
	size_t		n = 1;
	size_t		dpos = 0;

	for (p = value; *p; p++)
	{
		if (*p == ',' && ++n > PSSC_MAX_TAG_LIST_ENTRIES)
		{
			GUC_check_errdetail("The list has more than %d entries.",
								PSSC_MAX_TAG_LIST_ENTRIES);
			return false;
		}
		if (!IS_ASCII_SPACE(*p))
			blank = false;
	}
	if (blank)
		n = 0;

	if (list != NULL)
		dpos = offsetof(PsscTagList, keys) + list->nkeys_alloc * sizeof(PsscTagListKey);
	else
	{
		*nkeys = 0;
		*keybytes = 0;
		*star = false;
	}

	p = value;
	for (size_t seg = 0; seg < n; seg++)
	{
		const char *start = p;
		const char *end;
		size_t		klen;

		while (*p && *p != ',')
			p++;
		end = p;
		if (*p == ',')
			p++;

		while (start < end && IS_ASCII_SPACE(*start))
			start++;
		while (end > start && IS_ASCII_SPACE(end[-1]))
			end--;
		klen = end - start;

		if (list != NULL)
		{
			if ((klen == 1 && *start == '*') ||
				pssc_tag_list_find(list, start, (int) klen) >= 0)
				continue;
			list->keys[list->nkeys].off = (uint32) dpos;
			list->keys[list->nkeys].len = (uint32) klen;
			list->nkeys++;
			memcpy((char *) list + dpos, start, klen);
			dpos += klen + 1;	/* the NUL is already there */
			continue;
		}

		if (klen == 0)
		{
			GUC_check_errdetail("The list contains an empty tag key.");
			return false;
		}
		if (klen == 1 && *start == '*')
		{
			if (!allow_star)
			{
				GUC_check_errdetail("\"*\" is only allowed in %s.tags.",
									PSSC_GUC_PREFIX);
				return false;
			}
			if (n != 1)
			{
				GUC_check_errdetail("\"*\" must be the only entry in the list.");
				return false;
			}
			*star = true;
			continue;
		}
		if (klen > PSSC_MAX_KEY_LEN)
		{
			/* Clip before casting: klen is not bounded by INT_MAX. */
			int			shown = pg_mbcliplen(start,
											 (int) Min(klen, (size_t) PSSC_MAX_KEY_LEN + MAX_MULTIBYTE_CHAR_LEN),
											 PSSC_MAX_KEY_LEN);

			GUC_check_errdetail("Tag key \"%.*s...\" is longer than %d bytes.",
								shown, start, PSSC_MAX_KEY_LEN);
			return false;
		}
		for (const char *c = start; c < end; c++)
		{
			if (IS_ASCII_SPACE(*c))
			{
				GUC_check_errdetail("Tag key \"%.*s\" contains whitespace.",
									(int) klen, start);
				return false;
			}
			if (*c == '*')
			{
				GUC_check_errdetail("Tag key \"%.*s\" contains \"*\".",
									(int) klen, start);
				return false;
			}
		}
		(*nkeys)++;
		*keybytes += klen + 1;
	}
	return true;
}

/*
 * Validate a tag key list and build its PsscTagList blob (keys in list order,
 * later duplicates dropped). On error, sets the GUC error detail and returns
 * false without allocating anything.
 */
static bool
parse_tag_list(const char *value, bool allow_star, void **extra)
{
	size_t		nkeys;
	size_t		keybytes;
	bool		star;
	size_t		size;
	PsscTagList *list;

	if (!tag_list_pass(value, allow_star, NULL, &nkeys, &keybytes, &star))
		return false;

	/* Cannot fail after a successful validation pass; checked anyway. */
	if (!pssc_tag_list_blob_size(nkeys, keybytes, &size) || size > MaxAllocSize)
	{
		GUC_check_errdetail("The list is too long.");
		return false;
	}

	list = pssc_guc_extra_alloc(size);
	if (list == NULL)
	{
		GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
		GUC_check_errdetail("Out of memory.");
		return false;
	}
	memset(list, 0, size);
	list->match_all = star;
	list->nkeys_alloc = (uint32) nkeys;
	(void) tag_list_pass(value, allow_star, list, &nkeys, &keybytes, &star);
	Assert(list->nkeys <= list->nkeys_alloc);

	*extra = list;
	return true;
}

static bool
check_tags(char **newval, void **extra, GucSource source)
{
	return parse_tag_list(*newval ? *newval : "", true, extra);
}

static bool
check_exclude_tags(char **newval, void **extra, GucSource source)
{
	return parse_tag_list(*newval ? *newval : "", false, extra);
}

/* ---------------- assign hooks: install and bump; must not fail ---------------- */

static void
install_tag_list(const PsscTagList **cur, void *extra)
{
	const PsscTagList *list = extra ? (const PsscTagList *) extra : &empty_tag_list;

	if (!tag_list_equal(*cur, list))
		config_generation++;
	*cur = list;
}

static void
assign_tags(const char *newval, void *extra)
{
	install_tag_list(&cur_tags, extra);
}

static void
assign_exclude_tags(const char *newval, void *extra)
{
	install_tag_list(&cur_exclude_tags, extra);
}

/* ---------------- extractors DSL (§4.2) ---------------- */

/*
 *	list		:= [ extractor { ',' extractor } ]
 *	extractor	:= name [ '(' param { ',' param } ')' ]
 *	param		:= key '=' value
 *	value		:= quoted | unquoted
 *
 * ASCII whitespace may surround every token. Names, parameter keys and
 * keyword values (position, Booleans) are case-insensitive. A quoted value is
 * '...' with '' standing for one quote and is taken verbatim (separators may
 * be or contain whitespace). An unquoted value runs up to whitespace, ',' or
 * ')' and must not contain a quote or '('. Empty values are rejected.
 *
 * appname(format=F, ...) reads application_name instead of comments: it
 * takes format F's parameters (sqlcommenter: url_decode; marginalia: kv_sep,
 * pair_sep; regex: pattern, required keys) plus keys/rename/merge, but no
 * position, and is serialized as an extractor of kind F with source
 * PSSC_SOURCE_APPNAME.
 *
 * The check_hook parses into palloc'd DslExtractors in a private memory
 * context, validates them (test-compiling regexes), then serializes them
 * into one PsscExtractorList blob (guc.h) and deletes the context.
 */

/* Longest value text quoted in an error detail. */
#define DSL_SHOW_MAX	64

typedef struct DslStr
{
	const char *s;				/* not NUL-terminated */
	size_t		len;			/* 0: absent */
} DslStr;

typedef struct DslRename
{
	DslStr		from;
	DslStr		to;
} DslRename;

typedef enum DslParam
{
	DSL_POSITION,
	DSL_KEYS,
	DSL_RENAME,
	DSL_MERGE,
	DSL_URL_DECODE,
	DSL_KV_SEP,
	DSL_PAIR_SEP,
	DSL_PATTERN,
	DSL_FORMAT,
	DSL_NPARAMS
} DslParam;

#define KIND_BIT(k) (1u << (k))
#define ALL_KINDS	(KIND_BIT(PSSC_EXTRACTOR_SQLCOMMENTER) | \
					 KIND_BIT(PSSC_EXTRACTOR_MARGINALIA) | \
					 KIND_BIT(PSSC_EXTRACTOR_REGEX))
/*
 * appname(format=...): takes every parameter of its format but position,
 * plus format. Which format-specific parameters apply is only known once
 * format is, so dsl_finish() checks them (dsl_params[].kinds).
 */
#define APPNAME_BIT (1u << 8)

static const struct
{
	const char *name;
	uint32		kinds;			/* KIND_BITs of the extractors that take it */
}			dsl_params[DSL_NPARAMS] = {
	[DSL_POSITION] = {"position", ALL_KINDS},
	[DSL_KEYS] = {"keys", ALL_KINDS | APPNAME_BIT},
	[DSL_RENAME] = {"rename", ALL_KINDS | APPNAME_BIT},
	[DSL_MERGE] = {"merge", ALL_KINDS | APPNAME_BIT},
	[DSL_URL_DECODE] = {"url_decode", KIND_BIT(PSSC_EXTRACTOR_SQLCOMMENTER) | APPNAME_BIT},
	[DSL_KV_SEP] = {"kv_sep", KIND_BIT(PSSC_EXTRACTOR_MARGINALIA) | APPNAME_BIT},
	[DSL_PAIR_SEP] = {"pair_sep", KIND_BIT(PSSC_EXTRACTOR_MARGINALIA) | APPNAME_BIT},
	[DSL_PATTERN] = {"pattern", KIND_BIT(PSSC_EXTRACTOR_REGEX) | APPNAME_BIT},
	[DSL_FORMAT] = {"format", APPNAME_BIT},
};

static const char *const dsl_kind_names[] = {
	[PSSC_EXTRACTOR_SQLCOMMENTER] = "sqlcommenter",
	[PSSC_EXTRACTOR_MARGINALIA] = "marginalia",
	[PSSC_EXTRACTOR_REGEX] = "regex",
};

typedef struct DslExtractor
{
	PsscExtractorKind kind;		/* appname: its format, once given */
	bool		appname;		/* appname(format=kind) */
	const char *name;			/* canonical, for messages */
	uint32		given;			/* bit per DslParam */
	PsscPosition position;
	bool		merge;
	bool		url_decode;
	int			nkeys;
	DslStr	   *keys;
	int			nrename;
	DslRename  *rename;
	DslStr		kv_sep;
	DslStr		pair_sep;
	DslStr		pattern;
} DslExtractor;

/* Value text for an error detail, clipped to DSL_SHOW_MAX bytes. */
static char *
dsl_show(const char *s, size_t len)
{
	int			clip;

	if (len <= DSL_SHOW_MAX)
		return pnstrdup(s, len);
	clip = pg_mbcliplen(s, (int) Min(len, (size_t) DSL_SHOW_MAX + MAX_MULTIBYTE_CHAR_LEN),
						DSL_SHOW_MAX);
	return psprintf("%.*s...", clip, s);
}

/* Length of the character at p (p non-empty, NUL-terminated). */
static int
dsl_charlen(const char *p)
{
	return (int) Min((size_t) pg_mblen(p), strlen(p));
}

static bool
dsl_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool
dsl_ident_char(char c)
{
	return dsl_ident_start(c) || (c >= '0' && c <= '9');
}

static const char *
dsl_skip_ws(const char *p)
{
	while (IS_ASCII_SPACE(*p))
		p++;
	return p;
}

static bool
dsl_word_eq(const char *s, size_t len, const char *word)
{
	return strlen(word) == len && pg_strncasecmp(s, word, len) == 0;
}

static DslStr
dsl_trim(const char *s, size_t len)
{
	DslStr		r;

	while (len > 0 && IS_ASCII_SPACE(*s))
		s++, len--;
	while (len > 0 && IS_ASCII_SPACE(s[len - 1]))
		len--;
	r.s = s;
	r.len = len;
	return r;
}

/*
 * Validate a (trimmed) tag key named in parameter param: 1..PSSC_MAX_KEY_LEN
 * bytes, no whitespace.
 */
static bool
dsl_check_key(const DslExtractor *e, const char *param, DslStr k)
{
	if (k.len == 0)
	{
		GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" contains an empty key.",
							param, e->name);
		return false;
	}
	if (k.len > PSSC_MAX_KEY_LEN)
	{
		int			shown = pg_mbcliplen(k.s,
										 (int) Min(k.len, (size_t) PSSC_MAX_KEY_LEN + MAX_MULTIBYTE_CHAR_LEN),
										 PSSC_MAX_KEY_LEN);

		GUC_check_errdetail("Key \"%.*s...\" in parameter \"%s\" of extractor \"%s\" is longer than %d bytes.",
							shown, k.s, param, e->name, PSSC_MAX_KEY_LEN);
		return false;
	}
	for (size_t i = 0; i < k.len; i++)
	{
		if (IS_ASCII_SPACE(k.s[i]))
		{
			GUC_check_errdetail("Key \"%.*s\" in parameter \"%s\" of extractor \"%s\" contains whitespace.",
								(int) k.len, k.s, param, e->name);
			return false;
		}
	}
	return true;
}

/*
 * Split v on '|' into *nitems trimmed items (palloc'd array). Fails if there
 * are more than PSSC_MAX_TAG_LIST_ENTRIES.
 */
static bool
dsl_split_bar(const DslExtractor *e, const char *param, DslStr v,
			  DslStr **items, int *nitems)
{
	int			n = 1;
	int			i = 0;
	size_t		start = 0;

	for (size_t k = 0; k < v.len; k++)
	{
		if (v.s[k] == '|' && ++n > PSSC_MAX_TAG_LIST_ENTRIES)
		{
			GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" has more than %d entries.",
								param, e->name, PSSC_MAX_TAG_LIST_ENTRIES);
			return false;
		}
	}
	*items = palloc(sizeof(DslStr) * n);
	for (size_t k = 0; k <= v.len; k++)
	{
		if (k == v.len || v.s[k] == '|')
		{
			(*items)[i++] = dsl_trim(v.s + start, k - start);
			start = k + 1;
		}
	}
	Assert(i == n);
	*nitems = n;
	return true;
}

static bool
dsl_bool(const DslExtractor *e, DslParam param, DslStr v, bool *result)
{
	if (!parse_bool_with_len(v.s, v.len, result))
	{
		GUC_check_errdetail("Invalid value \"%s\" for parameter \"%s\" of extractor \"%s\": expected a Boolean value.",
							dsl_show(v.s, v.len), dsl_params[param].name, e->name);
		return false;
	}
	return true;
}

static bool
dsl_separator(const DslExtractor *e, DslParam param, DslStr v, DslStr *out)
{
	if (v.len > PSSC_MAX_SEP_LEN)
	{
		GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" is longer than %d bytes.",
							dsl_params[param].name, e->name, PSSC_MAX_SEP_LEN);
		return false;
	}
	*out = v;
	return true;
}

/* Apply one param = value (value non-empty). */
static bool
dsl_apply(DslExtractor *e, DslParam param, DslStr v)
{
	switch (param)
	{
		case DSL_POSITION:
			if (dsl_word_eq(v.s, v.len, "append"))
				e->position = PSSC_POS_APPEND;
			else if (dsl_word_eq(v.s, v.len, "prepend"))
				e->position = PSSC_POS_PREPEND;
			else if (dsl_word_eq(v.s, v.len, "any"))
				e->position = PSSC_POS_ANY;
			else
			{
				GUC_check_errdetail("Invalid value \"%s\" for parameter \"%s\" of extractor \"%s\": expected append, prepend or any.",
									dsl_show(v.s, v.len), dsl_params[param].name, e->name);
				return false;
			}
			return true;
		case DSL_MERGE:
			return dsl_bool(e, param, v, &e->merge);
		case DSL_URL_DECODE:
			return dsl_bool(e, param, v, &e->url_decode);
		case DSL_KEYS:
			if (!dsl_split_bar(e, "keys", v, &e->keys, &e->nkeys))
				return false;
			for (int i = 0; i < e->nkeys; i++)
				if (!dsl_check_key(e, "keys", e->keys[i]))
					return false;
			return true;
		case DSL_RENAME:
			{
				DslStr	   *items;

				if (!dsl_split_bar(e, "rename", v, &items, &e->nrename))
					return false;
				e->rename = palloc(sizeof(DslRename) * e->nrename);
				for (int i = 0; i < e->nrename; i++)
				{
					DslStr		it = items[i];
					const char *colon = memchr(it.s, ':', it.len);

					if (it.len == 0)
						return dsl_check_key(e, "rename", it);
					if (colon == NULL ||
						memchr(colon + 1, ':', it.len - (colon + 1 - it.s)) != NULL)
					{
						GUC_check_errdetail("Entry \"%s\" in parameter \"rename\" of extractor \"%s\" is not of the form old:new.",
											dsl_show(it.s, it.len), e->name);
						return false;
					}
					e->rename[i].from = dsl_trim(it.s, colon - it.s);
					e->rename[i].to = dsl_trim(colon + 1, it.len - (colon + 1 - it.s));
					if (!dsl_check_key(e, "rename", e->rename[i].from) ||
						!dsl_check_key(e, "rename", e->rename[i].to))
						return false;
					for (int j = 0; j < i; j++)
					{
						if (e->rename[j].from.len == e->rename[i].from.len &&
							memcmp(e->rename[j].from.s, e->rename[i].from.s,
								   e->rename[i].from.len) == 0)
						{
							GUC_check_errdetail("Key \"%.*s\" is renamed more than once in parameter \"rename\" of extractor \"%s\".",
												(int) e->rename[i].from.len,
												e->rename[i].from.s, e->name);
							return false;
						}
					}
				}
				return true;
			}
		case DSL_KV_SEP:
			return dsl_separator(e, param, v, &e->kv_sep);
		case DSL_PAIR_SEP:
			return dsl_separator(e, param, v, &e->pair_sep);
		case DSL_PATTERN:
			e->pattern = v;
			return true;
		case DSL_FORMAT:
			for (int k = 0; k < (int) lengthof(dsl_kind_names); k++)
			{
				if (dsl_word_eq(v.s, v.len, dsl_kind_names[k]))
				{
					e->kind = (PsscExtractorKind) k;
					return true;
				}
			}
			GUC_check_errdetail("Invalid value \"%s\" for parameter \"%s\" of extractor \"%s\": expected sqlcommenter, marginalia or regex.",
								dsl_show(v.s, v.len), dsl_params[param].name, e->name);
			return false;
		case DSL_NPARAMS:
			break;
	}
	Assert(false);
	return false;
}

/* Source of the value being checked (check_extractors(), check_normalize()). */
static GucSource regex_check_source = PGC_S_DEFAULT;

/*
 * Test-compile a regex extractor's pattern with the core engine, under the
 * compile time limit, and check the v1 limits (§4.2, §6.11). The compiled regex is freed right away;
 * backends compile their own copies lazily.
 */
static bool
dsl_check_regex(const DslExtractor *e, MemoryContext cxt)
{
	regex_t		re;
	pg_wchar   *wpat;
	int			wlen;
	int			rc;
	long		info;
	size_t		nsub;

	if (e->pattern.len > PSSC_MAX_REGEX_PATTERN_LEN)
	{
		GUC_check_errdetail("Pattern of extractor \"%s\" is longer than %d bytes.",
							e->name, PSSC_MAX_REGEX_PATTERN_LEN);
		return false;
	}
	if (!pg_verify_mbstr(GetDatabaseEncoding(), e->pattern.s, (int) e->pattern.len, true))
	{
		GUC_check_errdetail("Pattern of extractor \"%s\" is not valid in encoding \"%s\".",
							e->name, GetDatabaseEncodingName());
		return false;
	}
	wpat = palloc(sizeof(pg_wchar) * (e->pattern.len + 1));
	wlen = pg_mb2wchar_with_len(e->pattern.s, wpat, (int) e->pattern.len);

	rc = pssc_regex_check_compile(cxt, &re, wpat, wlen, regex_check_source,
								  psprintf("extractor \"%s\"", e->name));
	if (rc == PSSC_REGEX_COMPILE_UNCHECKED)
	{
		/* as nsub must be nkeys, the postmaster rejects it too */
		if (e->nkeys > pssc_max_tags)
		{
			GUC_check_errdetail("Extractor \"%s\" has %d keys, more than max_tags (%d).",
								e->name, e->nkeys, pssc_max_tags);
			return false;
		}
		return true;
	}
	if (rc == PSSC_REGEX_COMPILE_TOO_SLOW)
	{
		GUC_check_errdetail("Compiling the pattern of extractor \"%s\" took longer than %d ms.",
							e->name, pssc_regex_compile_limit_ms);
		return false;
	}
	if (rc != REG_OKAY)
	{
		char		msg[128];

		pg_regerror(rc, &re, msg, sizeof(msg));
		GUC_check_errdetail("Pattern of extractor \"%s\" is invalid: %s.", e->name, msg);
		return false;
	}
	info = re.re_info;
	nsub = re.re_nsub;
	pssc_regfree(&re);

	if (info & REG_UBACKREF)
	{
		GUC_check_errdetail("Pattern of extractor \"%s\" uses back-references, which are not allowed.",
							e->name);
		return false;
	}
	if (nsub > (size_t) pssc_max_tags)
	{
		GUC_check_errdetail("Pattern of extractor \"%s\" has %zu capture groups, more than max_tags (%d).",
							e->name, nsub, pssc_max_tags);
		return false;
	}
	if ((size_t) e->nkeys != nsub)
	{
		GUC_check_errdetail("Extractor \"%s\" has %d %s but its pattern has %zu capture %s.",
							e->name, e->nkeys, e->nkeys == 1 ? "key" : "keys",
							nsub, nsub == 1 ? "group" : "groups");
		return false;
	}
	return true;
}

/* Defaults and cross-parameter checks, once all parameters are known. */
static bool
dsl_finish(DslExtractor *e, MemoryContext cxt)
{
	if (e->appname)
	{
		if (!(e->given & (1u << DSL_FORMAT)))
		{
			GUC_check_errdetail("Extractor \"%s\" requires parameter \"format\".", e->name);
			return false;
		}
		for (int p = 0; p < DSL_NPARAMS; p++)
		{
			if ((e->given & (1u << p)) && p != DSL_FORMAT &&
				!(dsl_params[p].kinds & KIND_BIT(e->kind)))
			{
				GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" is not allowed with format=%s.",
									dsl_params[p].name, e->name, dsl_kind_names[e->kind]);
				return false;
			}
		}
		/* the format's defaults; no position (stored as any) */
		if (!(e->given & (1u << DSL_URL_DECODE)))
			e->url_decode = e->kind == PSSC_EXTRACTOR_SQLCOMMENTER;
		e->position = PSSC_POS_ANY;
	}
	switch (e->kind)
	{
		case PSSC_EXTRACTOR_SQLCOMMENTER:
			break;
		case PSSC_EXTRACTOR_MARGINALIA:
			if (e->kv_sep.len == 0)
				e->kv_sep = (DslStr) {":", 1};
			if (e->pair_sep.len == 0)
				e->pair_sep = (DslStr) {",", 1};

			/*
			 * pairs.c splits on pair_sep first, and a kv_sep match that
			 * contains the start of a pair_sep does not count, so a kv_sep
			 * containing pair_sep can never match (every pair would be
			 * malformed). This includes kv_sep = pair_sep.
			 */
			for (size_t i = 0; i + e->pair_sep.len <= e->kv_sep.len; i++)
			{
				if (memcmp(e->kv_sep.s + i, e->pair_sep.s, e->pair_sep.len) == 0)
				{
					GUC_check_errdetail("Parameter \"kv_sep\" (\"%.*s\") of extractor \"%s\" contains its pair_sep (\"%.*s\"), so it can never match.",
										(int) e->kv_sep.len, e->kv_sep.s, e->name,
										(int) e->pair_sep.len, e->pair_sep.s);
					return false;
				}
			}
			break;
		case PSSC_EXTRACTOR_REGEX:
			if (e->pattern.len == 0)
			{
				GUC_check_errdetail("Extractor \"%s\" requires parameter \"pattern\".", e->name);
				return false;
			}
			if (!(e->given & (1u << DSL_KEYS)))
			{
				GUC_check_errdetail("Extractor \"%s\" requires parameter \"keys\".", e->name);
				return false;
			}
			return dsl_check_regex(e, cxt);
	}
	return true;
}

/*
 * Parse one value at *pp (just after '=' and whitespace) into *v. Quoted
 * values are unescaped into a palloc'd copy. On success *pp is at the next
 * non-whitespace character, which is ',', ')' or the end.
 */
static bool
dsl_value(const DslExtractor *e, DslParam param, const char **pp, DslStr *v)
{
	const char *p = *pp;
	const char *pname = dsl_params[param].name;

	if (*p == '\'')
	{
		char	   *buf = palloc(strlen(p) + 1);
		size_t		n = 0;

		for (p++;; p++)
		{
			if (*p == '\0')
			{
				GUC_check_errdetail("Unterminated quoted value for parameter \"%s\" of extractor \"%s\".",
									pname, e->name);
				return false;
			}
			if (*p == '\'')
			{
				if (p[1] != '\'')
					break;
				p++;
			}
			buf[n++] = *p;
		}
		p = dsl_skip_ws(p + 1);
		if (*p != '\0' && *p != ',' && *p != ')')
		{
			GUC_check_errdetail("Unexpected \"%.*s\" after the quoted value of parameter \"%s\" of extractor \"%s\".",
								dsl_charlen(p), p, pname, e->name);
			return false;
		}
		v->s = buf;
		v->len = n;
	}
	else
	{
		const char *start = p;

		for (; *p != '\0' && *p != ',' && *p != ')' && !IS_ASCII_SPACE(*p); p++)
		{
			if (*p == '\'' || *p == '(')
			{
				if (*p == '\'')
					GUC_check_errdetail("Value of parameter \"%s\" of extractor \"%s\" contains a quote; quote the whole value.",
										pname, e->name);
				else
					GUC_check_errdetail("Value of parameter \"%s\" of extractor \"%s\" contains \"(\"; quote the whole value.",
										pname, e->name);
				return false;
			}
		}
		v->s = start;
		v->len = p - start;
		p = dsl_skip_ws(p);
		if (*p != '\0' && *p != ',' && *p != ')')
		{
			GUC_check_errdetail("Unexpected \"%.*s\" after the value of parameter \"%s\" of extractor \"%s\".",
								dsl_charlen(p), p, pname, e->name);
			return false;
		}
	}
	if (v->len == 0)
	{
		GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" has an empty value.",
							pname, e->name);
		return false;
	}
	*pp = p;
	return true;
}

/* Parse "( param, ... )" at *pp (at the '('). */
static bool
dsl_params_list(DslExtractor *e, const char **pp)
{
	const char *p = *pp + 1;
	const char *after = "(";

	for (;;)
	{
		const char *id;
		size_t		idlen;
		int			param;
		DslStr		v;

		p = dsl_skip_ws(p);
		if (!dsl_ident_start(*p))
		{
			GUC_check_errdetail("Expected a parameter name after \"%s\" of extractor \"%s\".",
								after, e->name);
			return false;
		}
		for (id = p; dsl_ident_char(*p); p++)
			;
		idlen = p - id;
		for (param = 0; param < DSL_NPARAMS; param++)
			if (dsl_word_eq(id, idlen, dsl_params[param].name))
				break;
		if (param == DSL_NPARAMS ||
			!(dsl_params[param].kinds & (e->appname ? APPNAME_BIT : KIND_BIT(e->kind))))
		{
			GUC_check_errdetail("Unknown parameter \"%.*s\" for extractor \"%s\".",
								(int) idlen, id, e->name);
			return false;
		}
		if (e->given & (1u << param))
		{
			GUC_check_errdetail("Parameter \"%s\" of extractor \"%s\" is given more than once.",
								dsl_params[param].name, e->name);
			return false;
		}
		e->given |= 1u << param;

		p = dsl_skip_ws(p);
		if (*p != '=')
		{
			GUC_check_errdetail("Expected \"=\" after parameter \"%s\" of extractor \"%s\".",
								dsl_params[param].name, e->name);
			return false;
		}
		p = dsl_skip_ws(p + 1);
		if (!dsl_value(e, param, &p, &v) || !dsl_apply(e, param, v))
			return false;

		if (*p == ',')
		{
			p++;
			after = ",";
			continue;
		}
		if (*p == ')')
			break;
		GUC_check_errdetail("Missing \")\" after the parameters of extractor \"%s\".", e->name);
		return false;
	}
	*pp = p + 1;
	return true;
}

/* Parse and validate the whole value into ext[0 .. *n - 1]. */
static bool
dsl_parse(const char *value, DslExtractor *ext, int *n, MemoryContext cxt)
{
	const char *p = dsl_skip_ws(value);

	*n = 0;
	if (*p == '\0')
		return true;
	for (;;)
	{
		DslExtractor *e;
		const char *id;
		size_t		idlen;
		int			kind;
		bool		appname = false;

		if (*p == ',' || *p == '\0')
		{
			GUC_check_errdetail("Empty entry in the extractor list.");
			return false;
		}
		if (!dsl_ident_start(*p))
		{
			GUC_check_errdetail("Expected an extractor name at \"%s\".", dsl_show(p, strlen(p)));
			return false;
		}
		for (id = p; dsl_ident_char(*p); p++)
			;
		idlen = p - id;
		for (kind = 0; kind < (int) lengthof(dsl_kind_names); kind++)
			if (dsl_word_eq(id, idlen, dsl_kind_names[kind]))
				break;
		if (kind == (int) lengthof(dsl_kind_names) && dsl_word_eq(id, idlen, "appname"))
		{
			appname = true;
			kind = PSSC_EXTRACTOR_SQLCOMMENTER; /* until format is given */
		}
		if (kind == (int) lengthof(dsl_kind_names))
		{
			GUC_check_errdetail("Unknown extractor \"%s\".", dsl_show(id, idlen));
			return false;
		}
		if (*n == PSSC_MAX_EXTRACTORS)
		{
			GUC_check_errdetail("The list has more than %d extractors.", PSSC_MAX_EXTRACTORS);
			return false;
		}

		e = &ext[(*n)++];
		memset(e, 0, sizeof(*e));
		e->kind = (PsscExtractorKind) kind;
		e->appname = appname;
		e->name = appname ? "appname" : dsl_kind_names[kind];
		e->position = kind == PSSC_EXTRACTOR_REGEX ? PSSC_POS_ANY : PSSC_POS_APPEND;
		e->url_decode = kind == PSSC_EXTRACTOR_SQLCOMMENTER;

		p = dsl_skip_ws(p);
		if (*p == '(')
		{
			if (!dsl_params_list(e, &p))
				return false;
			p = dsl_skip_ws(p);
		}
		if (!dsl_finish(e, cxt))
			return false;

		if (*p == '\0')
			return true;
		if (*p != ',')
		{
			GUC_check_errdetail("Unexpected \"%.*s\" after extractor \"%s\".",
								dsl_charlen(p), p, e->name);
			return false;
		}
		p = dsl_skip_ws(p + 1);
	}
}

/* Copy s into the blob at *dpos (NUL-terminated); returns its reference. */
static PsscBlobStr
blob_put(char *blob, size_t *dpos, DslStr s)
{
	PsscBlobStr r = {0, 0};

	if (s.len == 0)
		return r;
	r.off = (uint32) *dpos;
	r.len = (uint32) s.len;
	memcpy(blob + *dpos, s.s, s.len);
	*dpos += s.len + 1;			/* the NUL is already there */
	return r;
}

/*
 * Serialize ext[0 .. n - 1] into one zero-filled PsscExtractorList: header,
 * extractors, then each extractor's key and rename arrays, then all strings
 * in a fixed order. Returns NULL (detail set) on out-of-memory.
 */
static PsscExtractorList *
dsl_serialize(const DslExtractor *ext, int n)
{
	size_t		size = offsetof(PsscExtractorList, extractors) + n * sizeof(PsscExtractor);
	size_t		apos;
	size_t		dpos;
	PsscExtractorList *list;

	/* Bounded: at most 16 extractors with 1024 keys and renames of 63 bytes. */
	for (int i = 0; i < n; i++)
		size += ext[i].nkeys * sizeof(PsscBlobStr) + ext[i].nrename * sizeof(PsscBlobRename);
	dpos = size;
	for (int i = 0; i < n; i++)
	{
		const DslExtractor *e = &ext[i];

		size += (e->kv_sep.len ? e->kv_sep.len + 1 : 0) +
			(e->pair_sep.len ? e->pair_sep.len + 1 : 0) +
			(e->pattern.len ? e->pattern.len + 1 : 0);
		for (int k = 0; k < e->nkeys; k++)
			size += e->keys[k].len + 1;
		for (int k = 0; k < e->nrename; k++)
			size += e->rename[k].from.len + 1 + e->rename[k].to.len + 1;
	}
	Assert(size <= MaxAllocSize);

	list = pssc_guc_extra_alloc(size);
	if (list == NULL)
	{
		GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
		GUC_check_errdetail("Out of memory.");
		return NULL;
	}
	memset(list, 0, size);
	list->size = (uint32) size;
	list->nextractors = (uint32) n;
	apos = offsetof(PsscExtractorList, extractors) + n * sizeof(PsscExtractor);
	for (int i = 0; i < n; i++)
	{
		const DslExtractor *e = &ext[i];
		PsscExtractor *out = &list->extractors[i];
		PsscBlobStr *keys;
		PsscBlobRename *ren;

		out->kind = (uint8) e->kind;
		out->position = (uint8) e->position;
		out->merge = e->merge;
		out->url_decode = e->url_decode;
		out->has_keys = (e->given & (1u << DSL_KEYS)) != 0;
		out->source = (uint8) (e->appname ? PSSC_SOURCE_APPNAME : PSSC_SOURCE_COMMENT);
		out->nkeys = (uint32) e->nkeys;
		out->keys_off = (uint32) apos;
		apos += e->nkeys * sizeof(PsscBlobStr);
		out->nrename = (uint32) e->nrename;
		out->rename_off = (uint32) apos;
		apos += e->nrename * sizeof(PsscBlobRename);

		out->kv_sep = blob_put((char *) list, &dpos, e->kv_sep);
		out->pair_sep = blob_put((char *) list, &dpos, e->pair_sep);
		out->pattern = blob_put((char *) list, &dpos, e->pattern);
		keys = (PsscBlobStr *) ((char *) list + out->keys_off);
		for (int k = 0; k < e->nkeys; k++)
			keys[k] = blob_put((char *) list, &dpos, e->keys[k]);
		ren = (PsscBlobRename *) ((char *) list + out->rename_off);
		for (int k = 0; k < e->nrename; k++)
		{
			ren[k].from = blob_put((char *) list, &dpos, e->rename[k].from);
			ren[k].to = blob_put((char *) list, &dpos, e->rename[k].to);
		}
	}
	Assert(dpos == size);
	return list;
}

/*
 * extractors check_hook: parse and validate the DSL (§4.2) and return the
 * PsscExtractorList blob as extra. On error, sets the GUC error detail and
 * returns false; the previous config stays in effect.
 */
static bool
check_extractors(char **newval, void **extra, GucSource source)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	DslExtractor ext[PSSC_MAX_EXTRACTORS];
	int			n;
	PsscExtractorList *list = NULL;
	bool		ok;

	regex_check_source = source;
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"pg_stat_statement_context extractors check",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	ok = dsl_parse(*newval ? *newval : "", ext, &n, cxt);
	if (ok)
	{
		list = dsl_serialize(ext, n);
		ok = list != NULL;
	}
	MemoryContextSwitchTo(oldcxt);

	/* GUC_check_errdetail's text lives outside cxt (guc.c copies it). */
	MemoryContextDelete(cxt);

	if (ok)
		*extra = list;
	return ok;
}

static void
assign_extractors(const char *newval, void *extra)
{
	const PsscExtractorList *list =
		extra ? (const PsscExtractorList *) extra : &empty_extractor_list;

	if (cur_extractors == NULL || cur_extractors->size != list->size ||
		memcmp(cur_extractors, list, list->size) != 0)
		config_generation++;
	cur_extractors = list;
}

/* ---------------- normalize (DESIGN.md §6.11 step 6) ---------------- */

/*
 * Syntax: rule {"," rule}, rule = key ":" 'pattern' "=>" 'replacement', with
 * optional ASCII whitespace between tokens. Pattern and replacement are
 * always single-quoted ('' is a quote), so separators inside them need no
 * escaping; the key is bare (up to whitespace, ':', ',' or a quote) or
 * single-quoted too. A blank value has no rules.
 */
typedef struct NormRule
{
	DslStr		key;
	DslStr		pattern;
	DslStr		replacement;
	uint32		max_ref;
} NormRule;

/*
 * Parse a single-quoted string at *pp (at the quote) into a palloc'd copy;
 * *pp is left at the next non-whitespace character.
 */
static bool
norm_quoted(const char **pp, DslStr *out, int ruleno, const char *what)
{
	const char *p = *pp;
	char	   *buf = palloc(strlen(p) + 1);
	size_t		n = 0;

	Assert(*p == '\'');
	for (p++;; p++)
	{
		if (*p == '\0')
		{
			GUC_check_errdetail("Unterminated quoted %s in rule %d.", what, ruleno);
			return false;
		}
		if (*p == '\'')
		{
			if (p[1] != '\'')
				break;
			p++;
		}
		buf[n++] = *p;
	}
	buf[n] = '\0';
	out->s = buf;
	out->len = n;
	*pp = dsl_skip_ws(p + 1);
	return true;
}

static bool
norm_check_key(DslStr k, int ruleno)
{
	if (k.len == 0)
	{
		GUC_check_errdetail("Rule %d has an empty key.", ruleno);
		return false;
	}
	if (k.len > PSSC_MAX_KEY_LEN)
	{
		int			shown = pg_mbcliplen(k.s,
										 (int) Min(k.len, (size_t) PSSC_MAX_KEY_LEN + MAX_MULTIBYTE_CHAR_LEN),
										 PSSC_MAX_KEY_LEN);

		GUC_check_errdetail("Key \"%.*s...\" of rule %d is longer than %d bytes.",
							shown, k.s, ruleno, PSSC_MAX_KEY_LEN);
		return false;
	}
	for (size_t i = 0; i < k.len; i++)
	{
		if (IS_ASCII_SPACE(k.s[i]) || k.s[i] == '*')
		{
			GUC_check_errdetail("Key \"%s\" of rule %d contains %s.",
								dsl_show(k.s, k.len), ruleno,
								k.s[i] == '*' ? "\"*\"" : "whitespace");
			return false;
		}
	}
	return true;
}

/*
 * Validate the pattern (as for a regex extractor: length, encoding, compiles
 * as an advanced regex with the C collation within the compile time limit,
 * no back-references) and the
 * replacement (length, encoding, escapes \1..\9 within the pattern's
 * groups, \& and \\ only); sets r->max_ref.
 */
static bool
norm_check_rule(NormRule *r, int ruleno, MemoryContext cxt)
{
	regex_t		re;
	pg_wchar   *wpat;
	int			wlen;
	int			rc;
	long		info;
	size_t		nsub;
	int			enc = GetDatabaseEncoding();

	if (r->pattern.len == 0)
	{
		GUC_check_errdetail("Rule %d has an empty pattern.", ruleno);
		return false;
	}
	if (r->pattern.len > PSSC_MAX_REGEX_PATTERN_LEN)
	{
		GUC_check_errdetail("Pattern of rule %d is longer than %d bytes.",
							ruleno, PSSC_MAX_REGEX_PATTERN_LEN);
		return false;
	}
	if (r->replacement.len > PSSC_MAX_NORMALIZE_REPLACEMENT_LEN)
	{
		GUC_check_errdetail("Replacement of rule %d is longer than %d bytes.",
							ruleno, PSSC_MAX_NORMALIZE_REPLACEMENT_LEN);
		return false;
	}
	if (!pg_verify_mbstr(enc, r->pattern.s, (int) r->pattern.len, true))
	{
		GUC_check_errdetail("Pattern of rule %d is not valid in encoding \"%s\".",
							ruleno, GetDatabaseEncodingName());
		return false;
	}
	if (!pg_verify_mbstr(enc, r->replacement.s, (int) r->replacement.len, true))
	{
		GUC_check_errdetail("Replacement of rule %d is not valid in encoding \"%s\".",
							ruleno, GetDatabaseEncodingName());
		return false;
	}

	wpat = palloc(sizeof(pg_wchar) * (r->pattern.len + 1));
	wlen = pg_mb2wchar_with_len(r->pattern.s, wpat, (int) r->pattern.len);
	rc = pssc_regex_check_compile(cxt, &re, wpat, wlen, regex_check_source,
								  psprintf("normalize rule %d", ruleno));
	if (rc == PSSC_REGEX_COMPILE_TOO_SLOW)
	{
		GUC_check_errdetail("Compiling the pattern of rule %d took longer than %d ms.",
							ruleno, pssc_regex_compile_limit_ms);
		return false;
	}
	if (rc == PSSC_REGEX_COMPILE_UNCHECKED)
	{
		/* group references are checked by the lazy compile */
		info = 0;
		nsub = SIZE_MAX;
	}
	else if (rc != REG_OKAY)
	{
		char		msg[128];

		pg_regerror(rc, &re, msg, sizeof(msg));
		GUC_check_errdetail("Pattern of rule %d is invalid: %s.", ruleno, msg);
		return false;
	}
	else
	{
		info = re.re_info;
		nsub = re.re_nsub;
		pssc_regfree(&re);
	}
	if (info & REG_UBACKREF)
	{
		GUC_check_errdetail("Pattern of rule %d uses back-references, which are not allowed.",
							ruleno);
		return false;
	}

	r->max_ref = 0;
	for (size_t i = 0; i < r->replacement.len; i++)
	{
		char		c;

		if (r->replacement.s[i] != '\\')
			continue;
		if (i + 1 == r->replacement.len)
		{
			GUC_check_errdetail("Replacement of rule %d ends with a lone \"\\\".", ruleno);
			return false;
		}
		c = r->replacement.s[++i];
		if (c >= '1' && c <= '9')
		{
			if ((size_t) (c - '0') > nsub)
			{
				GUC_check_errdetail("Replacement of rule %d refers to group \\%c, but its pattern has %zu capture %s.",
									ruleno, c, nsub, nsub == 1 ? "group" : "groups");
				return false;
			}
			r->max_ref = Max(r->max_ref, (uint32) (c - '0'));
		}
		else if (c != '&' && c != '\\')
		{
			GUC_check_errdetail("Replacement of rule %d contains the unknown escape \"\\%.*s\"; only \\1 to \\9, \\& and \\\\ are allowed.",
								ruleno, (int) Min((size_t) pg_encoding_mblen(enc, r->replacement.s + i), r->replacement.len - i),
								r->replacement.s + i);
			return false;
		}
	}
	return true;
}

static bool
norm_parse(const char *value, NormRule *rules, int *n, MemoryContext cxt)
{
	const char *p = dsl_skip_ws(value);

	*n = 0;
	if (*p == '\0')
		return true;
	for (;;)
	{
		NormRule   *r;
		int			ruleno = *n + 1;

		if (*p == ',' || *p == '\0')
		{
			GUC_check_errdetail("Empty entry in the rule list.");
			return false;
		}
		if (*n == PSSC_MAX_NORMALIZE_RULES)
		{
			GUC_check_errdetail("The list has more than %d rules.", PSSC_MAX_NORMALIZE_RULES);
			return false;
		}
		r = &rules[(*n)++];
		memset(r, 0, sizeof(*r));

		if (*p == '\'')
		{
			if (!norm_quoted(&p, &r->key, ruleno, "key"))
				return false;
		}
		else
		{
			r->key.s = p;
			while (*p != '\0' && *p != ':' && *p != ',' && *p != '\'' && !IS_ASCII_SPACE(*p))
				p++;
			r->key.len = p - r->key.s;
			p = dsl_skip_ws(p);
		}
		if (!norm_check_key(r->key, ruleno))
			return false;
		if (*p != ':')
		{
			GUC_check_errdetail("Expected \":\" after the key of rule %d.", ruleno);
			return false;
		}
		p = dsl_skip_ws(p + 1);
		if (*p != '\'')
		{
			GUC_check_errdetail("Expected a quoted pattern after \"%s:\" in rule %d.",
								dsl_show(r->key.s, r->key.len), ruleno);
			return false;
		}
		if (!norm_quoted(&p, &r->pattern, ruleno, "pattern"))
			return false;
		if (p[0] != '=' || p[1] != '>')
		{
			GUC_check_errdetail("Expected \"=>\" after the pattern of rule %d.", ruleno);
			return false;
		}
		p = dsl_skip_ws(p + 2);
		if (*p != '\'')
		{
			GUC_check_errdetail("Expected a quoted replacement after \"=>\" in rule %d.", ruleno);
			return false;
		}
		if (!norm_quoted(&p, &r->replacement, ruleno, "replacement"))
			return false;
		if (!norm_check_rule(r, ruleno, cxt))
			return false;

		if (*p == '\0')
			return true;
		if (*p != ',')
		{
			GUC_check_errdetail("Unexpected \"%.*s\" after rule %d.", dsl_charlen(p), p, ruleno);
			return false;
		}
		p = dsl_skip_ws(p + 1);
	}
}

/* Serialize rules[0 .. n - 1] into one zero-filled PsscNormalizeList. */
static PsscNormalizeList *
norm_serialize(const NormRule *rules, int n)
{
	size_t		size = offsetof(PsscNormalizeList, rules) + n * sizeof(PsscNormalizeRule);
	size_t		dpos = size;
	PsscNormalizeList *list;

	/* Bounded: 32 rules of at most 63 + 1024 + 1024 bytes. */
	for (int i = 0; i < n; i++)
		size += rules[i].key.len + 1 + rules[i].pattern.len + 1 +
			(rules[i].replacement.len ? rules[i].replacement.len + 1 : 0);
	list = pssc_guc_extra_alloc(size);
	if (list == NULL)
	{
		GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
		GUC_check_errdetail("Out of memory.");
		return NULL;
	}
	memset(list, 0, size);
	list->size = (uint32) size;
	list->nrules = (uint32) n;
	for (int i = 0; i < n; i++)
	{
		list->rules[i].key = blob_put((char *) list, &dpos, rules[i].key);
		list->rules[i].pattern = blob_put((char *) list, &dpos, rules[i].pattern);
		list->rules[i].replacement = blob_put((char *) list, &dpos, rules[i].replacement);
		list->rules[i].max_ref = rules[i].max_ref;
	}
	Assert(dpos == size);
	return list;
}

/*
 * normalize check_hook: parse and validate the rules and return the
 * PsscNormalizeList blob as extra (NULL for no rules).
 */
static bool
check_normalize(char **newval, void **extra, GucSource source)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	NormRule	rules[PSSC_MAX_NORMALIZE_RULES];
	int			n;
	PsscNormalizeList *list = NULL;
	bool		ok;

	regex_check_source = source;
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"pg_stat_statement_context normalize check",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	ok = norm_parse(*newval ? *newval : "", rules, &n, cxt);
	if (ok && n > 0)
	{
		list = norm_serialize(rules, n);
		ok = list != NULL;
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	if (ok)
		*extra = list;
	return ok;
}

static void
assign_normalize(const char *newval, void *extra)
{
	const PsscNormalizeList *list =
		extra ? (const PsscNormalizeList *) extra : &empty_normalize_list;

	if (cur_normalize->size != list->size ||
		memcmp(cur_normalize, list, list->size) != 0)
		config_generation++;
	cur_normalize = list;
}

/* ---------------- tags_override ---------------- */

/*
 * tags_override check_hook: parse the value with the sqlcommenter parser
 * (URL-decoding on) and return the PsscOverrideList blob as extra (NULL for
 * no pair). Rejects what a comment's pipeline would silently drop, so a
 * mistake is reported at SET time: malformed segments, invalid %-escapes,
 * decoded NUL bytes, keys longer than 63 bytes and, for values set in this
 * session (SET, a function's SET clause, connection options), text invalid
 * in the database encoding. Values from other sources (configuration file,
 * ALTER ROLE/DATABASE SET) may be applied in databases with another
 * encoding; invalid pairs among them are dropped and counted per statement
 * like a comment's (invalid_tags).
 */
static bool
check_tags_override(char **newval, void **extra, GucSource source)
{
	const char *val = *newval ? *newval : "";
	size_t		len = strlen(val);
	MemoryContext cxt;
	MemoryContext oldcxt;
	PsscPairOut out;
	PsscPairResult res;
	PsscOverrideList *list = NULL;
	bool		ok = true;
	bool		verify = source == PGC_S_SESSION || source == PGC_S_CLIENT;

	*extra = NULL;
	if (len == 0)
		return true;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"pg_stat_statement_context tags_override check",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	/* pairs.h: len bytes and (len + 1) / 2 + 1 pairs always suffice */
	out.max_pairs = (len + 1) / 2 + 1;
	out.pairs = palloc(out.max_pairs * sizeof(PsscPair));
	out.bufsize = len;
	out.buf = palloc(len);
	memset(&res, 0, sizeof(res));
	pssc_parse_sqlcommenter(val, len, true, &out, &res);

	if (res.nmalformed > 0 || res.ndropped > 0)
	{
		GUC_check_errdetail("Expected comma-separated key='value' pairs, "
							"with URL-encoded keys and values.");
		ok = false;
	}
	for (size_t i = 0; ok && i < res.npairs; i++)
	{
		const PsscPair *p = &out.pairs[i];

		if (p->flags & PSSC_PAIR_BAD_ESCAPE)
		{
			GUC_check_errdetail("Pair %zu has an invalid %%-escape.", i + 1);
			ok = false;
		}
		else if ((p->flags & (PSSC_PAIR_KEY_NUL | PSSC_PAIR_VALUE_NUL)) ||
				 memchr(p->key, '\0', p->keylen) != NULL ||
				 (p->valuelen > 0 && memchr(p->value, '\0', p->valuelen) != NULL))
		{
			GUC_check_errdetail("Pair %zu contains a NUL byte.", i + 1);
			ok = false;
		}
		else if (p->keylen > PSSC_MAX_KEY_LEN)
		{
			GUC_check_errdetail("The key of pair %zu is longer than %d bytes.",
								i + 1, PSSC_MAX_KEY_LEN);
			ok = false;
		}
		else if (verify &&
				 (!pg_verify_mbstr(GetDatabaseEncoding(), p->key, (int) p->keylen, true) ||
				  !pg_verify_mbstr(GetDatabaseEncoding(), p->value, (int) p->valuelen, true)))
		{
			GUC_check_errdetail("Pair %zu is not valid in encoding \"%s\".",
								i + 1, GetDatabaseEncodingName());
			ok = false;
		}
	}

	if (ok && res.npairs > 0)
	{
		size_t		size = offsetof(PsscOverrideList, pairs) +
			res.npairs * sizeof(PsscOverridePair);
		size_t		dpos = size;

		for (size_t i = 0; i < res.npairs; i++)
			size += out.pairs[i].keylen + 1 +
				(out.pairs[i].valuelen ? out.pairs[i].valuelen + 1 : 0);
		/* at most about 2 * len: GUC strings are far below 4 GB */
		list = pssc_guc_extra_alloc(size);
		if (list == NULL)
		{
			GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
			GUC_check_errdetail("Out of memory.");
			ok = false;
		}
		else
		{
			memset(list, 0, size);
			list->size = (uint32) size;
			list->npairs = (uint32) res.npairs;
			for (size_t i = 0; i < res.npairs; i++)
			{
				DslStr		k = {out.pairs[i].key, out.pairs[i].keylen};
				DslStr		v = {out.pairs[i].value, out.pairs[i].valuelen};

				list->pairs[i].key = blob_put((char *) list, &dpos, k);
				list->pairs[i].value = blob_put((char *) list, &dpos, v);
			}
			Assert(dpos == size);
		}
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	if (ok)
		*extra = list;
	return ok;
}

static void
assign_tags_override(const char *newval, void *extra)
{
	const PsscOverrideList *list = (const PsscOverrideList *) extra;

	/* the old extra is still allocated during the assign hook */
	if ((list == NULL) != (cur_override == NULL) ||
		(list != NULL &&
		 (list->size != cur_override->size ||
		  memcmp(list, cur_override, list->size) != 0)))
		override_generation++;
	cur_override = list;
}

/* ---------------- definitions ---------------- */

void
pssc_guc_define(void)
{
	/* superuser */
	DefineCustomBoolVariable(PSSC_GUC_PREFIX ".enabled",
							 "Enables collection of statement statistics per tag set.",
							 NULL,
							 &pssc_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable(PSSC_GUC_PREFIX ".track",
							 "Selects which statements are tracked.",
							 "none: no statements; top: top-level statements; "
							 "all: also nested statements (as in pg_stat_statements).",
							 &pssc_track,
							 PSSC_TRACK_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable(PSSC_GUC_PREFIX ".track_utility",
							 "Selects whether utility commands are tracked.",
							 NULL,
							 &pssc_track_utility,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable(PSSC_GUC_PREFIX ".nested_tags",
							 "Selects which tags nested statements get.",
							 "inherit: the tags of the top-level statement; "
							 "scan: tags from the nested statement's own source text; "
							 "none: no tags.",
							 &pssc_nested_tags,
							 PSSC_NESTED_INHERIT,
							 nested_tags_options,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* postmaster: these size shared memory or fix bucket IDs */
	DefineCustomIntVariable(PSSC_GUC_PREFIX ".max_entries",
							"Sets the maximum number of tracked combinations of query and tag set.",
							"Each entry holds a ring of bucket_count time buckets, "
							"so this does not depend on bucket_count.",
							&pssc_max_entries,
							10000,
							MAX_ENTRIES_MIN,
							MAX_ENTRIES_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".bucket_count",
							"Sets the number of time buckets kept for each entry.",
							NULL,
							&pssc_bucket_count,
							12,
							1,
							BUCKET_COUNT_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".bucket_interval",
							"Sets the width of each time bucket.",
							NULL,
							&pssc_bucket_interval,
							300,
							1,
							BUCKET_INTERVAL_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".max_tags",
							"Sets the maximum number of tags stored per entry.",
							NULL,
							&pssc_max_tags,
							8,
							1,
							MAX_TAGS_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".max_tag_value_len",
							"Sets the maximum length of a tag value in bytes.",
							"Longer values are truncated on a character boundary.",
							&pssc_max_tag_value_len,
							64,
							1,
							MAX_TAG_VALUE_LEN_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable(PSSC_GUC_PREFIX ".max_tagset_bytes",
							"Sets the maximum size of a serialized tag set in bytes.",
							"The tag set is part of the hash key; tags that do not fit are dropped.",
							&pssc_max_tagset_bytes,
							512,
							MAX_TAGSET_BYTES_MIN,
							MAX_TAGSET_BYTES_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	/* sighup */
	DefineCustomIntVariable(PSSC_GUC_PREFIX ".scan_window",
							"Sets how many bytes at the head or tail of a long statement are searched for comments.",
							NULL,
							&pssc_scan_window,
							2048,
							SCAN_WINDOW_MIN,
							SCAN_WINDOW_MAX,
							PGC_SIGHUP,
							GUC_UNIT_BYTE,
							NULL, NULL, NULL);

	DefineCustomStringVariable(PSSC_GUC_PREFIX ".extractors",
							   "Sets the extractors that turn SQL comments into tags.",
							   NULL,
							   &pssc_extractors,
							   "sqlcommenter, marginalia",
							   PGC_SIGHUP,
							   0,
							   check_extractors,
							   assign_extractors,
							   NULL);

	DefineCustomStringVariable(PSSC_GUC_PREFIX ".tags",
							   "Sets the tag keys to keep (\"*\" keeps all).",
							   "Comma-separated, case-sensitive keys of at most 63 bytes, "
							   "matched after rename.",
							   &pssc_tags,
							   "action, controller, job",
							   PGC_SIGHUP,
							   GUC_LIST_INPUT,
							   check_tags,
							   assign_tags,
							   NULL);

	DefineCustomStringVariable(PSSC_GUC_PREFIX ".exclude_tags",
							   "Sets the tag keys to discard when tags is \"*\".",
							   "Comma-separated, case-sensitive keys of at most 63 bytes.",
							   &pssc_exclude_tags,
							   "traceparent, tracestate, request_id",
							   PGC_SIGHUP,
							   GUC_LIST_INPUT,
							   check_exclude_tags,
							   assign_exclude_tags,
							   NULL);

	DefineCustomEnumVariable(PSSC_GUC_PREFIX ".untagged",
							 "Selects whether statements without tags are recorded.",
							 "skip: not recorded; record: recorded with an empty tag set.",
							 &pssc_untagged,
							 PSSC_UNTAGGED_SKIP,
							 untagged_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable(PSSC_GUC_PREFIX ".normalize",
							   "Sets regex-replace rules that normalize tag values.",
							   "Comma-separated rules key: 'pattern' => 'replacement', "
							   "applied in order to the values of their key.",
							   &pssc_normalize,
							   "",
							   PGC_SIGHUP,
							   0,
							   check_normalize,
							   assign_normalize,
							   NULL);

	/* user */
	DefineCustomStringVariable(PSSC_GUC_PREFIX ".tags_override",
							   "Sets tags that override the tags of SQL comments in this session or transaction.",
							   "Comma-separated key='value' pairs (sqlcommenter format, "
							   "URL-encoded), merged with the tags of each statement; "
							   "they win key conflicts.",
							   &pssc_tags_override,
							   "",
							   PGC_USERSET,
							   0,
							   check_tags_override,
							   assign_tags_override,
							   NULL);

	PSSC_MARK_GUC_PREFIX_RESERVED(PSSC_GUC_PREFIX);
}
