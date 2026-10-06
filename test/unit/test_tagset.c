/*
 * test_tagset.c
 *		Standalone unit tests for src/tagset.c, the tag-set canonicalization
 *		pipeline and extractor chain (DESIGN.md §4.2, §6.5, §6.11), built
 *		with -DPSSC_STANDALONE under ASan/UBSan (no server needed).
 *
 * The environment the backend normally supplies is faked here: a tracked
 * scratch allocator (with failure injection), a strict UTF-8 validator for
 * pg_verify_mbstr, UTF-8 character-boundary clipping for pg_mbcliplen, and
 * the guc.h tag-list accessors over a simple in-memory list. Extractor
 * configurations are built as real PsscExtractorList blobs (guc.h layout).
 * The output buffer is malloc'd with exactly max_tagset_bytes, so ASan
 * catches any overrun.
 *
 *	test_tagset                     run all tests
 *	test_tagset --emit-corpus DIR   write sample statements to DIR
 */
#include "pssc_standalone.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "guc.h"
#include "tagset.h"

static int	failures;
static int	checks;

#define FAIL(...) \
	do { \
		failures++; \
		fprintf(stderr, "not ok: "); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} while (0)

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) FAIL(__VA_ARGS__); \
	} while (0)

/* sprintf without the macOS deprecation; callers size their buffers */
static int
xsprintf(char *dst, const char *fmt,...)
{
	va_list		ap;
	int			n;

	va_start(ap, fmt);
	n = vsnprintf(dst, (size_t) 1 << 30, fmt, ap);
	va_end(ap);
	return n;
}

/* ---------------- fake guc.h tag lists ---------------- */

#define MAXLIST 1100

struct PsscTagList
{
	bool		match_all;
	int			n;
	char		keys[MAXLIST][PSSC_MAX_KEY_LEN + 1];
	int			lens[MAXLIST];
};

bool
pssc_tag_list_match_all(const PsscTagList *list)
{
	return list->match_all;
}

int
pssc_tag_list_count(const PsscTagList *list)
{
	return list->n;
}

const char *
pssc_tag_list_key(const PsscTagList *list, int i, int *len)
{
	assert(i >= 0 && i < list->n);
	if (len)
		*len = list->lens[i];
	return list->keys[i];
}

int
pssc_tag_list_find(const PsscTagList *list, const char *key, int len)
{
	for (int i = 0; i < list->n; i++)
		if (list->lens[i] == len && memcmp(list->keys[i], key, len) == 0)
			return i;
	return -1;
}

/* "a,b,c", "*" or "" (empty list) */
static PsscTagList *
mklist(const char *spec)
{
	PsscTagList *l = calloc(1, sizeof(PsscTagList));
	const char *p = spec;

	if (strcmp(spec, "*") == 0)
	{
		l->match_all = true;
		return l;
	}
	while (*p)
	{
		const char *e = strchr(p, ',');
		size_t		n = e ? (size_t) (e - p) : strlen(p);

		assert(n >= 1 && n <= PSSC_MAX_KEY_LEN && l->n < MAXLIST);
		memcpy(l->keys[l->n], p, n);
		l->lens[l->n++] = (int) n;
		p += n;
		if (*p == ',')
			p++;
	}
	return l;
}

/* ---------------- PsscExtractorList blobs ---------------- */

typedef struct XSpec
{
	PsscExtractorKind kind;
	PsscPosition position;
	bool		merge;
	bool		no_url_decode;	/* sqlcommenter: url_decode=off */
	const char *keys;			/* "a|b" or NULL */
	const char *rename;			/* "a:b|c:d" or NULL */
	const char *kv_sep;			/* marginalia; NULL = ":" */
	const char *pair_sep;		/* marginalia; NULL = "," */
} XSpec;

#define SC(pos)		{PSSC_EXTRACTOR_SQLCOMMENTER, pos}
#define MG(pos)		{PSSC_EXTRACTOR_MARGINALIA, pos}

typedef struct Blob
{
	char	   *base;
	size_t		used;
	size_t		cap;
} Blob;

static uint32
blob_reserve(Blob *b, size_t n)
{
	uint32		off;

	b->used = (b->used + 7) & ~(size_t) 7;
	off = (uint32) b->used;
	b->used += n;
	assert(b->used <= b->cap);
	return off;
}

static PsscBlobStr
blob_str(Blob *b, const char *s, size_t n)
{
	PsscBlobStr r;

	r.off = blob_reserve(b, n + 1);
	r.len = (uint32) n;
	memcpy(b->base + r.off, s, n);
	return r;
}

/* split "a|b|c" (sep '|') into PsscBlobStr[] */
static uint32
blob_split(Blob *b, const char *spec, uint32 *n)
{
	int			cnt = 1;
	uint32		off;
	PsscBlobStr *arr;
	const char *p = spec;

	for (const char *q = spec; *q; q++)
		cnt += (*q == '|');
	off = blob_reserve(b, cnt * sizeof(PsscBlobStr));
	for (int i = 0; i < cnt; i++)
	{
		const char *e = strchr(p, '|');
		size_t		len = e ? (size_t) (e - p) : strlen(p);
		PsscBlobStr s = blob_str(b, p, len);

		arr = (PsscBlobStr *) (b->base + off);
		arr[i] = s;
		p += len + (e != NULL);
	}
	*n = (uint32) cnt;
	return off;
}

static PsscExtractorList *
mkex(int n, const XSpec *specs)
{
	Blob		b;
	PsscExtractorList *list;

	b.cap = 1 << 16;
	b.base = calloc(1, b.cap);
	b.used = offsetof(PsscExtractorList, extractors) + n * sizeof(PsscExtractor);
	for (int i = 0; i < n; i++)
	{
		const XSpec *x = &specs[i];
		PsscExtractor *e;
		PsscExtractor tmp;

		memset(&tmp, 0, sizeof(tmp));
		tmp.kind = (uint8) x->kind;
		tmp.position = (uint8) x->position;
		tmp.merge = x->merge;
		tmp.url_decode = (x->kind == PSSC_EXTRACTOR_SQLCOMMENTER && !x->no_url_decode);
		if (x->keys)
		{
			tmp.has_keys = true;
			tmp.keys_off = blob_split(&b, x->keys, &tmp.nkeys);
		}
		if (x->rename)
		{
			uint32		nparts;
			uint32		parts_off = blob_split(&b, x->rename, &nparts);
			uint32		roff = blob_reserve(&b, nparts * sizeof(PsscBlobRename));

			for (uint32 j = 0; j < nparts; j++)
			{
				PsscBlobStr part = ((PsscBlobStr *) (b.base + parts_off))[j];
				const char *ps = b.base + part.off;
				const char *colon = strchr(ps, ':');
				PsscBlobRename r;

				assert(colon);
				r.from = blob_str(&b, ps, colon - ps);
				r.to = blob_str(&b, colon + 1, strlen(colon + 1));
				((PsscBlobRename *) (b.base + roff))[j] = r;
			}
			tmp.nrename = nparts;
			tmp.rename_off = roff;
		}
		if (x->kind == PSSC_EXTRACTOR_MARGINALIA)
		{
			const char *kv = x->kv_sep ? x->kv_sep : ":";
			const char *ps = x->pair_sep ? x->pair_sep : ",";

			tmp.kv_sep = blob_str(&b, kv, strlen(kv));
			tmp.pair_sep = blob_str(&b, ps, strlen(ps));
		}
		e = &((PsscExtractorList *) b.base)->extractors[i];
		*e = tmp;
	}
	list = (PsscExtractorList *) b.base;
	list->nextractors = (uint32) n;
	list->size = (uint32) b.used;
	return list;
}

/* regex extractor with capture-group keys "k1|k2" */
static PsscExtractorList *
mkex_regex(int n, const XSpec *specs, int regex_at, const char *keys,
		   PsscPosition pos, bool merge)
{
	XSpec		all[PSSC_MAX_EXTRACTORS];
	PsscExtractorList *l;
	int			j = 0;

	for (int i = 0; i <= n; i++)
	{
		if (i == regex_at)
		{
			XSpec		r = {PSSC_EXTRACTOR_REGEX, pos, merge, false, keys};

			all[j++] = r;
		}
		if (i < n)
			all[j++] = specs[i];
	}
	l = mkex(n + 1, all);
	/* regex: keys always present but are capture names, not an allowlist */
	return l;
}

/* ---------------- fake encoding environment ---------------- */

typedef struct Arena
{
	void	   *ptrs[4096];
	int			n;
	long		fail_at;		/* fail the allocation with this index (-1 never) */
	long		count;
	size_t		bytes;
} Arena;

static void *
arena_alloc(void *arg, size_t size)
{
	Arena	   *a = arg;
	void	   *p;

	if (a->fail_at >= 0 && a->count++ == a->fail_at)
		return NULL;
	if (a->fail_at < 0)
		a->count++;
	assert(a->n < 4096);
	p = malloc(size ? size : 1);
	assert(p);
	/* poison so the pipeline cannot rely on zeroed memory */
	memset(p, 0xa5, size);
	a->ptrs[a->n++] = p;
	a->bytes += size;
	return p;
}

