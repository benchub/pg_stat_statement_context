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
 * whole pipeline is O(n log n) in the number of pairs.
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

	/* 7. truncate the value on a character boundary */
	maxv = c->lim->max_tag_value_len > 0 ? (size_t) c->lim->max_tag_value_len : 0;
	if (vlen > maxv)
	{
		size_t		n = c->env->cliplen(c->env->arg, val, vlen, maxv);

		vlen = n <= maxv ? n : 0;
	}

	return add_tag(c, key, klen, val, vlen, prio);
}

/* Parse every comment of extractor e; returns true if it added a tag. */
static bool
run_extractor(Ctx *c, int index, const PsscExtractor *e)
{
	const PsscScanResult *scan;
	bool		produced = false;

	if (e->position >= NPOSITIONS ||
		(e->kind != PSSC_EXTRACTOR_SQLCOMMENTER &&
		 e->kind != PSSC_EXTRACTOR_MARGINALIA &&
		 e->kind != PSSC_EXTRACTOR_REGEX))
		return false;
	if (e->kind == PSSC_EXTRACTOR_REGEX && c->env->regex == NULL)
		return false;

	scan = get_scan(c, (PsscPosition) e->position);
	for (int i = 0; i < scan->ncomments && !c->oom; i++)
	{
		const char *span = c->s + scan->comments[i].offset;
		size_t		boff,
					blen;
		const char *body;
		PsscPairOut out;
		PsscPairResult res;

		if (!pssc_comment_body(span, scan->comments[i].len, &boff, &blen))
			continue;
		body = span + boff;
		/* pairs.h: blen bytes and (blen + 1) / 2 + 1 pairs always suffice */
		out.max_pairs = (blen + 1) / 2 + 1;
		if (e->kind == PSSC_EXTRACTOR_REGEX && out.max_pairs < e->nkeys)
			out.max_pairs = e->nkeys;
		out.bufsize = blen;
		out.pairs = ctx_alloc(c, out.max_pairs * sizeof(PsscPair));
		out.buf = ctx_alloc(c, blen > 0 ? blen : 1);
		if (c->oom)
			break;
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
		 * well-formed pair in the same comment: the comment is then in its
		 * format, while a comment without one may be another format's.
		 */
		if (res.npairs > 0)
			c->stats->invalid_tags += res.nmalformed;
		if (res.npairs > out.max_pairs)
			res.npairs = out.max_pairs;
		for (size_t j = 0; j < res.npairs && !c->oom; j++)
			produced |= process_pair(c, e, &out.pairs[j]);
	}
	return produced && !c->oom;
}

/* The extractor chain over the statement's own comments or its footer. */
static void
run_chain(Ctx *c, bool footer)
{
	bool		produced = false;

	c->footer = footer;
	memset(c->scanned, 0, sizeof(c->scanned));
	for (uint32 i = 0; i < c->ex->nextractors && !c->oom; i++)
	{
		const PsscExtractor *e = &c->ex->extractors[i];

		if (produced && !e->merge)
			continue;
		produced |= run_extractor(c, (int) i, e);
	}
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
