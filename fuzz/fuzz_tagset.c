/*
 * fuzz_tagset.c
 *		libFuzzer entry point for the tag-set pipeline, src/tagset.c
 *		(DESIGN.md §4.2, §6.5, §6.11): scanner + SQLCommenter/marginalia
 *		parsers + extractor chain + limits + serialization, built standalone
 *		(-DPSSC_STANDALONE) like test/unit/test_tagset.c, with scan.c and
 *		pairs.c bounds-checked (-DPSSC_SCAN_CHECKED, -DPSSC_PAIRS_CHECKED).
 *		The input is a statement string. Every input runs through a fixed set
 *		of configurations (positions, merge, keys/rename, custom separators,
 *		tags = '*' with a denylist, a fake regex extractor, small and large
 *		limits, all within the GUC bounds) and both a UTF-8 and a SQL_ASCII
 *		environment, on the whole string and on a sub-range.
 *
 * Invariants checked on every result:
 *	- at most max_tags tags and max_tagset_bytes bytes, written into an
 *	  exactly sized buffer (ASan catches overruns);
 *	- the bytes are key \0 value \0 ... with ntags pairs, keys 1..63 bytes,
 *	  strictly increasing (sorted, unique), values <= max_tag_value_len,
 *	  keys and values without NUL and valid in the environment's encoding;
 *	- keys are in the allowlist, or not in the denylist when tags = '*';
 *	- the same call again gives the same bytes and counters;
 *	- an allocation failure at any point gives an empty set with oom set.
 */
#include "pssc_standalone.h"

#include "fuzz_check.h"
#include "guc.h"
#include "tagset.h"

/* ---------------- guc.h tag lists ---------------- */

#define MAXLIST 8

struct PsscTagList
{
	bool		match_all;
	int			n;
	const char *keys[MAXLIST];
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
	if (len)
		*len = (int) strlen(list->keys[i]);
	return list->keys[i];
}

int
pssc_tag_list_find(const PsscTagList *list, const char *key, int len)
{
	for (int i = 0; i < list->n; i++)
		if ((int) strlen(list->keys[i]) == len && memcmp(list->keys[i], key, len) == 0)
			return i;
	return -1;
}

/* ---------------- PsscExtractorList blobs ---------------- */

typedef struct XSpec
{
	PsscExtractorKind kind;
	PsscPosition position;
	bool		merge;
	bool		no_url_decode;
	const char *keys;			/* "a|b" or NULL; regex: capture names */
	const char *rename;			/* "a:b|c:d" or NULL */
	const char *kv_sep;			/* marginalia; NULL = ":" */
	const char *pair_sep;		/* marginalia; NULL = "," */
} XSpec;

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
	FUZZ_CHECK(b->used <= b->cap);
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

/* split "a|b|c" into PsscBlobStr[] */
static uint32
blob_split(Blob *b, const char *spec, uint32 *n)
{
	int			cnt = 1;
	uint32		off;
	const char *p = spec;

	for (const char *q = spec; *q; q++)
		cnt += (*q == '|');
	off = blob_reserve(b, cnt * sizeof(PsscBlobStr));
	for (int i = 0; i < cnt; i++)
	{
		const char *e = strchr(p, '|');
		size_t		len = e ? (size_t) (e - p) : strlen(p);
		PsscBlobStr s = blob_str(b, p, len);

		((PsscBlobStr *) (b->base + off))[i] = s;
		p += len + (e != NULL);
	}
	*n = (uint32) cnt;
	return off;
}

static PsscExtractorList *
mkex(int n, const XSpec *specs)
{
	Blob		b;

	b.cap = 1 << 14;
	b.base = calloc(1, b.cap);
	FUZZ_CHECK(b.base);
	b.used = offsetof(PsscExtractorList, extractors) + n * sizeof(PsscExtractor);
	for (int i = 0; i < n; i++)
	{
		const XSpec *x = &specs[i];
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
		((PsscExtractorList *) b.base)->extractors[i] = tmp;
	}
	((PsscExtractorList *) b.base)->nextractors = (uint32) n;
	((PsscExtractorList *) b.base)->size = (uint32) b.used;
	return (PsscExtractorList *) b.base;
}

/* ---------------- environment ---------------- */

typedef struct Arena
{
	void	   *ptrs[1024];
	int			n;
	long		fail_at;		/* fail the allocation with this index (-1 never) */
	long		count;
} Arena;

