/*
 * tagset.c
 *		Tag-set canonicalization pipeline and extractor chain. See tagset.h
 *		for the API and the rules, and DESIGN.md §4.2, §6.5, §6.11.
 *
 * Backend-independent on purpose: it includes postgres.h only for the c.h
 * types that guc.h uses (test/unit and fuzz/ build it with -DPSSC_STANDALONE
 * and a shim instead) and calls no backend function. Scratch memory comes
 * from env->alloc and is released by the caller in bulk.
 *
 * Cost: every comment byte is lexed once per distinct position (scans are
 * shared between extractors) and parsed once per extractor that runs; every
 * surviving pair is validated in linear time and looked up linearly in the
 * extractor's keys/rename lists and the global list (bounded by the
 * check_hook). Deduplication and the drop order use two sorts, so the
 * whole pipeline is O(n log n) in the number of pairs. The appname
 * extractors run as a separate chain over application_name
 * (pssc_appname_tags_build()); their tags join the comment chain's with a
 * later sequence number, so deduplication keeps the comment's value.
 */
#ifdef PSSC_STANDALONE
#include "pssc_standalone.h"
#else
#include "postgres.h"
#endif

#include <string.h>

#include "guc.h"
#include "tagset.h"

typedef struct Tag
{
	const char *key;
	size_t		klen;
	const char *val;
	size_t		vlen;
	size_t		seq;			/* order of appearance in the chain */
	size_t		prio;			/* drop priority: lower is kept first */
	bool		keep;
} Tag;

#define NPOSITIONS 3

typedef struct Ctx
{
	const char *s;
	size_t		start;
	size_t		end;
	bool		footer;
	const PsscExtractorList *ex;
	const PsscTagList *tags;
	const PsscTagList *exclude;
	bool		match_all;
	const PsscTagsetLimits *lim;
	const PsscTagsetEnv *env;
	PsscTagsetStats *stats;

	Tag		   *tag;			/* collected candidates */
	size_t		ntag;
	size_t		cap;
	size_t		seq;
	bool		oom;

	/*
	 * Keys normalized so far in this pass (step 6), and whether that failed.
	 * At most one entry per distinct key a rule names.
	 */
	struct
	{
		const char *key;
		size_t		klen;
		bool		failed;
	}			normseen[PSSC_MAX_NORMALIZE_RULES];
	int			nnormseen;

	/* comment scans, shared by extractors with the same position */
	bool		scanned[NPOSITIONS];
	PsscScanResult scan[NPOSITIONS];
} Ctx;

static void *
ctx_alloc(Ctx *c, size_t size)
{
	void	   *p;

	if (c->oom)
		return NULL;
	p = c->env->alloc(c->env->arg, size);
	if (p == NULL)
		c->oom = true;
	return p;
}

static bool
bytes_eq(const char *a, size_t alen, const char *b, size_t blen)
{
	return alen == blen && memcmp(a, b, alen) == 0;
}

static const PsscScanResult *
get_scan(Ctx *c, PsscPosition pos)
{
	PsscScanResult *r = &c->scan[pos];

	if (!c->scanned[pos])
	{
		if (c->footer)
			pssc_scan_footer(c->s, c->end, pos, (size_t) c->lim->scan_window,
							 c->lim->standard_conforming_strings, r);
		else
			pssc_scan_statement(c->s, c->start, c->end, pos,
								(size_t) c->lim->scan_window,
								c->lim->standard_conforming_strings, r);
		if (r->heuristic)
			c->stats->heuristic_scans++;
		c->scanned[pos] = true;
	}
	return r;
}

static bool
add_tag(Ctx *c, const char *key, size_t klen, const char *val, size_t vlen,
		size_t prio)
{
	Tag		   *t;

	if (c->ntag == c->cap)
	{
		size_t		ncap = c->cap ? c->cap * 2 : 16;
		Tag		   *n = ctx_alloc(c, ncap * sizeof(Tag));

		if (n == NULL)
			return false;
		if (c->ntag)
			memcpy(n, c->tag, c->ntag * sizeof(Tag));
		c->tag = n;
		c->cap = ncap;
	}
	t = &c->tag[c->ntag++];
	t->key = key;
	t->klen = klen;
	t->val = val;
	t->vlen = vlen;
	t->seq = c->seq++;
	t->prio = prio;
	t->keep = false;
	return true;
}

/*
 * Step 6 normalization of *val for final key key. Returns false if the pair
 * must be dropped.
 */