static void
arena_free(Arena *a)
{
	for (int i = 0; i < a->n; i++)
		free(a->ptrs[i]);
	a->n = 0;
}

static int	verify_calls;

/* strict UTF-8 (no overlongs, no surrogates, <= U+10FFFF); never sees NUL */
static bool
utf8_verify(void *arg, const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t		i = 0;

	(void) arg;
	verify_calls++;
	while (i < len)
	{
		unsigned char c = p[i];
		int			n;
		uint32_t	cp;

		/* contract: the pipeline rejects NUL before calling verify */
		assert(c != 0);
		if (c < 0x80)
		{
			i++;
			continue;
		}
		if (c >= 0xc2 && c <= 0xdf)
			n = 1, cp = c & 0x1f;
		else if (c >= 0xe0 && c <= 0xef)
			n = 2, cp = c & 0x0f;
		else if (c >= 0xf0 && c <= 0xf4)
			n = 3, cp = c & 0x07;
		else
			return false;
		if (i + n + 1 > len)
			return false;
		for (int k = 1; k <= n; k++)
		{
			if ((p[i + k] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (p[i + k] & 0x3f);
		}
		if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
			(cp >= 0xd800 && cp <= 0xdfff))
			return false;
		i += n + 1;
	}
	return true;
}

static size_t
utf8_cliplen(void *arg, const char *s, size_t len, size_t limit)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t		n = limit;

	(void) arg;
	assert(len > limit);
	while (n > 0 && (p[n] & 0xc0) == 0x80)
		n--;
	return n;
}

/* ---------------- fake regex extractor ---------------- */

static int	regex_calls;
static int	regex_last_index;

/*
 * Each comment body yields one pair per configured capture key: key i gets
 * the i-th whitespace-separated word of the trimmed body (missing words are
 * not reported). Keys point into the blob, values into the body.
 */
static void
fake_regex(void *arg, int index, const PsscExtractorList *list,
		   const char *body, size_t len, const PsscPairOut *out,
		   PsscPairResult *result)
{
	const PsscExtractor *e = &list->extractors[index];
	const PsscBlobStr *keys = pssc_extractor_keys(list, e);
	size_t		i = 0;

	(void) arg;
	regex_calls++;
	regex_last_index = index;
	memset(result, 0, sizeof(*result));
	assert(e->kind == PSSC_EXTRACTOR_REGEX);
	for (uint32 k = 0; k < e->nkeys && result->npairs < out->max_pairs; k++)
	{
		size_t		st;
		PsscPair   *p;

		while (i < len && body[i] == ' ')
			i++;
		if (i >= len)
			break;
		st = i;
		while (i < len && body[i] != ' ')
			i++;
		p = &out->pairs[result->npairs++];
		p->key = pssc_blob_str(list, keys[k]);
		p->keylen = keys[k].len;
		p->value = body + st;
		p->valuelen = i - st;
		p->flags = 0;
	}
}

/* ---------------- fake value normalization ---------------- */

/*
 * Rules for key "route" only: every run of ASCII digits becomes ":id"
 * (global replacement, like a "\d+ => :id" rule). A value containing "FAIL"
 * fails (as on an engine error), and one containing "GROW" is replaced by
 * as many 'g' as the limit allows (to check the output bound). Every call
 * is recorded, so tests can check which keys reach step 6.
 */
static int	norm_calls;
static char norm_keys[64][PSSC_MAX_KEY_LEN + 2];	/* "key" of each call */
static size_t norm_limits[64];
static size_t norm_vlens[64];

static PsscNormalizeResult
fake_normalize(void *arg, const char *key, size_t klen, const char *val,
			   size_t vlen, size_t limit, const char **out, size_t *outlen)
{
	char	   *buf;
	size_t		n = 0;

	assert(klen >= 1 && klen <= PSSC_MAX_KEY_LEN);
	assert(vlen == 0 || memchr(val, '\0', vlen) == NULL);
	if (norm_calls < 64)
	{
		memcpy(norm_keys[norm_calls], key, klen);
		norm_keys[norm_calls][klen] = '\0';
		norm_limits[norm_calls] = limit;
		norm_vlens[norm_calls] = vlen;
	}
	norm_calls++;
	if (klen != 5 || memcmp(key, "route", 5) != 0)
		return PSSC_NORMALIZE_NO_RULES;
	for (size_t i = 0; i + 4 <= vlen; i++)
		if (memcmp(val + i, "FAIL", 4) == 0)
			return PSSC_NORMALIZE_FAILED;
	buf = arena_alloc(arg, limit + 1);
	for (size_t i = 0; i + 4 <= vlen; i++)
	{
		if (memcmp(val + i, "GROW", 4) == 0)
		{
			memset(buf, 'g', limit);
			*out = buf;
			*outlen = limit;
			return PSSC_NORMALIZE_DONE;
		}
	}
	for (size_t i = 0; i < vlen;)
	{
		if (val[i] >= '0' && val[i] <= '9')
		{
			while (i < vlen && val[i] >= '0' && val[i] <= '9')
				i++;
			if (n + 3 > limit)
				break;
			memcpy(buf + n, ":id", 3);
			n += 3;
			continue;
		}
		if (n + 1 > limit)
			break;
		buf[n++] = val[i++];
	}
	*out = buf;
	*outlen = n;
	return PSSC_NORMALIZE_DONE;
}

/* ---------------- running the pipeline ---------------- */

typedef struct Run
{
	const PsscExtractorList *ex;
	const char *tags;			/* list spec; default "*" */
	const char *exclude;		/* default "" */
	int			max_tags;		/* default 8 */
	int			max_value;		/* default 64 */
	int			max_bytes;		/* default 512 */
	int			window;			/* default 2048 */
	bool		non_scs;		/* standard_conforming_strings off */
	bool		with_regex;
	bool		with_normalize;
	long		fail_at;		/* alloc failure injection; 0 = none, n = fail nth (1-based) */
} Run;

typedef struct Res
{
	char		buf[16384];
	size_t		len;
	int			ntags;
	bool		footer;
	bool		oom;
	PsscTagsetStats st;
	char		text[65536];	/* human-readable "k=v|k=v" */
	long		allocs;
} Res;

static const char *
fmt_bytes(char *dst, const char *s, size_t n)
{
	char	   *d = dst;

	for (size_t i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char) s[i];

		if (c < 0x20 || c >= 0x7f || c == '|' || c == '=' || c == '\\')
			d += xsprintf(d, "\\x%02x", c);
		else
			*d++ = (char) c;
	}
	*d = '\0';
	return dst;
}

/* structural check of a serialized set; renders it into r->text */
static void
check_serialized(const char *name, const Run *cfg, Res *r)
{
	size_t		i = 0;
	int			n = 0;
	const char *prevk = NULL;
	size_t		prevkl = 0;
	char	   *t = r->text;

	*t = '\0';
	while (i < r->len)
	{
		const char *k = r->buf + i;
		size_t		kl = strnlen(k, r->len - i);
		const char *v;
		size_t		vl;

		CHECK(i + kl < r->len, "%s: unterminated key", name);
		if (i + kl >= r->len)
			return;
		i += kl + 1;
		v = r->buf + i;
		vl = strnlen(v, r->len - i);
		CHECK(i + vl < r->len, "%s: unterminated value", name);
		if (i + vl >= r->len)
			return;
		i += vl + 1;
		CHECK(kl >= 1 && kl <= PSSC_MAX_KEY_LEN, "%s: key length %zu", name, kl);
		CHECK(vl <= (size_t) cfg->max_value, "%s: value length %zu > %d", name, vl,
			  cfg->max_value);
		CHECK(utf8_verify(NULL, k, kl) && utf8_verify(NULL, v, vl),
			  "%s: invalid encoding in output", name);
		if (prevk)
		{
			size_t		m = prevkl < kl ? prevkl : kl;
			int			c = memcmp(prevk, k, m);

			CHECK(c < 0 || (c == 0 && prevkl < kl), "%s: keys not strictly sorted", name);
		}
		prevk = k;
		prevkl = kl;
		if (n)
			*t++ = '|';
		fmt_bytes(t, k, kl);
		t += strlen(t);
		*t++ = '=';
		fmt_bytes(t, v, vl);
		t += strlen(t);
		n++;
	}
	CHECK(n == r->ntags, "%s: ntags %d but %d serialized", name, r->ntags, n);
	CHECK(r->len <= (size_t) cfg->max_bytes, "%s: len %zu > max_tagset_bytes %d",
		  name, r->len, cfg->max_bytes);
	CHECK(r->ntags <= cfg->max_tags, "%s: ntags %d > max_tags %d", name, r->ntags,
		  cfg->max_tags);
}

static void
apply_defaults(Run *c)
{
	if (!c->tags)
		c->tags = "*";
	if (!c->exclude)
		c->exclude = "";
	if (!c->max_tags)
		c->max_tags = 8;
	if (!c->max_value)
		c->max_value = 64;
	if (!c->max_bytes)
		c->max_bytes = 512;
	if (!c->window)
		c->window = 2048;
}

