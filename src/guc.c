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
#include "utils/guc.h"

#include "compat.h"
#include "guc.h"

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

/*
 * extractors: until the DSL parser exists (backlog item 20261005-091225-8),
 * the extra blob is just a NUL-terminated copy of the value, which lets the
 * assign hook detect changes without depending on when guc.c updates the
 * variable.
 */
static const char *cur_extractors = NULL;

static uint64 config_generation = 0;

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

uint64
pssc_guc_config_generation(void)
{
	return config_generation;
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

/*
 * extractors check_hook. Hook point for backlog item 20261005-091225-8, which
 * replaces this with the DSL parser (§4.2): it will validate the value and
 * return the parsed, pointer-free blob as extra. For now any string is
 * accepted and the blob is a copy of it.
 */
static bool
check_extractors(char **newval, void **extra, GucSource source)
{
	const char *v = *newval ? *newval : "";
	size_t		len = strlen(v);
	char	   *copy = pssc_guc_extra_alloc(len + 1);

	if (copy == NULL)
	{
		GUC_check_errcode(ERRCODE_OUT_OF_MEMORY);
		GUC_check_errdetail("Out of memory.");
		return false;
	}
	memcpy(copy, v, len + 1);
	*extra = copy;
	return true;
}

static void
assign_extractors(const char *newval, void *extra)
{
	const char *v = extra ? (const char *) extra : "";

	if (cur_extractors == NULL || strcmp(cur_extractors, v) != 0)
		config_generation++;
	cur_extractors = v;
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

	PSSC_MARK_GUC_PREFIX_RESERVED(PSSC_GUC_PREFIX);
}