static bool
normalize_value(Ctx *c, const char *key, size_t klen, const char **val,
				size_t *vlen, size_t maxv)
{
	const char *out = NULL;
	size_t		outlen = 0;
	size_t		limit = *vlen > maxv ? *vlen : maxv;
	PsscNormalizeResult r;

	for (int i = 0; i < c->nnormseen; i++)
	{
		if (bytes_eq(key, klen, c->normseen[i].key, c->normseen[i].klen))
			return !c->normseen[i].failed;
	}

	r = c->env->normalize(c->env->arg, key, klen, *val, *vlen, limit, &out, &outlen);
	if (r == PSSC_NORMALIZE_NO_RULES)
		return true;
	if (r == PSSC_NORMALIZE_DONE &&
		(outlen > limit || (outlen > 0 && out == NULL) ||
		 (outlen > 0 && memchr(out, '\0', outlen) != NULL) ||
		 (outlen > 0 && !c->env->verify(c->env->arg, out, outlen))))
		r = PSSC_NORMALIZE_FAILED;
	if (c->nnormseen < PSSC_MAX_NORMALIZE_RULES)
	{
		c->normseen[c->nnormseen].key = key;
		c->normseen[c->nnormseen].klen = klen;
		c->normseen[c->nnormseen].failed = r != PSSC_NORMALIZE_DONE;
		c->nnormseen++;
	}
	if (r != PSSC_NORMALIZE_DONE)
	{
		c->stats->normalize_failures++;
		return false;
	}
	if (!bytes_eq(out, outlen, *val, *vlen))
		c->stats->normalized_tags++;
	*val = outlen > 0 ? out : "";
	*vlen = outlen;
	return true;
}

/*
 * Steps 2-7 of DESIGN.md §6.11 for one pair of extractor e. Returns true if
 * a tag was added.
 */
static bool
process_pair(Ctx *c, const PsscExtractor *e, const PsscPair *p)
{
	const char *key = p->key;
	size_t		klen = p->keylen;
	const char *val = p->value;
	size_t		vlen = p->valuelen;
	size_t		prio = 0;
	size_t		maxv;

	/* 2. NUL bytes and encoding */
	if ((p->flags & (PSSC_PAIR_KEY_NUL | PSSC_PAIR_VALUE_NUL)) != 0 ||
		klen == 0 || key == NULL || (vlen > 0 && val == NULL) ||
		memchr(key, '\0', klen) != NULL ||
		(vlen > 0 && memchr(val, '\0', vlen) != NULL) ||
		!c->env->verify(c->env->arg, key, klen) ||
		(vlen > 0 && !c->env->verify(c->env->arg, val, vlen)))
	{
		c->stats->invalid_tags++;
		return false;
	}
	if (vlen == 0)
		val = "";

	/* 3. per-extractor keys, by original name (regex keys name captures) */
	if (e->kind != PSSC_EXTRACTOR_REGEX && e->has_keys)
	{
		const PsscBlobStr *keys = pssc_extractor_keys(c->ex, e);
		bool		found = false;

		for (uint32 i = 0; i < e->nkeys && !found; i++)
			found = bytes_eq(key, klen, pssc_blob_str(c->ex, keys[i]), keys[i].len);
		if (!found)
			return false;
	}

	/* 4. rename */
	if (e->nrename > 0)
	{
		const PsscBlobRename *ren = pssc_extractor_renames(c->ex, e);

		for (uint32 i = 0; i < e->nrename; i++)
		{
			if (bytes_eq(key, klen, pssc_blob_str(c->ex, ren[i].from), ren[i].from.len))
			{
				key = pssc_blob_str(c->ex, ren[i].to);
				klen = ren[i].to.len;
				/* config bytes, never checked against this encoding */
				if (!c->env->verify(c->env->arg, key, klen))
				{
					c->stats->invalid_tags++;
					return false;
				}
				break;
			}
		}
	}

	/*
	 * 5. global allowlist, or the denylist with tags = '*'. List keys are at
	 * most PSSC_MAX_KEY_LEN bytes, so a longer key can match neither.
	 */
	if (c->match_all)
	{
		if (klen <= PSSC_MAX_KEY_LEN &&
			pssc_tag_list_find(c->exclude, key, (int) klen) >= 0)
			return false;
	}
	else
	{
		int			pos = -1;

		if (klen <= PSSC_MAX_KEY_LEN)
			pos = pssc_tag_list_find(c->tags, key, (int) klen);
		if (pos < 0)
			return false;
		prio = (size_t) pos;
	}

	/* 6. key length */
	if (klen > PSSC_MAX_KEY_LEN)
	{
		c->stats->invalid_tags++;
		return false;
	}

	maxv = c->lim->max_tag_value_len > 0 ? (size_t) c->lim->max_tag_value_len : 0;

	/* 6. normalize the value */
	if (c->env->normalize != NULL && !normalize_value(c, key, klen, &val, &vlen, maxv))
		return false;

	/* 7. truncate the value on a character boundary */
	if (vlen > maxv)
	{
		size_t		n = c->env->cliplen(c->env->arg, val, vlen, maxv);

		vlen = n <= maxv ? n : 0;
	}

	return add_tag(c, key, klen, val, vlen, prio);
}