static PsscExtractorList *default_ex;

/* run on s[start, end) */
static Res *
run_range(const char *name, Run cfg, const char *s, size_t start, size_t end)
{
	static Res	r;
	Arena	   *a = calloc(1, sizeof(Arena));
	PsscTagList *tags,
			   *excl;
	PsscTagsetLimits lim;
	PsscTagsetEnv env;
	PsscTagsetOut out;
	char	   *buf;

	apply_defaults(&cfg);
	tags = mklist(cfg.tags);
	excl = mklist(cfg.exclude);
	memset(&r, 0, sizeof(r));
	a->fail_at = cfg.fail_at > 0 ? cfg.fail_at - 1 : -1;
	lim.max_tags = cfg.max_tags;
	lim.max_tag_value_len = cfg.max_value;
	lim.max_tagset_bytes = cfg.max_bytes;
	lim.scan_window = cfg.window;
	lim.standard_conforming_strings = !cfg.non_scs;
	env.arg = a;
	env.alloc = arena_alloc;
	env.verify = utf8_verify;
	env.cliplen = utf8_cliplen;
	env.regex = cfg.with_regex ? fake_regex : NULL;
	env.normalize = cfg.with_normalize ? fake_normalize : NULL;
	/* exactly max_tagset_bytes, so ASan catches overruns */
	buf = malloc(cfg.max_bytes ? cfg.max_bytes : 1);
	memset(&out, 0x5a, sizeof(out));
	out.buf = buf;
	pssc_tagset_build(s, start, end, cfg.ex ? cfg.ex : default_ex, tags, excl,
					  &lim, &env, &out, &r.st);
	r.len = out.len;
	r.ntags = out.ntags;
	r.footer = out.footer;
	r.oom = out.oom;
	r.allocs = a->count;
	assert(r.len <= sizeof(r.buf));
	if (r.len <= (size_t) cfg.max_bytes)
		memcpy(r.buf, buf, r.len);
	check_serialized(name, &cfg, &r);
	free(buf);
	arena_free(a);
	free(a);
	free(tags);
	free(excl);
	return &r;
}

static Res *
run(const char *name, Run cfg, const char *s)
{
	return run_range(name, cfg, s, 0, strlen(s));
}

#define EXPECT(name, cfg, sql, exp) \
	do { \
		Res *r_ = run(name, cfg, sql); \
		CHECK(strcmp(r_->text, exp) == 0, "%s: got \"%s\", want \"%s\"", \
			  name, r_->text, exp); \
	} while (0)

/* ---------------- tests ---------------- */

static void
test_serialization(void)
{
	Run			c = {0};
	Res		   *r;
	static const char want[] = "a\0" "1\0" "b\0" "22\0";

	r = run("serialize", c, "SELECT 1 /*b:22,a:1*/");
	CHECK(r->len == sizeof(want) - 1 && memcmp(r->buf, want, r->len) == 0,
		  "serialize: wrong bytes (len %zu, text %s)", r->len, r->text);
	CHECK(r->ntags == 2, "serialize: ntags %d", r->ntags);
	CHECK(!r->footer && !r->oom, "serialize: flags");

	/* empty value is a value; a key that is a prefix sorts first */
	EXPECT("empty value", c, "SELECT 1 /*ab='',a='x'*/", "a=x|ab=");
	/* bytewise (unsigned) order: 'B' < 'a' < '\xc3...' */
	EXPECT("bytewise order", c, "SELECT 1 /*\xc3\xa9:1,a:2,B:3*/",
		   "B=3|a=2|\\xc3\\xa9=1");
	/* nothing at all */
	r = run("no comment", c, "SELECT 1");
	CHECK(r->len == 0 && r->ntags == 0, "no comment: %s", r->text);
}

/* the same tags in any order (pairs, comments, extractors) -> same bytes */
static void
test_order_independence(void)
{
	static const char *perms[] = {
		"SELECT 1 /*controller:users,action:show,job:x,zeta:9,alpha:0*/",
		"SELECT 1 /*zeta:9,alpha:0,job:x,action:show,controller:users*/",
		"SELECT 1 /*job:x,controller:users,zeta:9,action:show,alpha:0*/",
		"SELECT 1 /*job:x*/ /*controller:users,zeta:9*/ /*action:show,alpha:0*/",
		"SELECT 1 /*alpha:0,action:show*/ /*zeta:9*/ /*controller:users,job:x*/",
		"SELECT 1 /*action='show',zeta='9'*/ /*job:x,alpha:0,controller:users*/",
		"SELECT 1 /*job:x,alpha:0,controller:users*/ /*action='show',zeta='9'*/",
	};
	XSpec		xs[] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND)};
	Run			c = {0};
	char		first[16384];
	size_t		firstlen = 0;

	xs[1].merge = true;
	c.ex = mkex(2, xs);
	for (size_t i = 0; i < sizeof(perms) / sizeof(perms[0]); i++)
	{
		Res		   *r = run("order", c, perms[i]);

		CHECK(r->ntags == 5, "order %zu: %s", i, r->text);
		if (i == 0)
		{
			memcpy(first, r->buf, r->len);
			firstlen = r->len;
			CHECK(strcmp(r->text, "action=show|alpha=0|controller=users|job=x|zeta=9") == 0,
				  "order: %s", r->text);
		}
		else
			CHECK(r->len == firstlen && memcmp(r->buf, first, r->len) == 0,
				  "order %zu: bytes differ: %s", i, r->text);
	}

	/* with an allowlist (different allowlist orders, same output) */
	c.tags = "zeta,job,action";
	for (size_t i = 0; i < sizeof(perms) / sizeof(perms[0]); i++)
		EXPECT("order allowlist", c, perms[i], "action=show|job=x|zeta=9");
	c.tags = "action,zeta,job";
	for (size_t i = 0; i < sizeof(perms) / sizeof(perms[0]); i++)
		EXPECT("order allowlist 2", c, perms[i], "action=show|job=x|zeta=9");
}

static void
test_allow_deny(void)
{
	Run			c = {0};

	c.tags = "controller,action";
	EXPECT("allowlist", c, "SELECT 1 /*controller:u,foo:1,action:a,bar:2*/",
		   "action=a|controller=u");
	c.tags = "";
	EXPECT("empty allowlist", c, "SELECT 1 /*controller:u*/", "");
	c.tags = "*";
	c.exclude = "traceparent,request_id";
	EXPECT("denylist", c,
		   "SELECT 1 /*controller:u,traceparent:00-ab,request_id:7,x:y*/",
		   "controller=u|x=y");
	/* the denylist only applies with tags = '*' */
	c.tags = "controller,traceparent";
	EXPECT("denylist ignored", c, "SELECT 1 /*controller:u,traceparent:t*/",
		   "controller=u|traceparent=t");
	/* keys are case-sensitive */
	c.tags = "Controller";
	EXPECT("case", c, "SELECT 1 /*controller:u,Controller:U*/", "Controller=U");
	{
		Res		   *r;

		c.tags = "controller";
		c.exclude = "";
		r = run("filter not counted", c, "SELECT 1 /*controller:u,a:1,b:2*/");
		CHECK(r->st.invalid_tags == 0 && r->st.dropped_tags == 0,
			  "filter not counted: invalid %llu dropped %llu",
			  (unsigned long long) r->st.invalid_tags,
			  (unsigned long long) r->st.dropped_tags);
	}
}

/* per-extractor keys match ORIGINAL names, before rename (§6.11 step 3/4) */
static void
test_keys_and_rename(void)
{
	XSpec		x = SC(PSSC_POS_APPEND);
	Run			c = {0};

	x.rename = "route:endpoint";
	c.tags = "endpoint,action";

	x.keys = "route";
	c.ex = mkex(1, &x);
	EXPECT("keys original", c, "SELECT 1 /*route='/u',action='s'*/", "endpoint=/u");
	/* an "endpoint" key in the comment is not "route": not kept by keys */
	EXPECT("keys original 2", c, "SELECT 1 /*endpoint='/e'*/", "");

	x.keys = "endpoint";
	c.ex = mkex(1, &x);
	EXPECT("keys renamed only", c, "SELECT 1 /*route='/u'*/", "");
	EXPECT("keys renamed only 2", c, "SELECT 1 /*endpoint='/e'*/", "endpoint=/e");

	x.keys = "route|action";
	c.ex = mkex(1, &x);
	EXPECT("keys both", c, "SELECT 1 /*route='/u',action='s',job='j'*/",
		   "action=s|endpoint=/u");

	/* no keys: everything goes on to rename and the global allowlist */
	x.keys = NULL;
	c.ex = mkex(1, &x);
	EXPECT("no keys", c, "SELECT 1 /*route='/u',action='s'*/", "action=s|endpoint=/u");

	/* global allowlist applies after rename: naming the original fails */
	c.tags = "route";
	EXPECT("global after rename", c, "SELECT 1 /*route='/u'*/", "");

	/* keys see decoded names (decode is step 1) */
	x.keys = "route";
	c.ex = mkex(1, &x);
	c.tags = "*";
	EXPECT("keys decoded", c, "SELECT 1 /*%72oute='/u'*/", "endpoint=/u");

	/* rename into a key that also occurs: first occurrence wins */
	x.keys = NULL;
	c.ex = mkex(1, &x);
	EXPECT("rename collision", c, "SELECT 1 /*route='/r',endpoint='/e'*/", "endpoint=/r");
	EXPECT("rename collision 2", c, "SELECT 1 /*endpoint='/e',route='/r'*/", "endpoint=/e");

	/* several renames, swap */
	x.rename = "a:b|b:a";
	c.ex = mkex(1, &x);
	EXPECT("rename swap", c, "SELECT 1 /*a='1',b='2'*/", "a=2|b=1");

	/* marginalia keys */
	{
		XSpec		m = MG(PSSC_POS_APPEND);

		m.keys = "controller";
		m.rename = "controller:endpoint";
		c.ex = mkex(1, &m);
		c.tags = "endpoint";
		EXPECT("mg keys", c, "SELECT 1 /*controller:u,endpoint:e*/", "endpoint=u");
	}
}