typedef struct Env
{
	Arena		arena;
	bool		utf8;			/* else SQL_ASCII: every byte string is valid */
} Env;

static void *
env_alloc(void *arg, size_t size)
{
	Arena	   *a = &((Env *) arg)->arena;
	void	   *p;

	if (a->count++ == a->fail_at)
		return NULL;
	FUZZ_CHECK(a->n < 1024);
	p = malloc(size ? size : 1);
	FUZZ_CHECK(p);
	memset(p, 0xa5, size);		/* the pipeline must not rely on zeroed memory */
	a->ptrs[a->n++] = p;
	return p;
}

static void
arena_reset(Arena *a, long fail_at)
{
	for (int i = 0; i < a->n; i++)
		free(a->ptrs[i]);
	a->n = 0;
	a->count = 0;
	a->fail_at = fail_at;
}

/* strict UTF-8 (no overlongs, no surrogates, <= U+10FFFF) */
static bool
utf8_valid(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t		i = 0;

	while (i < len)
	{
		unsigned char c = p[i];
		int			n;
		uint32_t	cp;

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

static bool
env_verify(void *arg, const char *s, size_t len)
{
	/* contract: NUL is rejected before verify is called */
	FUZZ_CHECK(!memchr(s, 0, len));
	return !((Env *) arg)->utf8 || utf8_valid(s, len);
}

static size_t
env_cliplen(void *arg, const char *s, size_t len, size_t limit)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t		n = limit;

	FUZZ_CHECK(len > limit);
	if (!((Env *) arg)->utf8)
		return limit;
	while (n > 0 && (p[n] & 0xc0) == 0x80)
		n--;
	return n;
}

/*
 * Fake regex extractor: capture key i gets the i-th space-separated word of
 * the body (pointing into the body, so it may hold NUL or invalid bytes);
 * every other key's value is copied into out->buf while it fits, to
 * exercise the scratch buffer.
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
	FUZZ_CHECK(e->kind == PSSC_EXTRACTOR_REGEX);
	memset(result, 0, sizeof(*result));
	for (uint32 k = 0; k < e->nkeys && result->npairs < out->max_pairs; k++)
	{
		size_t		st;
		PsscPair   *p = &out->pairs[result->npairs];

		while (i < len && body[i] == ' ')
			i++;
		if (i >= len)
			break;
		st = i;
		while (i < len && body[i] != ' ')
			i++;
		p->key = pssc_blob_str(list, keys[k]);
		p->keylen = keys[k].len;
		p->value = body + st;
		p->valuelen = i - st;
		p->flags = 0;
		if (k % 2 && result->bufused + p->valuelen <= out->bufsize)
		{
			memcpy(out->buf + result->bufused, body + st, p->valuelen);
			p->value = out->buf + result->bufused;
			result->bufused += p->valuelen;
		}
		result->npairs++;
	}
}

/* ---------------- configurations ---------------- */

#define SC(pos)	{PSSC_EXTRACTOR_SQLCOMMENTER, pos}
#define MG(pos)	{PSSC_EXTRACTOR_MARGINALIA, pos}
#define A PSSC_POS_APPEND
#define P PSSC_POS_PREPEND
#define Y PSSC_POS_ANY

typedef struct Config
{
	int			nx;
	XSpec		x[4];
	const char *tags;			/* "a,b" or "*" */
	const char *exclude;
	PsscTagsetLimits limits;
} Config;

static const Config configs[] = {
	/* the defaults */
	{2, {SC(A), MG(A)}, "action,controller,job", "",
	{8, 64, 512, 2048, true}},
	/* every position, keys + rename, merge, custom separators, tiny limits */
	{3, {{PSSC_EXTRACTOR_SQLCOMMENTER, P, false, true, "a|controller|b", "a:action|b:job"},
		{PSSC_EXTRACTOR_MARGINALIA, Y, true, false, NULL, "route:controller", "=", " "},
	MG(A)},
	"*", "traceparent,request_id", {2, 3, 128, 64, false}},
	/* fake regex with merge, '*' with everything, the largest limits */
	{4, {{PSSC_EXTRACTOR_MARGINALIA, P, false, false, NULL, NULL, "=>", ";"},
		{PSSC_EXTRACTOR_REGEX, Y, true, false, "k1|k2|k3"},
		{PSSC_EXTRACTOR_SQLCOMMENTER, Y, true},
	{PSSC_EXTRACTOR_MARGINALIA, Y, true, false, NULL, NULL, " :", ","}},
	"*", "", {64, 4096, 8192, 1024 * 1024, true}},
	/* many small tags against a small byte budget: drops in priority order */
	{2, {{PSSC_EXTRACTOR_MARGINALIA, Y, false}, {PSSC_EXTRACTOR_SQLCOMMENTER, Y, true}},
		"*", "a", {64, 64, 128, 64, false}},
	/* allowlist order decides drops; renames collide with existing keys */
	{2, {{PSSC_EXTRACTOR_SQLCOMMENTER, A, false, false, NULL, "x:job|job:x"},
		{PSSC_EXTRACTOR_MARGINALIA, A, true, false, "job|action|x", "action:job"}},
	"job,x,controller,action", "", {3, 1, 128, 100, true}},
};

#define NCONFIGS (sizeof(configs) / sizeof(configs[0]))

static PsscTagList *
mklist(const char *spec)
{
	PsscTagList *l = calloc(1, sizeof(PsscTagList));
	const char *p = spec;

	FUZZ_CHECK(l);
	if (strcmp(spec, "*") == 0)
	{
		l->match_all = true;
		return l;
	}
	while (*p)
	{
		const char *e = strchr(p, ',');
		size_t		n = e ? (size_t) (e - p) : strlen(p);
		char	   *k = calloc(1, n + 1);

		FUZZ_CHECK(k && l->n < MAXLIST);
		memcpy(k, p, n);
		l->keys[l->n++] = k;
		p += n + (*(p + n) == ',');
	}
	return l;
}

typedef struct Prepared
{
	PsscExtractorList *ex;
	PsscTagList *tags;
	PsscTagList *exclude;
} Prepared;

static Prepared prepared[NCONFIGS];

static void
prepare(void)
{
	static bool done;

	if (done)
		return;
	for (size_t i = 0; i < NCONFIGS; i++)
	{
		prepared[i].ex = mkex(configs[i].nx, configs[i].x);
		prepared[i].tags = mklist(configs[i].tags);
		prepared[i].exclude = mklist(configs[i].exclude);
	}
	done = true;
}

/* ---------------- checks ---------------- */

/* Returns the number of null values. */
static int
check_output(const Config *c, const Prepared *pr, const Env *env,
			 const PsscTagsetOut *out, bool capped)
{
	int			nnull = 0;
	const char *prevkey = NULL;
	size_t		prevlen = 0;
	size_t		off = 0;
	int			n = 0;

	FUZZ_CHECK(!out->oom);
	FUZZ_CHECK(out->ntags >= 0 && out->ntags <= c->limits.max_tags);
	FUZZ_CHECK(out->len <= (size_t) c->limits.max_tagset_bytes);
	FUZZ_CHECK((out->ntags == 0) == (out->len == 0));
	while (off < out->len)
	{
		const char *k,
				   *v;
		size_t		kl,
					vl;
		PsscTagView t;

		FUZZ_CHECK(pssc_tagset_next(out->buf, out->len, &off, &t));
		k = t.key;
		kl = t.klen;
		v = t.isnull ? "" : t.val;
		vl = t.vlen;
		n++;
		/* only a cap hook produces nulls (§6.11 step 8) */
		FUZZ_CHECK(!t.isnull || capped);
		nnull += t.isnull;

		FUZZ_CHECK(kl >= 1 && kl <= PSSC_MAX_KEY_LEN);
		FUZZ_CHECK(vl <= (size_t) c->limits.max_tag_value_len);
		FUZZ_CHECK(!env->utf8 || (utf8_valid(k, kl) && utf8_valid(v, vl)));
		if (prevkey)
		{
			int			cmp = memcmp(prevkey, k, prevlen < kl ? prevlen : kl);

			FUZZ_CHECK(cmp < 0 || (cmp == 0 && prevlen < kl));
		}
		prevkey = k;
		prevlen = kl;
		if (pr->tags->match_all)
			FUZZ_CHECK(pssc_tag_list_find(pr->exclude, k, (int) kl) < 0);
		else
			FUZZ_CHECK(pssc_tag_list_find(pr->tags, k, (int) kl) >= 0);
	}
	FUZZ_CHECK(n == out->ntags);
	return nnull;
}

/*
 * A deterministic, stateless cap for step 8: collapses values by their
 * first byte and length (odd first byte: "over the cap"; length 3: "table
 * full"); keys starting with 'k' are uncapped.
 */
static PsscCapResult
fuzz_cap(void *arg, const char *key, size_t klen, const char *val,
		 size_t vlen, bool admit)
{
	(void) arg;
	(void) admit;
	if (klen > 0 && key[0] == 'k')
		return PSSC_CAP_KEEP;
	if (vlen == 3)
		return PSSC_CAP_NULL_FULL;
	if (vlen > 0 && (val[0] & 1))
		return PSSC_CAP_NULL;
	return PSSC_CAP_KEEP;
}

static void
run_one(const char *s, size_t start, size_t end, size_t ci, Env *env,
		uint64_t h)
{
	const Config *c = &configs[ci];
	const Prepared *pr = &prepared[ci];
	PsscTagsetEnv te = {env, env_alloc, env_verify, env_cliplen, fake_regex};
	size_t		bufsize = (size_t) c->limits.max_tagset_bytes;
	char	   *buf = malloc(bufsize ? bufsize : 1);
	char	   *buf2 = malloc(bufsize ? bufsize : 1);
	PsscTagsetOut out = {buf},
				out2 = {buf2};
	PsscTagsetStats st = {0},
				st2 = {0};
	long		nallocs;

	FUZZ_CHECK(buf && buf2);
	arena_reset(&env->arena, -1);
	pssc_tagset_build(s, start, end, pr->ex, pr->tags, pr->exclude,
					  &c->limits, &te, NULL, &out, &st);
	nallocs = env->arena.count;
	check_output(c, pr, env, &out, false);

	/* deterministic */
	arena_reset(&env->arena, -1);
	pssc_tagset_build(s, start, end, pr->ex, pr->tags, pr->exclude,
					  &c->limits, &te, NULL, &out2, &st2);
	FUZZ_CHECK(out2.len == out.len && out2.ntags == out.ntags &&
			   out2.footer == out.footer && memcmp(buf, buf2, out.len) == 0);
	FUZZ_CHECK(memcmp(&st, &st2, sizeof(st)) == 0);

	/* an allocation failure anywhere yields an empty set flagged oom */
	if (nallocs > 0)
	{
		PsscTagsetStats st3 = {0};

		arena_reset(&env->arena, (long) (h % (uint64_t) nallocs));
		memset(&out2, 0, sizeof(out2));
		out2.buf = buf2;
		pssc_tagset_build(s, start, end, pr->ex, pr->tags, pr->exclude,
						  &c->limits, &te, NULL, &out2, &st3);
		FUZZ_CHECK(out2.oom && out2.len == 0 && out2.ntags == 0);
	}

	/*
	 * step 8 with a stateless fake cap: the output stays well-formed, nulls
	 * appear only through it and are exactly the counted ones, and it never
	 * adds tags
	 */
	{
		PsscTagsetEnv tc = te;
		PsscTagsetStats st4 = {0};

		tc.cap = fuzz_cap;
		arena_reset(&env->arena, -1);
		memset(&out2, 0, sizeof(out2));
		out2.buf = buf2;
		pssc_tagset_build(s, start, end, pr->ex, pr->tags, pr->exclude,
						  &c->limits, &tc, NULL, &out2, &st4);
		/* every counted collapse is in the output (and only those) */
		FUZZ_CHECK(check_output(c, pr, env, &out2, true) ==
				   (int) st4.capped_tags);
		FUZZ_CHECK(st4.cap_table_full <= st4.capped_tags);
	}
	arena_reset(&env->arena, -1);
	free(buf);
	free(buf2);
}

FUZZ_ALPHABET("/*-'$ \n;:=,%2Faxk\\\0\xc3\xa9\xff")

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t		len = strnlen((const char *) data, size);
	char	   *s = malloc(len + 1);
	uint64_t	h = 1469598103934665603ULL;
	Env			env;

	prepare();
	FUZZ_CHECK(s);
	memcpy(s, data, len);
	s[len] = '\0';
	for (size_t i = 0; i < len; i++)
		h = (h ^ (unsigned char) s[i]) * 1099511628211ULL;
	memset(&env, 0, sizeof(env));
	for (size_t ci = 0; ci < NCONFIGS; ci++)
	{
		size_t		a = (size_t) (h % (len + 1)),
					b = a + (size_t) ((h >> 13) % (len - a + 1));

		env.utf8 = (ci + (h >> 40)) % 2 == 0;
		run_one(s, 0, len, ci, &env, h >> 3);
		run_one(s, a, b, ci, &env, h >> 7);
	}
	free(s);
	return 0;
}