/* True if extractor e has a kind this code knows (and can run). */
static bool
runnable(const Ctx *c, const PsscExtractor *e)
{
	if (e->kind != PSSC_EXTRACTOR_SQLCOMMENTER &&
		e->kind != PSSC_EXTRACTOR_MARGINALIA &&
		e->kind != PSSC_EXTRACTOR_REGEX)
		return false;
	return e->kind != PSSC_EXTRACTOR_REGEX || c->env->regex != NULL;
}

/*
 * Parse body[0, blen) (a comment body, or application_name) with extractor
 * e's format and run its pairs through steps 2-7; returns true if it added
 * a tag.
 */
static bool
parse_body(Ctx *c, int index, const PsscExtractor *e, const char *body,
		   size_t blen)
{
	PsscPairOut out;
	PsscPairResult res;
	bool		produced = false;

	/* pairs.h: blen bytes and (blen + 1) / 2 + 1 pairs always suffice */
	out.max_pairs = (blen + 1) / 2 + 1;
	if (e->kind == PSSC_EXTRACTOR_REGEX && out.max_pairs < e->nkeys)
		out.max_pairs = e->nkeys;
	out.bufsize = blen;
	out.pairs = ctx_alloc(c, out.max_pairs * sizeof(PsscPair));
	out.buf = ctx_alloc(c, blen > 0 ? blen : 1);
	if (c->oom)
		return false;
	memset(&res, 0, sizeof(res));
	switch (e->kind)
	{
		case PSSC_EXTRACTOR_SQLCOMMENTER:
			pssc_parse_sqlcommenter(body, blen, e->url_decode, &out, &res);
			break;
		case PSSC_EXTRACTOR_MARGINALIA:
			{
				PsscMarginaliaOpts opts;

				opts.kv_sep = pssc_blob_str(c->ex, e->kv_sep);
				opts.kv_sep_len = e->kv_sep.len;
				opts.pair_sep = pssc_blob_str(c->ex, e->pair_sep);
				opts.pair_sep_len = e->pair_sep.len;
				pssc_parse_marginalia(body, blen, &opts, &out, &res);
			}
			break;
		default:
			c->env->regex(c->env->arg, index, c->ex, body, blen, &out, &res);
			break;
	}
	c->stats->dropped_tags += res.ndropped;

	/*
	 * Malformed segments count as invalid only if this parser found a
	 * well-formed pair in the same body: the body is then in its format,
	 * while one without may be another format's (or, for application_name,
	 * a plain name such as "psql").
	 */
	if (res.npairs > 0)
		c->stats->invalid_tags += res.nmalformed;
	if (res.npairs > out.max_pairs)
		res.npairs = out.max_pairs;
	for (size_t j = 0; j < res.npairs && !c->oom; j++)
		produced |= process_pair(c, e, &out.pairs[j]);
	return produced && !c->oom;
}

/* Parse every comment of extractor e; returns true if it added a tag. */
static bool
run_extractor(Ctx *c, int index, const PsscExtractor *e)
{
	const PsscScanResult *scan;
	bool		produced = false;

	if (e->position >= NPOSITIONS || !runnable(c, e))
		return false;

	scan = get_scan(c, (PsscPosition) e->position);
	for (int i = 0; i < scan->ncomments && !c->oom; i++)
	{
		const char *span = c->s + scan->comments[i].offset;
		size_t		boff,
					blen;

		if (!pssc_comment_body(span, scan->comments[i].len, &boff, &blen))
			continue;
		produced |= parse_body(c, index, e, span + boff, blen);
	}
	return produced && !c->oom;
}