/* first extractor that produces a tag wins; merge=on unions (§4.2) */
static void
test_chain(void)
{
	XSpec		xs[3] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND), MG(PSSC_POS_APPEND)};
	Run			c = {0};
	Res		   *r;

	c.tags = "controller,action,a,b,c";
	c.ex = mkex(2, xs);
	EXPECT("chain second", c, "SELECT 1 /*controller:u,action:s*/",
		   "action=s|controller=u");
	EXPECT("chain first", c, "SELECT 1 /*controller='u',action='s'*/",
		   "action=s|controller=u");
	/* first produced: second not consulted */
	EXPECT("chain first wins", c, "SELECT 1 /*controller='a'*/ /*action:b*/",
		   "controller=a");
	/* first produced pairs, but none survives the allowlist: it produced nothing */
	EXPECT("chain filtered", c, "SELECT 1 /*foo='1'*/ /*controller:x*/", "controller=x");
	/* first produced only invalid pairs */
	EXPECT("chain invalid", c, "SELECT 1 /*controller='%FF'*/ /*controller:x*/",
		   "controller=x");

	/* merge=on: union; earlier extractor wins on a duplicate key */
	xs[1].merge = true;
	c.ex = mkex(2, xs);
	EXPECT("merge", c, "SELECT 1 /*controller='a'*/ /*action:b*/", "action=b|controller=a");
	EXPECT("merge dup", c, "SELECT 1 /*controller='a'*/ /*controller:b,action:c*/",
		   "action=c|controller=a");
	EXPECT("merge dup order", c, "SELECT 1 /*controller:b,action:c*/ /*controller='a'*/",
		   "action=c|controller=a");
	/* merge=on still works when the first produced nothing */
	EXPECT("merge first empty", c, "SELECT 1 /*controller:b*/", "controller=b");

	/* reversed order: marginalia first now wins the duplicate */
	{
		XSpec		rv[2] = {MG(PSSC_POS_APPEND), SC(PSSC_POS_APPEND)};

		rv[1].merge = true;
		c.ex = mkex(2, rv);
		EXPECT("merge dup reversed", c,
			   "SELECT 1 /*controller='a'*/ /*controller:b,action:c*/",
			   "action=c|controller=b");
	}

	/* a skipped (non-merge) extractor does not stop a later merge=on one */
	xs[1].merge = false;
	xs[2].merge = true;
	xs[2].kv_sep = "=";
	c.ex = mkex(3, xs);
	EXPECT("merge after skip", c, "SELECT 1 /*a='1'*/ /*b:2*/ /*c=3*/", "a=1|c=3");

	/* nothing configured */
	c.ex = mkex(0, xs);
	EXPECT("no extractors", c, "SELECT 1 /*a:1*/", "");

	/* regex extractor without a hook produces nothing: chain moves on */
	c.with_regex = false;
	c.tags = "*";
	c.ex = mkex_regex(1, xs + 1, 0, "k1|k2", PSSC_POS_ANY, false);
	regex_calls = 0;
	EXPECT("regex no hook", c, "SELECT 1 /*w1 w2*/ /*a:1*/", "a=1");

	/* with the hook, regex pairs go through the pipeline */
	c.with_regex = true;
	r = run("regex hook", c, "SELECT 1 /*w1 w2*/ /*a:1*/");
	CHECK(strcmp(r->text, "k1=w1|k2=w2") == 0, "regex hook: %s", r->text);
	CHECK(regex_calls == 2 && regex_last_index == 0, "regex hook calls %d index %d",
		  regex_calls, regex_last_index);
	/* regex at index 1, merge=on, with allowlist and value truncation */
	c.ex = mkex_regex(1, xs + 1, 1, "k1|k2", PSSC_POS_ANY, true);
	c.tags = "a,k2";
	c.max_value = 2;
	regex_calls = 0;
	EXPECT("regex merge", c, "SELECT 1 /*a:1*/ /*w1 w22*/", "a=1|k2=w2");
	CHECK(regex_last_index == 1, "regex merge index %d", regex_last_index);
	/* a hook's pairs are checked for NUL even when not flagged */
	{
		static const char nul[] = "SELECT 1 /*a\0b ok*/";
		Res		   *rn;

		c.tags = "*";
		c.max_value = 0;
		c.ex = mkex_regex(0, xs, 0, "k1|k2", PSSC_POS_ANY, false);
		rn = run_range("regex nul", c, nul, 0, sizeof(nul) - 1);
		CHECK(strcmp(rn->text, "k2=ok") == 0 && rn->st.invalid_tags == 1,
			  "regex nul: %s", rn->text);
	}
	/* a regex extractor's keys are capture names, not an allowlist */
	c.tags = "*";
	c.max_value = 0;
	c.ex = mkex_regex(0, xs, 0, "x", PSSC_POS_ANY, false);
	EXPECT("regex keys", c, "SELECT 1 /*hello*/", "x=hello");
}

/* duplicates within one extractor: first occurrence wins (position=any) */
static void
test_first_occurrence(void)
{
	XSpec		x = MG(PSSC_POS_ANY);
	Run			c = {0};

	c.ex = mkex(1, &x);
	EXPECT("any first", c, "/*a:1*/ SELECT /*a:2,b:3*/ 1 /*b:4*/", "a=1|b=3");
	EXPECT("same comment", c, "SELECT 1 /*a:1,a:2*/", "a=1");
	/* many duplicates: the sort must not depend on qsort stability */
	{
		static char sql[8192];
		size_t		n = 0;

		n += snprintf(sql + n, sizeof(sql) - n, "SELECT 1 /*");
		for (int i = 0; i < 900; i++)
			n += snprintf(sql + n, sizeof(sql) - n, "%c:%d,", "cab"[i % 3], i);
		snprintf(sql + n, sizeof(sql) - n, "*/");
		c.window = 1 << 20;
		EXPECT("many duplicates", c, sql, "a=1|b=2|c=0");
		c.window = 0;
	}
	x.position = PSSC_POS_APPEND;
	c.ex = mkex(1, &x);
	EXPECT("append run", c, "SELECT 1 /*a:1*/ /*a:2,b:2*/", "a=1|b=2");
	EXPECT("append only run", c, "/*z:0*/ SELECT 1 /*a:1*/", "a=1");
	x.position = PSSC_POS_PREPEND;
	c.ex = mkex(1, &x);
	EXPECT("prepend run", c, "/*a:1*/ -- a:2,b:2\n SELECT 1 /*c:3*/", "a=1|b=2");
	/* position decides the comments, per extractor, scans shared */
	{
		XSpec		xs[2] = {MG(PSSC_POS_PREPEND), SC(PSSC_POS_APPEND)};

		xs[1].merge = true;
		c.ex = mkex(2, xs);
		EXPECT("mixed positions", c, "/*a:1*/ SELECT 1 /*b='2'*/", "a=1|b=2");
		EXPECT("mixed positions 2", c, "/*b='2'*/ SELECT 1 /*a:1*/", "");
	}
}

/* §6.11 step 2: NUL and invalid encoding; step 6: long keys */
static void
test_invalid(void)
{
	Run			c = {0};
	Res		   *r;
	char		key63[64],
				key64[65],
				sql[512];

	r = run("bad utf8 value", c, "SELECT 1 /*a:\xff\xfe,b:ok*/");
	CHECK(strcmp(r->text, "b=ok") == 0 && r->st.invalid_tags == 1,
		  "bad utf8 value: %s invalid %llu", r->text,
		  (unsigned long long) r->st.invalid_tags);
	r = run("bad utf8 key", c, "SELECT 1 /*\xc3(:1,b:ok*/");
	CHECK(strcmp(r->text, "b=ok") == 0 && r->st.invalid_tags == 1,
		  "bad utf8 key: %s", r->text);
	/* a truncated multibyte character at the end of a value */
	r = run("bad utf8 tail", c, "SELECT 1 /*a:x\xe2\x82*/");
	CHECK(r->ntags == 0 && r->st.invalid_tags == 1, "bad utf8 tail: %s", r->text);
	/* %-decoded invalid bytes and NUL */
	r = run("decoded ff", c, "SELECT 1 /*a='%FF',b='%00',c='x%00y',d='ok'*/");
	CHECK(strcmp(r->text, "d=ok") == 0 && r->st.invalid_tags == 3,
		  "decoded: %s invalid %llu", r->text, (unsigned long long) r->st.invalid_tags);
	r = run("decoded nul key", c, "SELECT 1 /*a%00b='1'*/");
	CHECK(r->ntags == 0 && r->st.invalid_tags == 1, "decoded nul key: %s", r->text);
	/* raw NUL inside the statement (never from the server, but harmless) */
	{
		static const char s[] = "SELECT 1 /*a:x\0y,b:1*/";

		r = run_range("raw nul", c, s, 0, sizeof(s) - 1);
		CHECK(strcmp(r->text, "b=1") == 0 && r->st.invalid_tags == 1,
			  "raw nul: %s", r->text);
	}
	/* step 2 precedes the allowlist: rejected pairs are counted even if unlisted */
	c.tags = "b";
	r = run("invalid unlisted", c, "SELECT 1 /*a:\xff,b:1*/");
	CHECK(strcmp(r->text, "b=1") == 0 && r->st.invalid_tags == 1,
		  "invalid unlisted: %s", r->text);
	c.tags = "*";

	/* a valid multibyte value is kept */
	EXPECT("valid utf8", c, "SELECT 1 /*k:\xe2\x82\xac\xf0\x9f\x98\x80*/",
		   "k=\\xe2\\x82\\xac\\xf0\\x9f\\x98\\x80");

	/* keys: 63 bytes kept, 64 dropped and counted */
	memset(key63, 'k', 63);
	key63[63] = '\0';
	memset(key64, 'q', 64);
	key64[64] = '\0';
	snprintf(sql, sizeof(sql), "SELECT 1 /*%s:1,%s:2*/", key63, key64);
	r = run("key length", c, sql);
	CHECK(r->ntags == 1 && r->len == 63 + 1 + 2 && r->st.invalid_tags == 1,
		  "key length: ntags %d len %zu invalid %llu", r->ntags, r->len,
		  (unsigned long long) r->st.invalid_tags);
	/* a rename to a key > 63 bytes cannot happen (check_hook), but a long
	 * original renamed to a short key is fine */
	{
		XSpec		x = MG(PSSC_POS_APPEND);
		char		ren[200];
		char		want[100];

		snprintf(ren, sizeof(ren), "%s:short", key64);
		x.rename = ren;
		c.ex = mkex(1, &x);
		snprintf(want, sizeof(want), "%s=1|short=2", key63);
		EXPECT("long renamed", c, sql, want);
		c.ex = NULL;
	}
	/* the renamed key is verified too: rename targets come from config,
	 * which may be in another encoding than this database */
	{
		XSpec		x = MG(PSSC_POS_APPEND);

		x.rename = "a:\xff|b:\xc3\xa9|c:\xe9t\xe9";
		c.ex = mkex(1, &x);
		r = run("rename invalid", c, "SELECT 1 /*a:1,b:2,c:3,d:4*/");
		CHECK(strcmp(r->text, "d=4|\\xc3\\xa9=2") == 0 && r->st.invalid_tags == 2,
			  "rename invalid: %s invalid %llu", r->text,
			  (unsigned long long) r->st.invalid_tags);
		/* an invalid rename target is dropped even when it is listed */
		c.tags = "\xff,d";
		r = run("rename invalid listed", c, "SELECT 1 /*a:1,d:4*/");
		CHECK(strcmp(r->text, "d=4") == 0 && r->st.invalid_tags == 1,
			  "rename invalid listed: %s invalid %llu", r->text,
			  (unsigned long long) r->st.invalid_tags);
		c.tags = "*";
		c.ex = NULL;
	}
	/* invalid %-escapes are kept literally, not rejected */
	EXPECT("bad escape kept", c, "SELECT 1 /*a='%zz'*/", "a=%zz");
	/*
	 * malformed segments count as invalid tags when the same parser got a
	 * well-formed pair from the same comment: marginalia here (1 malformed
	 * segment); sqlcommenter finds no pair in it, so its 3 are not counted
	 */
	r = run("malformed", c, "SELECT 1 /*with annotation, no:pairs here,a:1*/");
	CHECK(strcmp(r->text, "a=1|no=pairs here") == 0 && r->st.invalid_tags == 1,
		  "malformed: %s invalid %llu", r->text, (unsigned long long) r->st.invalid_tags);
	{
		XSpec		xs[2] = {SC(PSSC_POS_ANY), MG(PSSC_POS_ANY)};

		xs[1].merge = true;
		c.ex = mkex(2, xs);
		/* each parser probes the other format's comment: not counted */
		r = run("cross-format", c, "SELECT 1 /*a='1',b='2'*/ /*c:3,d:4*/");
		CHECK(strcmp(r->text, "a=1|b=2|c=3|d=4") == 0 && r->st.invalid_tags == 0,
			  "cross-format: %s invalid %llu", r->text,
			  (unsigned long long) r->st.invalid_tags);
		/* a broken sqlcommenter pair next to a good one is counted once
		 * (marginalia finds no pair in that comment) */
		r = run("broken sqlcommenter", c, "SELECT 1 /*a='1',b=2,c='3*/ /*d:4*/");
		CHECK(strcmp(r->text, "a=1|d=4") == 0 && r->st.invalid_tags == 2,
			  "broken sqlcommenter: %s invalid %llu", r->text,
			  (unsigned long long) r->st.invalid_tags);
		/* a comment with no well-formed pair counts nothing */
		r = run("all malformed", c, "SELECT 1 /*x y, z*/ /*a:1*/");
		CHECK(strcmp(r->text, "a=1") == 0 && r->st.invalid_tags == 0,
			  "all malformed: %s invalid %llu", r->text,
			  (unsigned long long) r->st.invalid_tags);
		c.ex = NULL;
	}
}

/* §6.11 step 7: truncate values on a character boundary */
static void
test_truncation(void)
{
	Run			c = {0};

	c.max_value = 4;
	EXPECT("truncate ascii", c, "SELECT 1 /*a:abcdef,b:abcd,c:abc*/", "a=abcd|b=abcd|c=abc");
	/* a(1) e-acute(2) euro(3): 4 bytes would cut the euro sign */
	EXPECT("truncate mb", c, "SELECT 1 /*a:a\xc3\xa9\xe2\x82\xac*/", "a=a\\xc3\\xa9");
	c.max_value = 3;
	EXPECT("truncate mb 3", c, "SELECT 1 /*a:a\xc3\xa9\xe2\x82\xac*/", "a=a\\xc3\\xa9");
	c.max_value = 2;
	EXPECT("truncate mb 2", c, "SELECT 1 /*a:a\xc3\xa9\xe2\x82\xac*/", "a=a");
	c.max_value = 1;
	EXPECT("truncate mb all", c, "SELECT 1 /*a:\xe2\x82\xac*/", "a=");
	/* truncation does not count as dropping or invalid */
	{
		Res		   *r = run("truncate counts", c, "SELECT 1 /*a:abcdef*/");

		CHECK(r->st.invalid_tags == 0 && r->st.dropped_tags == 0, "truncate counts");
	}
	/* truncated values can make different inputs collide: still one tag */
	c.max_value = 2;
	EXPECT("truncate dup", c, "SELECT 1 /*a:xx1,a:xx2*/", "a=xx");
}