/*
 * The comment extractor chain over the statement's own comments or its
 * footer. appname extractors are not part of it.
 */
static void
run_chain(Ctx *c, bool footer)
{
	bool		produced = false;

	c->footer = footer;
	c->nnormseen = 0;
	memset(c->scanned, 0, sizeof(c->scanned));
	for (uint32 i = 0; i < c->ex->nextractors && !c->oom; i++)
	{
		const PsscExtractor *e = &c->ex->extractors[i];

		if (e->source != PSSC_SOURCE_COMMENT)
			continue;
		if (produced && !e->merge)
			continue;
		produced |= run_extractor(c, (int) i, e);
	}
}

/* The appname extractor chain over application_name. */
static void
run_appname_chain(Ctx *c, const char *appname, size_t len)
{
	bool		produced = false;

	c->nnormseen = 0;
	for (uint32 i = 0; i < c->ex->nextractors && !c->oom; i++)
	{
		const PsscExtractor *e = &c->ex->extractors[i];

		if (e->source != PSSC_SOURCE_APPNAME || !runnable(c, e))
			continue;
		if (produced && !e->merge)
			continue;
		produced |= parse_body(c, (int) i, e, appname, len);
	}
}

static void
stats_add(PsscTagsetStats *dst, const PsscTagsetStats *src)
{
	dst->invalid_tags += src->invalid_tags;
	dst->dropped_tags += src->dropped_tags;
	dst->heuristic_scans += src->heuristic_scans;
	dst->regex_compile_failures += src->regex_compile_failures;
	dst->normalized_tags += src->normalized_tags;
	dst->normalize_failures += src->normalize_failures;
}

static int
cmp_key_seq(const void *a, const void *b)
{
	const Tag  *x = a;
	const Tag  *y = b;
	size_t		m = x->klen < y->klen ? x->klen : y->klen;
	int			r = memcmp(x->key, y->key, m);

	if (r != 0)
		return r;
	if (x->klen != y->klen)
		return x->klen < y->klen ? -1 : 1;
	if (x->seq != y->seq)
		return x->seq < y->seq ? -1 : 1;
	return 0;
}

static int
cmp_prio(const void *a, const void *b)
{
	const Tag  *x = *(Tag *const *) a;
	const Tag  *y = *(Tag *const *) b;

	if (x->prio != y->prio)
		return x->prio < y->prio ? -1 : 1;
	return 0;
}

void
pssc_tagset_build(const char *s, size_t start, size_t end,
				  const struct PsscExtractorList *extractors,
				  const struct PsscTagList *tags,
				  const struct PsscTagList *exclude_tags,
				  const PsscTagsetLimits *limits,
				  const PsscTagsetEnv *env,
				  const PsscAppnameTags *appname,
				  PsscTagsetOut *out,
				  PsscTagsetStats *stats)
{
	Ctx			c;
	size_t		n = 0;
	size_t		maxtags,
				maxbytes,
				used = 0,
				kept = 0;
	Tag		  **order;

	out->len = 0;
	out->ntags = 0;
	out->footer = false;
	out->oom = false;

	memset(&c, 0, sizeof(c));
	c.s = s;
	c.start = start <= end ? start : end;
	c.end = end;
	c.ex = extractors;
	c.tags = tags;
	c.exclude = exclude_tags;
	c.match_all = pssc_tag_list_match_all(tags);
	c.lim = limits;
	c.env = env;
	c.stats = stats;

	run_chain(&c, false);
	if (c.ntag == 0 && !c.oom)
	{
		run_chain(&c, true);
		out->footer = c.ntag > 0;
	}

	/*
	 * application_name tags after the comment tags (higher seq): the
	 * deduplication below keeps the comment's value of a key.
	 */
	if (appname != NULL)
	{
		stats_add(stats, &appname->stats);
		if (appname->oom)
			c.oom = true;
		for (size_t i = 0; i < appname->ntags && !c.oom; i++)
		{
			const PsscTagCandidate *t = &appname->tags[i];

			add_tag(&c, t->key, t->klen, t->val, t->vlen, t->prio);
		}
	}
	if (c.ntag == 0 || c.oom)
		goto done;

	/* Sort by key, first occurrence first, and keep one tag per key. */
	qsort(c.tag, c.ntag, sizeof(Tag), cmp_key_seq);
	for (size_t i = 0; i < c.ntag; i++)
	{
		if (n > 0 && bytes_eq(c.tag[n - 1].key, c.tag[n - 1].klen,
							  c.tag[i].key, c.tag[i].klen))
			continue;
		c.tag[n++] = c.tag[i];
	}

	/*
	 * Greedy fill in priority order: keep each tag that still fits both
	 * limits, drop the others.
	 */
	order = ctx_alloc(&c, n * sizeof(Tag *));
	if (order == NULL)
		goto done;
	for (size_t i = 0; i < n; i++)
		order[i] = &c.tag[i];
	/* with tags = '*' the priority is the sorted key order: nothing to do */
	if (!c.match_all)
		qsort(order, n, sizeof(Tag *), cmp_prio);
	maxtags = limits->max_tags > 0 ? (size_t) limits->max_tags : 0;
	maxbytes = limits->max_tagset_bytes > 0 ? (size_t) limits->max_tagset_bytes : 0;
	for (size_t i = 0; i < n; i++)
	{
		size_t		need = order[i]->klen + 1 + order[i]->vlen + 1;

		if (kept >= maxtags)
			break;
		if (need > maxbytes - used)
			continue;
		order[i]->keep = true;
		used += need;
		kept++;
	}
	stats->dropped_tags += n - kept;

	/* Serialize the kept tags in key order. */
	for (size_t i = 0; i < n; i++)
	{
		const Tag  *t = &c.tag[i];

		if (!t->keep)
			continue;
		memcpy(out->buf + out->len, t->key, t->klen);
		out->len += t->klen;
		out->buf[out->len++] = '\0';
		if (t->vlen > 0)
			memcpy(out->buf + out->len, t->val, t->vlen);
		out->len += t->vlen;
		out->buf[out->len++] = '\0';
		out->ntags++;
	}

done:
	if (c.oom)
	{
		out->len = 0;
		out->ntags = 0;
		out->footer = false;
		out->oom = true;
	}
}