/* max_tags / max_tagset_bytes: drop order and the hard size bound */
static void
test_limits(void)
{
	Run			c = {0};
	Res		   *r;

	/* allowlist order c, b, a: a is dropped first */
	c.tags = "c,b,a";
	c.max_tags = 2;
	r = run("max_tags allowlist", c, "SELECT 1 /*a:1,b:2,c:3*/");
	CHECK(strcmp(r->text, "b=2|c=3") == 0 && r->st.dropped_tags == 1,
		  "max_tags allowlist: %s dropped %llu", r->text,
		  (unsigned long long) r->st.dropped_tags);
	/* tags = '*': overflow dropped in reverse sorted-key order */
	c.tags = "*";
	r = run("max_tags star", c, "SELECT 1 /*c:3,a:1,b:2*/");
	CHECK(strcmp(r->text, "a=1|b=2") == 0 && r->st.dropped_tags == 1,
		  "max_tags star: %s", r->text);

	/* bytes: each "k\0vvvv\0" is 7 bytes; 20 bytes hold two */
	c.max_tags = 8;
	c.max_bytes = 20;
	c.tags = "c,b,a";
	r = run("bytes allowlist", c, "SELECT 1 /*a:1111,b:2222,c:3333*/");
	CHECK(strcmp(r->text, "b=2222|c=3333") == 0 && r->len == 14 &&
		  r->st.dropped_tags == 1, "bytes allowlist: %s len %zu", r->text, r->len);
	/* exactly full */
	c.max_bytes = 21;
	r = run("bytes exact", c, "SELECT 1 /*a:1111,b:2222,c:3333*/");
	CHECK(r->ntags == 3 && r->len == 21 && r->st.dropped_tags == 0,
		  "bytes exact: %s", r->text);
	/* greedy: a tag that does not fit is dropped, and lower-priority tags
	 * that still fit are kept */
	c.max_bytes = 20;
	r = run("bytes greedy", c, "SELECT 1 /*a:1,b:222222222222,c:3333*/");
	CHECK(strcmp(r->text, "a=1|c=3333") == 0 && r->len == 11 &&
		  r->st.dropped_tags == 1, "bytes greedy: %s", r->text);
	/* an oversized top-priority tag does not empty the set */
	c.tags = "action,controller";
	r = run("oversized first", c,
			"SELECT 1 /*action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,controller:c*/");
	CHECK(strcmp(r->text, "controller=c") == 0 && r->st.dropped_tags == 1,
		  "oversized first: %s", r->text);
	/* max_tags still keeps the highest-priority tags that fit */
	c.tags = "c,b,a,d";
	c.max_tags = 2;
	r = run("greedy count", c, "SELECT 1 /*a:1,b:22222222222222,c:3,d:4*/");
	CHECK(strcmp(r->text, "a=1|c=3") == 0 && r->st.dropped_tags == 2,
		  "greedy count: %s", r->text);
	c.max_tags = 8;
	c.tags = "*";
	r = run("bytes greedy star", c, "SELECT 1 /*c:3,b:222222222222,a:1111*/");
	CHECK(strcmp(r->text, "a=1111|c=3") == 0 && r->st.dropped_tags == 1,
		  "bytes greedy star: %s", r->text);
	c.tags = "c,b,a";
	c.tags = "*";
	r = run("bytes star", c, "SELECT 1 /*c:3333,b:2222,a:1111*/");
	CHECK(strcmp(r->text, "a=1111|b=2222") == 0 && r->st.dropped_tags == 1,
		  "bytes star: %s", r->text);
	/* one tag larger than the whole budget */
	c.max_bytes = 128;
	c.max_value = 4096;
	{
		char		sql[600];

		memset(sql, 0, sizeof(sql));
		strcpy(sql, "SELECT 1 /*a:");
		memset(sql + strlen(sql), 'v', 300);
		strcat(sql, "*/");
		r = run("huge tag", c, sql);
		CHECK(r->ntags == 0 && r->len == 0 && r->st.dropped_tags == 1,
			  "huge tag: %s", r->text);
	}
	/* drops depend only on the tag set, not on the order */
	c.max_value = 64;
	c.max_bytes = 20;
	c.tags = "c,b,a";
	EXPECT("bytes order 1", c, "SELECT 1 /*c:3333,a:1111,b:2222*/", "b=2222|c=3333");
	EXPECT("bytes order 2", c, "SELECT 1 /*b:2222*/ /*c:3333,a:1111*/", "b=2222|c=3333");
}

static void
test_footer(void)
{
	static const char s[] = "SELECT 1; SELECT 2; /*controller:x*/";
	static const char s2[] = "SELECT 1 /*a:1*/; /*controller:x*/";
	static const char s3[] = "SELECT 1 /*foo:1*/; /*controller:x*/";
	static const char s4[] = "SELECT 1; /*a:1*/ ; -- b:2\n";
	static const char s4a[] = "SELECT 1; /*a:1*/ ; /*b:2*/ ;";
	Run			c = {0};
	Res		   *r;

	/* SELECT 2 is provably last: it gets the footer */
	r = run_range("footer last", c, s, 10, 18);
	CHECK(strcmp(r->text, "controller=x") == 0 && r->footer, "footer last: %s", r->text);
	/* SELECT 1 is not last */
	r = run_range("footer not last", c, s, 0, 8);
	CHECK(r->ntags == 0 && !r->footer, "footer not last: %s", r->text);
	/* own tags win; footer unused */
	r = run_range("footer own", c, s2, 0, 16);
	CHECK(strcmp(r->text, "a=1") == 0 && !r->footer, "footer own: %s", r->text);
	/* own comment yields no (allowlisted) tag: footer used */
	c.tags = "controller";
	r = run_range("footer own filtered", c, s3, 0, 18);
	CHECK(strcmp(r->text, "controller=x") == 0 && r->footer,
		  "footer own filtered: %s", r->text);
	c.tags = "*";
	/* footer uses the extractor's position (append run across ';') */
	r = run_range("footer append", c, s4a, 0, 8);
	CHECK(strcmp(r->text, "a=1|b=2") == 0 && r->footer, "footer append: %s", r->text);
	/* a trailing line comment ends no append run */
	r = run_range("footer append line", c, s4, 0, 8);
	CHECK(r->ntags == 0, "footer append line: %s", r->text);
	{
		XSpec		x = MG(PSSC_POS_ANY);

		c.ex = mkex(1, &x);
		r = run_range("footer any", c, s4, 0, 8);
		CHECK(strcmp(r->text, "a=1|b=2") == 0 && r->footer, "footer any: %s", r->text);
		c.ex = NULL;
	}
	/* the chain applies to the footer too */
	{
		XSpec		xs[2] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND)};
		static const char s5[] = "SELECT 1; /*a='1'*/ /*b:2*/";

		xs[1].merge = true;
		c.ex = mkex(2, xs);
		r = run_range("footer merge", c, s5, 0, 8);
		CHECK(strcmp(r->text, "a=1|b=2") == 0 && r->footer, "footer merge: %s", r->text);
		c.ex = NULL;
	}
	/* footer only for the statement's END: range ends before trivia */
	r = run_range("footer whole", c, s, 0, sizeof(s) - 1);
	CHECK(strcmp(r->text, "controller=x") == 0 && !r->footer, "footer whole: %s", r->text);
	/* footer limited by scan_window */
	c.window = 64;
	{
		char		big[400];

		strcpy(big, "SELECT 1;");
		memset(big + 9, ' ', 100);
		strcpy(big + 109, "/*a:1*/");
		r = run_range("footer window", c, big, 0, 8);
		CHECK(r->ntags == 0, "footer window: %s", r->text);
	}
}

static void
test_heuristic(void)
{
	char		big[5000];
	Run			c = {0};
	Res		   *r;

	memset(big, 0, sizeof(big));
	strcpy(big, "SELECT ");
	memset(big + 7, 'x', 3000);
	strcat(big, " /*a:1*/");
	r = run("heuristic", c, big);
	CHECK(strcmp(r->text, "a=1") == 0, "heuristic: %s", r->text);
	/* sqlcommenter and marginalia (both append) share one scan */
	CHECK(r->st.heuristic_scans == 1, "heuristic scans %llu",
		  (unsigned long long) r->st.heuristic_scans);
	r = run("heuristic short", c, "SELECT 1 /*a:1*/");
	CHECK(r->st.heuristic_scans == 0, "heuristic short");
	c.window = 8192;
	r = run("heuristic wide", c, big);
	CHECK(r->st.heuristic_scans == 0 && r->ntags == 1, "heuristic wide");
}

/* malformed and hostile input never fails or overruns */
static void
test_hostile(void)
{
	Run			c = {0};
	Res		   *r;
	size_t		n = 200000;
	char	   *s = malloc(n * 12 + 64);
	char	   *p = s;

	/* huge comment count */
	p += xsprintf(p, "SELECT 1 ");
	for (size_t i = 0; i < n; i++)
		p += xsprintf(p, "/*a%zu:1*/", i % 1000);
	*p = '\0';
	r = run("many comments", c, s);
	CHECK(r->ntags <= 8, "many comments");
	c.window = 1024 * 1024;
	r = run("many comments wide", c, s);
	CHECK(r->ntags == 8 && r->st.dropped_tags > 0, "many comments wide: %d", r->ntags);
	{
		XSpec		x = MG(PSSC_POS_ANY);

		c.ex = mkex(1, &x);
		r = run("many comments any", c, s);
		CHECK(r->ntags == 8, "many comments any: %d", r->ntags);
		x.position = PSSC_POS_PREPEND;
		c.ex = mkex(1, &x);
		r = run("many comments prepend", c, s + 9);
		CHECK(r->ntags == 8, "many comments prepend: %d", r->ntags);
		c.ex = NULL;
	}

	/* one comment with very many pairs, distinct keys */
	p = s;
	p += xsprintf(p, "SELECT 1 /*");
	for (size_t i = 0; i < 60000; i++)
		p += xsprintf(p, "k%zu:v,", i);
	p += xsprintf(p, "*/");
	r = run("many pairs", c, s);
	CHECK(r->ntags == 8 && strncmp(r->text, "k0=v|k1=v|k10=v|", 16) == 0,
		  "many pairs: %.60s", r->text);
	c.tags = "k59999,k3";
	r = run("many pairs allowlist", c, s);
	CHECK(strcmp(r->text, "k3=v|k59999=v") == 0, "many pairs allowlist: %s", r->text);
	c.tags = NULL;

	/* overlong key and value */
	p = s;
	p += xsprintf(p, "SELECT 1 /*");
	memset(p, 'k', 100000);
	p += 100000;
	*p++ = ':';
	memset(p, 'v', 300000);
	p += 300000;
	p += xsprintf(p, ",a:");
	memset(p, 'w', 300000);
	p += 300000;
	p += xsprintf(p, "*/");
	r = run("overlong", c, s);
	CHECK(r->ntags == 1 && r->len == 2 + 64 + 1 && r->st.invalid_tags == 1,
		  "overlong: ntags %d len %zu", r->ntags, r->len);

	/* unterminated constructs */
	EXPECT("unterminated comment", c, "SELECT 1 /*a:1", "");
	EXPECT("unterminated string", c, "SELECT ' /*a:1*/", "");
	EXPECT("comment in string", c, "SELECT '/*a:1*/'", "");
	EXPECT("comment in dollar", c, "SELECT $$ /*a:1*/ $$", "");
	c.non_scs = true;
	EXPECT("non scs", c, "SELECT 'x\\' /*a:1*/'", "");
	c.non_scs = false;
	EXPECT("scs", c, "SELECT 'x\\' /*a:1*/", "a=1");

	/* empty / degenerate ranges */
	r = run_range("empty range", c, "", 0, 0);
	CHECK(r->ntags == 0, "empty range");
	r = run_range("start==end", c, "SELECT 1 /*a:1*/", 5, 5);
	CHECK(r->ntags == 0, "start==end");
	free(s);
}

/* allocation failure anywhere: empty result with oom, never a crash */
static void
test_oom(void)
{
	static const char sql[] = "/*x:0*/ SELECT 1 /*a='1',b='2'*/ /*c:3*/; /*d:4*/";
	XSpec		xs[3] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_ANY), MG(PSSC_POS_PREPEND)};
	Run			c = {0};
	Res		   *r;
	long		total;
	char		want[256];

	xs[1].merge = true;
	xs[2].merge = true;
	c.ex = mkex(3, xs);
	r = run("oom baseline", c, sql);
	total = r->allocs;
	strcpy(want, r->text);
	CHECK(strcmp(want, "c=3|d=4|x=0") == 0 && total > 0, "oom baseline: %s (%ld)",
		  want, total);
	for (long k = 1; k <= total; k++)
	{
		c.fail_at = k;
		r = run("oom", c, sql);
		CHECK(r->oom && r->ntags == 0 && r->len == 0,
			  "oom at %ld: oom %d text %s", k, r->oom, r->text);
	}
	c.fail_at = total + 1;
	r = run("oom after", c, sql);
	CHECK(!r->oom && strcmp(r->text, want) == 0, "oom after: %s", r->text);
}

/* ---------------- randomized properties ---------------- */

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint32_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t) (rng >> 16);
}

static const char *const vocab[] = {
	"a", "b", "ab", "abc", "controller", "action", "job", "zz", "B", "\xc3\xa9",
	"k%41", "x y", "", "q:", "r=", "s'", "%00", "\xff", "trace",
	"0123456789012345678901234567890123456789012345678901234567890123",
};

static void
rnd_word(char **p)
{
	const char *w = vocab[rnd() % (sizeof(vocab) / sizeof(vocab[0]))];

	*p += xsprintf(*p, "%s", w);
	if (rnd() % 3 == 0)
		*p += xsprintf(*p, "%u", rnd() % 50);
}

/* random statements: properties hold, nothing crashes */
static void
test_random(void)
{
	char		sql[8192];
	XSpec		xs[4] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND), MG(PSSC_POS_ANY),
	SC(PSSC_POS_PREPEND)};
	static const char *tagsets[] = {"*", "a,b,controller,action,job,zz", "job", ""};

	for (int iter = 0; iter < 20000; iter++)
	{
		Run			c = {0};
		char	   *p = sql;
		int			nc = rnd() % 6;
		int			nx = 1 + rnd() % 4;

		for (int i = 0; i < 4; i++)
		{
			xs[i].merge = rnd() % 2;
			xs[i].rename = (rnd() % 3 == 0) ? "a:controller|b:zz" : NULL;
			xs[i].keys = (rnd() % 4 == 0) ? "a|b|job|\xc3\xa9" : NULL;
		}
		c.ex = mkex(nx, xs);
		c.tags = tagsets[rnd() % 4];
		c.exclude = (rnd() % 2) ? "job,zz" : "";
		c.max_tags = 1 + rnd() % 10;
		c.max_value = 1 + rnd() % 20;
		c.max_bytes = 1 + rnd() % 200;
		c.window = 16 + rnd() % 300;
		c.non_scs = rnd() % 2;
		p += xsprintf(p, "%s", rnd() % 2 ? "/*lead:1*/ " : "");
		p += xsprintf(p, "SELECT %s", rnd() % 4 == 0 ? "'it''s /*x:1*/' " : "1 ");
		for (int i = 0; i < nc && p < sql + 6000; i++)
		{
			int			np = rnd() % 5;
			bool		scfmt = rnd() % 2;
			bool		line = rnd() % 5 == 0;

			p += xsprintf(p, line ? "-- " : "/* ");
			for (int j = 0; j < np; j++)
			{
				rnd_word(&p);
				p += xsprintf(p, scfmt ? "='" : ":");
				rnd_word(&p);
				p += xsprintf(p, scfmt ? "'," : ",");
			}
			p += xsprintf(p, line ? "\n" : " */ ");
			if (rnd() % 4 == 0)
				p += xsprintf(p, "; ");
		}
		*p = '\0';
		{
			size_t		len = strlen(sql);
			size_t		end = (rnd() % 3 == 0) ? rnd() % (len + 1) : len;
			Res		   *r = run_range("random", c, sql, 0, end);

			(void) r;			/* properties are checked in check_serialized */
		}
		free((void *) c.ex);
	}
}

/* random tag sets in random orders/splits: identical bytes */
static void
test_random_permutations(void)
{
	XSpec		xs[2] = {SC(PSSC_POS_ANY), MG(PSSC_POS_ANY)};
	Run			c = {0};

	xs[1].merge = true;
	c.ex = mkex(2, xs);
	for (int iter = 0; iter < 3000; iter++)
	{
		char		keys[12][16],
					vals[12][16];
		int			n = 1 + rnd() % 12,
					idx[12];
		char		first[1024];
		size_t		firstlen = 0;

		c.max_tags = 1 + rnd() % 12;
		c.max_bytes = 8 + rnd() % 120;
		c.tags = (rnd() % 2) ? "*" : "k3,k1,k7,k0,k9,k5,k11,k2";
		for (int i = 0; i < n; i++)
		{
			xsprintf(keys[i], "k%d", i);
			xsprintf(vals[i], "v%u", rnd() % 100000);
			idx[i] = i;
		}
		for (int perm = 0; perm < 6; perm++)
		{
			char		sql[2048];
			char	   *p = sql;
			Res		   *r;

			for (int i = n - 1; i > 0; i--)
			{
				int			j = rnd() % (i + 1),
							t = idx[i];

				idx[i] = idx[j];
				idx[j] = t;
			}
			p += xsprintf(p, "SELECT 1 /*");
			for (int i = 0; i < n; i++)
			{
				bool		sc = (rnd() % 2) == 0;

				/* split into a new comment now and then, either format */
				p += xsprintf(p, sc ? "*/ /*%s='%s'" : "*/ /*%s:%s", keys[idx[i]], vals[idx[i]]);
			}
			p += xsprintf(p, "*/");
			r = run("perm", c, sql);
			if (perm == 0)
			{
				memcpy(first, r->buf, r->len);
				firstlen = r->len;
			}
			else
				CHECK(r->len == firstlen && memcmp(first, r->buf, r->len) == 0,
					  "perm %d differs: %s", iter, r->text);
		}
	}
}

/* ---------------- driver ---------------- */

static void
emit_corpus(const char *dir)
{
	static const char *const samples[] = {
		"SELECT 1 /*controller:users,action:show*/",
		"SELECT 1 /*controller='users',action='show'*/",
		"/*a:1*/ SELECT 1; SELECT 2; /*b:2*/",
		"SELECT 1 /*a='%FF',b='%00',c='ok'*/",
		"SELECT 1 /*a:x\xe2\x82\xac\xe2\x82*/",
	};

	if (mkdir(dir, 0777) != 0 && errno != EEXIST)
	{
		perror(dir);
		exit(1);
	}
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
	{
		char		path[1024];
		FILE	   *f;

		snprintf(path, sizeof(path), "%s/ts-%03zu.txt", dir, i);
		f = fopen(path, "wb");
		if (!f)
		{
			perror(path);
			exit(1);
		}
		fwrite(samples[i], 1, strlen(samples[i]), f);
		fclose(f);
	}
}