bool
pssc_extractors_have_appname(const PsscExtractorList *list)
{
	for (uint32 i = 0; i < list->nextractors; i++)
		if (list->extractors[i].source == PSSC_SOURCE_APPNAME)
			return true;
	return false;
}

void
pssc_appname_tags_build(const char *appname, size_t len,
						const PsscExtractorList *extractors,
						const PsscTagList *tags,
						const PsscTagList *exclude_tags,
						const PsscTagsetLimits *limits,
						const PsscTagsetEnv *env,
						PsscAppnameTags *out)
{
	Ctx			c;
	PsscTagCandidate *cand;

	memset(out, 0, sizeof(*out));
	if (len == 0)
		return;

	memset(&c, 0, sizeof(c));
	c.s = appname;
	c.ex = extractors;
	c.tags = tags;
	c.exclude = exclude_tags;
	c.match_all = pssc_tag_list_match_all(tags);
	c.lim = limits;
	c.env = env;
	c.stats = &out->stats;

	run_appname_chain(&c, appname, len);
	if (c.ntag > 0 && !c.oom)
	{
		cand = ctx_alloc(&c, c.ntag * sizeof(PsscTagCandidate));
		if (cand != NULL)
		{
			for (size_t i = 0; i < c.ntag; i++)
			{
				cand[i].key = c.tag[i].key;
				cand[i].klen = c.tag[i].klen;
				cand[i].val = c.tag[i].val;
				cand[i].vlen = c.tag[i].vlen;
				cand[i].prio = c.tag[i].prio;
			}
			out->tags = cand;
			out->ntags = c.ntag;
		}
	}
	if (c.oom)
	{
		out->tags = NULL;
		out->ntags = 0;
		out->oom = true;
	}
}

size_t
pssc_tag_escaped_len(const char *s, size_t len)
{
	size_t		n = 0;
	size_t		i;

	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) s[i];

		n += c >= 0x80 ? 4 : c == '\\' ? 2 : 1;
	}
	return n;
}

size_t
pssc_tag_escape(const char *s, size_t len, char *dst)
{
	static const char hex[] = "0123456789abcdef";
	char	   *d = dst;
	size_t		i;

	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) s[i];

		if (c >= 0x80)
		{
			*d++ = '\\';
			*d++ = 'x';
			*d++ = hex[c >> 4];
			*d++ = hex[c & 0xf];
		}
		else if (c == '\\')
		{
			*d++ = '\\';
			*d++ = '\\';
		}
		else
			*d++ = (char) c;
	}
	return (size_t) (d - dst);
}