/*
 * Value normalization (DESIGN.md §6.11 step 6): after rename and the
 * allowlist/denylist, before truncation; only for the configured key; a
 * failure drops the key for the statement.
 */
static int
norm_calls_for(const char *key)
{
	int			n = 0;

	for (int i = 0; i < norm_calls && i < 64; i++)
		n += strcmp(norm_keys[i], key) == 0;
	return n;
}

static void
test_normalize(void)
{
	Run			c = {0};
	Res		   *r;

	c.with_normalize = true;

	/* only the rule's key is rewritten, every match */
	norm_calls = 0;
	r = run("norm key only", c, "SELECT 1 /*route:/users/123/posts/45,action:/users/456*/");
	CHECK(strcmp(r->text, "action=/users/456|route=/users/:id/posts/:id") == 0,
		  "norm key only: %s", r->text);
	CHECK(r->st.normalized_tags == 1 && r->st.normalize_failures == 0,
		  "norm key only: counters %llu %llu",
		  (unsigned long long) r->st.normalized_tags,
		  (unsigned long long) r->st.normalize_failures);
	/* a value the rule leaves unchanged is not counted as normalized */
	r = run("norm unchanged", c, "SELECT 1 /*route:/users/me*/");
	CHECK(strcmp(r->text, "route=/users/me") == 0 && r->st.normalized_tags == 0,
		  "norm unchanged: %s %llu", r->text,
		  (unsigned long long) r->st.normalized_tags);

	/* normalization sees the renamed key, not the original one */
	{
		XSpec		xs[] = {MG(PSSC_POS_APPEND)};

		xs[0].rename = "controller:route|route:path";
		c.ex = mkex(1, xs);
		norm_calls = 0;
		EXPECT("norm after rename", c, "SELECT 1 /*controller:/u/1,route:/p/2*/",
			   "path=/p/2|route=/u/:id");
		CHECK(norm_calls_for("controller") == 0 && norm_calls_for("route") == 1,
			  "norm after rename: called for controller %d, route %d",
			  norm_calls_for("controller"), norm_calls_for("route"));
		c.ex = NULL;
	}

	/* only tags that pass the allowlist / denylist reach normalization */
	c.tags = "action";
	norm_calls = 0;
	EXPECT("norm allowlist", c, "SELECT 1 /*route:/u/1,action:a1*/", "action=a1");
	CHECK(norm_calls_for("route") == 0 && norm_calls_for("action") == 1,
		  "norm allowlist: route %d action %d", norm_calls_for("route"),
		  norm_calls_for("action"));
	c.tags = "*";
	c.exclude = "route";
	norm_calls = 0;
	EXPECT("norm denylist", c, "SELECT 1 /*route:/u/1,action:a1*/", "action=a1");
	CHECK(norm_calls_for("route") == 0, "norm denylist: route %d",
		  norm_calls_for("route"));
	c.exclude = NULL;
	/* nor does a key over 63 bytes (dropped in step 5) */
	norm_calls = 0;
	r = run("norm long key", c,
			"SELECT 1 /*k123456789012345678901234567890123456789012345678901234567890123:1*/");
	CHECK(norm_calls == 0 && r->ntags == 0, "norm long key: calls %d", norm_calls);

	/* the output is then truncated (step 7), on the normalized value */
	c.max_value = 8;
	norm_calls = 0;
	EXPECT("norm then truncate", c, "SELECT 1 /*route:/users/123456789*/", "route=/users/:");
	CHECK(norm_calls == 1 && norm_vlens[0] == 16 && norm_limits[0] == 16,
		  "norm then truncate: vlen %zu limit %zu", norm_vlens[0], norm_limits[0]);
	/* a short value may grow up to max_tag_value_len, then truncation */
	norm_calls = 0;
	EXPECT("norm limit", c, "SELECT 1 /*route:GROW*/", "route=gggggggg");
	CHECK(norm_calls == 1 && norm_limits[0] == 8, "norm limit: %zu", norm_limits[0]);
	EXPECT("norm short", c, "SELECT 1 /*route:1*/", "route=:id");
	c.max_value = 0;

	/* a failure drops the key, also its later occurrences, and is counted */
	r = run("norm fail", c, "SELECT 1 /*route:/FAIL/1,action:a,route:/ok/2*/");
	CHECK(strcmp(r->text, "action=a") == 0 && r->st.normalize_failures == 1 &&
		  r->st.normalized_tags == 0 && r->st.invalid_tags == 0 &&
		  r->st.dropped_tags == 0,
		  "norm fail: %s failures %llu", r->text,
		  (unsigned long long) r->st.normalize_failures);
	/* across extractors too: a merge extractor cannot bring it back */
	{
		XSpec		xs[] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND)};

		xs[1].merge = true;
		c.ex = mkex(2, xs);
		EXPECT("norm fail merge", c,
			   "SELECT 1 /*route='/FAIL',job='j'*/ /*route:/u/1*/", "job=j");
		/* a failed pair does not "produce": the next extractor runs */
		xs[1].merge = false;
		c.ex = mkex(2, xs);
		EXPECT("norm fail chain", c,
			   "SELECT 1 /*route='/FAIL'*/ /*action:a*/", "action=a");
		c.ex = NULL;
	}

	/* later occurrences of a normalized key lose anyway: normalized once */
	norm_calls = 0;
	EXPECT("norm first wins", c, "SELECT 1 /*route:/a/1,route:/b/2,route:/c/3*/",
		   "route=/a/:id");
	CHECK(norm_calls_for("route") == 1, "norm first wins: %d calls",
		  norm_calls_for("route"));

	/* the footer pass starts afresh */
	{
		const char *s = "SELECT 1; /*route:/f/9*/";

		r = run_range("norm footer", c, s, 0, 8);
		CHECK(r->footer && strcmp(r->text, "route=/f/:id") == 0,
			  "norm footer: %d %s", r->footer, r->text);
		/* a failure in the statement's own comments does not carry over */
		s = "SELECT 1 /*route:FAIL*/; /*route:/f/9*/";
		r = run_range("norm footer after fail", c, s, 0, 23);
		CHECK(r->footer && strcmp(r->text, "route=/f/:id") == 0 &&
			  r->st.normalize_failures == 1,
			  "norm footer after fail: %d %s", r->footer, r->text);
	}

	/* the normalized value is what gets serialized and deduplicated */
	EXPECT("norm collapse", c, "SELECT 1 /*route:/u/1*/", "route=/u/:id");
	{
		Res		   *r1;
		char		b1[64];
		size_t		l1;

		r1 = run("norm same 1", c, "SELECT 1 /*route:/u/1*/");
		l1 = r1->len;
		memcpy(b1, r1->buf, l1);
		r1 = run("norm same 2", c, "SELECT 1 /*route:/u/99999*/");
		CHECK(r1->len == l1 && memcmp(r1->buf, b1, l1) == 0,
			  "norm collapse: different values, different tag sets");
	}

	/* no callback: values untouched */
	c.with_normalize = false;
	EXPECT("norm off", c, "SELECT 1 /*route:/u/1*/", "route=/u/1");
}

/* SQL_ASCII output escaping (pssc_tag_escape, DESIGN.md §6.11). */
static void
test_escape(void)
{
	static const struct
	{
		const char *in;
		size_t		len;
		const char *out;
	}			cases[] = {
		{"", 0, ""},
		{"abc", 3, "abc"},
		{"a\\b", 3, "a\\\\b"},
		{"\xff", 1, "\\xff"},
		{"c\xc3\xa9", 3, "c\\xc3\\xa9"},
		{"\x80\x7f\\x", 4, "\\x80\x7f\\\\x"},
	};
	char		buf[64];
	size_t		i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		size_t		want = strlen(cases[i].out);
		size_t		n = pssc_tag_escaped_len(cases[i].in, cases[i].len);
		size_t		w = pssc_tag_escape(cases[i].in, cases[i].len, buf);

		CHECK(n == want && w == want && memcmp(buf, cases[i].out, want) == 0,
			  "escape case %zu: got %zu/%zu bytes", i, n, w);
	}
}

int
main(int argc, char **argv)
{
	XSpec		defaults[2] = {SC(PSSC_POS_APPEND), MG(PSSC_POS_APPEND)};

	if (argc == 3 && strcmp(argv[1], "--emit-corpus") == 0)
	{
		emit_corpus(argv[2]);
		return 0;
	}
	default_ex = mkex(2, defaults);

	test_serialization();
	test_order_independence();
	test_allow_deny();
	test_keys_and_rename();
	test_chain();
	test_first_occurrence();
	test_invalid();
	test_truncation();
	test_limits();
	test_footer();
	test_heuristic();
	test_hostile();
	test_oom();
	test_random();
	test_random_permutations();
	test_escape();
	test_normalize();

	if (failures)
	{
		fprintf(stderr, "test_tagset: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("test_tagset: all %d checks passed\n", checks);
	return 0;
}
