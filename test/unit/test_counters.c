/*
 * test_counters.c
 *		Standalone unit tests for src/counters.c, the per-bucket counter slot
 *		(DESIGN.md §5.1, §5.2, §7) and the pgss-style usage (§5.3). Built
 *		with -DPSSC_STANDALONE under ASan/UBSan (no server needed).
 *
 * All double comparisons are exact: the functions must compute precisely
 * the documented expressions (left-to-right sums, s * 1000.0 as in pgss,
 * usage * 0.99), not merely something close.
 */
#include "pssc_standalone.h"

#include <stdio.h>

#include "counters.h"

static int	failures;
static int	checks;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			fprintf(stderr, "not ok (%s:%d): ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\n"); \
		} \
	} while (0)

/* Bitwise equality, so -0.0 vs 0.0 and NaNs are not glossed over. */
static bool
same_double(double a, double b)
{
	return memcmp(&a, &b, sizeof(double)) == 0;
}

static void
fill_garbage(PsscSlot *slot)
{
	slot->bucket_id = 77;
	slot->calls = 12345;
	slot->total_exec_time = 9.75;
}

static void
test_layout(void)
{
	/* §5.1: two counters plus the label, ~24 bytes per bucket */
	CHECK(sizeof(PsscSlot) == 24, "sizeof(PsscSlot) = %zu", sizeof(PsscSlot));
	CHECK(PSSC_BUCKET_NONE < (int64) -1000000000000LL, "PSSC_BUCKET_NONE is below any real id");
}

static void
test_init(void)
{
	PsscSlot	s;

	fill_garbage(&s);
	pssc_slot_init(&s);
	CHECK(s.bucket_id == PSSC_BUCKET_NONE, "init: bucket_id %lld", (long long) s.bucket_id);
	CHECK(s.calls == 0, "init: calls %lld", (long long) s.calls);
	CHECK(same_double(s.total_exec_time, 0.0), "init: total %g", s.total_exec_time);
}

static void
test_relabel(void)
{
	PsscSlot	s;
	const int64 ids[] = {0, 1, 41, -3, INT64_MAX};
	size_t		i;

	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
	{
		fill_garbage(&s);
		pssc_slot_relabel(&s, ids[i]);
		CHECK(s.bucket_id == ids[i], "relabel %lld: got %lld",
			  (long long) ids[i], (long long) s.bucket_id);
		CHECK(s.calls == 0, "relabel zeroes calls (%lld)", (long long) s.calls);
		CHECK(same_double(s.total_exec_time, 0.0), "relabel zeroes total (%g)",
			  s.total_exec_time);
	}
}

static void
test_roll(void)
{
	PsscSlot	s;
	bool		r;

	/* empty slot: relabeled */
	pssc_slot_init(&s);
	r = pssc_slot_roll(&s, 12);
	CHECK(r, "roll of an empty slot relabels");
	CHECK(s.bucket_id == 12 && s.calls == 0, "roll of empty: %lld/%lld",
		  (long long) s.bucket_id, (long long) s.calls);

	/* same bucket: counters kept */
	pssc_slot_accum(&s, 2.5);
	r = pssc_slot_roll(&s, 12);
	CHECK(!r, "roll to the same bucket does not relabel");
	CHECK(s.bucket_id == 12 && s.calls == 1 && same_double(s.total_exec_time, 2.5),
		  "roll to the same bucket keeps counters: %lld/%lld/%g",
		  (long long) s.bucket_id, (long long) s.calls, s.total_exec_time);

	/* older bucket (one ring of 12 later): zeroed and relabeled */
	r = pssc_slot_roll(&s, 24);
	CHECK(r, "roll to a newer bucket relabels");
	CHECK(s.bucket_id == 24 && s.calls == 0 && same_double(s.total_exec_time, 0.0),
		  "roll to a newer bucket zeroes: %lld/%lld/%g",
		  (long long) s.bucket_id, (long long) s.calls, s.total_exec_time);

	/* signed comparison: -1 is older than 0 */
	pssc_slot_relabel(&s, -1);
	pssc_slot_accum(&s, 1.0);
	r = pssc_slot_roll(&s, 0);
	CHECK(r && s.bucket_id == 0 && s.calls == 0, "roll from -1 to 0 relabels");
}

static void
test_accum(void)
{
	PsscSlot	s;
	const double v[] = {0.125, 3.5, 1e-3, 0.1, 1234.5678, 0.0, 7e-9, 1e6, 0.3};
	const int	nv = (int) (sizeof(v) / sizeof(v[0]));
	volatile double ref = 0.0;
	int			round,
				i;

	pssc_slot_relabel(&s, 5);
	for (round = 0; round < 1000; round++)
		for (i = 0; i < nv; i++)
		{
			pssc_slot_accum(&s, v[i]);
			ref += v[i];
		}
	CHECK(s.calls == 1000 * nv, "accum calls %lld", (long long) s.calls);
	CHECK(same_double(s.total_exec_time, ref), "accum total %.17g, want %.17g",
		  s.total_exec_time, (double) ref);
	CHECK(s.bucket_id == 5, "accum keeps bucket_id (%lld)", (long long) s.bucket_id);

	/* exactly representable: an exact sum */
	pssc_slot_relabel(&s, 6);
	pssc_slot_accum(&s, 0.25);
	pssc_slot_accum(&s, 1.5);
	pssc_slot_accum(&s, 40.0);
	CHECK(s.calls == 3 && same_double(s.total_exec_time, 41.75),
		  "accum 0.25+1.5+40: %lld/%g", (long long) s.calls, s.total_exec_time);

	/* calls beyond 32 bits */
	s.calls = INT64_C(0xFFFFFFFF);
	pssc_slot_accum(&s, 0.0);
	CHECK(s.calls == INT64_C(0x100000000), "calls is 64-bit (%lld)", (long long) s.calls);
}

static PsscSlot
mk(int64 bucket_id, int64 calls, double total)
{
	PsscSlot	s;

	s.bucket_id = bucket_id;
	s.calls = calls;
	s.total_exec_time = total;
	return s;
}

static PsscSlot
merge_all(const PsscSlot *in, const int *order, int n)
{
	PsscSlot	acc;
	int			i;

	fill_garbage(&acc);
	pssc_slot_init(&acc);
	for (i = 0; i < n; i++)
		pssc_slot_merge(&acc, &in[order ? order[i] : i]);
	return acc;
}

static void
next_perm(int *a, int n, bool *done)
{
	int			i = n - 2,
				j = n - 1,
				t;

	while (i >= 0 && a[i] >= a[i + 1])
		i--;
	if (i < 0)
	{
		*done = true;
		return;
	}
	while (a[j] <= a[i])
		j--;
	t = a[i];
	a[i] = a[j];
	a[j] = t;
	for (i++, j = n - 1; i < j; i++, j--)
	{
		t = a[i];
		a[i] = a[j];
		a[j] = t;
	}
}

static void
test_merge(void)
{
	/* the oldest (3) is neither first nor last; an empty slot is mixed in */
	PsscSlot	in[5];
	PsscSlot	m;
	int			order[5] = {0, 1, 2, 3, 4};
	bool		done = false;
	int			nperm = 0;

	in[0] = mk(7, 10, 1.5);
	in[1] = mk(5, 3, 0.25);
	in[2] = mk(PSSC_BUCKET_NONE, 0, 0.0);
	in[3] = mk(3, 1, 100.0);
	in[4] = mk(6, 2, 8.0);

	m = merge_all(in, NULL, 5);
	CHECK(m.bucket_id == 3, "merge keeps the oldest bucket_id: %lld", (long long) m.bucket_id);
	CHECK(m.calls == 16, "merge sums calls: %lld", (long long) m.calls);
	CHECK(same_double(m.total_exec_time, 109.75), "merge sums total: %.17g", m.total_exec_time);

	/* every input order gives the same oldest bucket and exact sums */
	while (!done)
	{
		m = merge_all(in, order, 5);
		CHECK(m.bucket_id == 3 && m.calls == 16 && same_double(m.total_exec_time, 109.75),
			  "merge order %d%d%d%d%d: %lld/%lld/%.17g",
			  order[0], order[1], order[2], order[3], order[4],
			  (long long) m.bucket_id, (long long) m.calls, m.total_exec_time);
		nperm++;
		next_perm(order, 5, &done);
	}
	CHECK(nperm == 120, "all permutations tried (%d)", nperm);

	/* empty first: the first real slot's id is taken, not PSSC_BUCKET_NONE */
	{
		PsscSlot	two[2];

		two[0] = mk(PSSC_BUCKET_NONE, 0, 0.0);
		two[1] = mk(9, 4, 2.0);
		m = merge_all(two, NULL, 2);
		CHECK(m.bucket_id == 9 && m.calls == 4 && same_double(m.total_exec_time, 2.0),
			  "merge skips empty: %lld/%lld/%g",
			  (long long) m.bucket_id, (long long) m.calls, m.total_exec_time);
	}

	/* nothing contributes: stays empty */
	{
		PsscSlot	none[2];

		none[0] = mk(PSSC_BUCKET_NONE, 0, 0.0);
		none[1] = mk(PSSC_BUCKET_NONE, 0, 0.0);
		m = merge_all(none, NULL, 2);
		CHECK(m.bucket_id == PSSC_BUCKET_NONE && m.calls == 0 &&
			  same_double(m.total_exec_time, 0.0), "merge of empties is empty");
		m = merge_all(none, NULL, 0);
		CHECK(m.bucket_id == PSSC_BUCKET_NONE && m.calls == 0, "merge of nothing is empty");
	}

	/* signed: -2 is older than 1 (an unsigned compare would pick 1) */
	{
		PsscSlot	neg[2];

		neg[0] = mk(1, 1, 1.0);
		neg[1] = mk(-2, 1, 1.0);
		m = merge_all(neg, NULL, 2);
		CHECK(m.bucket_id == -2, "merge compares bucket ids signed: %lld", (long long) m.bucket_id);
	}

	/* src unchanged */
	{
		PsscSlot	src = mk(4, 2, 0.5);
		PsscSlot	dst = mk(8, 1, 0.25);

		pssc_slot_merge(&dst, &src);
		CHECK(src.bucket_id == 4 && src.calls == 2 && same_double(src.total_exec_time, 0.5),
			  "merge leaves src unchanged");
		CHECK(dst.bucket_id == 4 && dst.calls == 3 && same_double(dst.total_exec_time, 0.75),
			  "merge into a non-empty dst: %lld/%lld/%g",
			  (long long) dst.bucket_id, (long long) dst.calls, dst.total_exec_time);
	}
}

static void
test_merge_matches_accum(void)
{
	/* merging per-bucket slots gives the same calls as one big slot */
	PsscSlot	ring[4],
				m;
	int			i;
	int64		want_calls = 0;
	volatile double want_total = 0.0;

	for (i = 0; i < 4; i++)
		pssc_slot_relabel(&ring[i], 20 + i);
	for (i = 0; i < 400; i++)
	{
		double		ms = (double) (i % 17) * 0.5;

		pssc_slot_accum(&ring[(i * 7) % 4], ms);
		want_calls++;
		want_total += ms;		/* all multiples of 0.5: every sum is exact */
	}
	m = merge_all(ring, NULL, 4);
	CHECK(m.calls == want_calls, "merged calls %lld want %lld",
		  (long long) m.calls, (long long) want_calls);
	CHECK(same_double(m.total_exec_time, want_total), "merged total %.17g want %.17g",
		  m.total_exec_time, (double) want_total);
	CHECK(m.bucket_id == 20, "merged bucket %lld", (long long) m.bucket_id);
}

static void
test_ms_from_seconds(void)
{
	/* pgss: queryDesc->totaltime->total * 1000.0 */
	const double secs[] = {0.0, 1.0, 0.001, 0.1, 1e-9, 123.456789, 0.0123456789012345,
	1e-6 * 3.0, 86400.5, 2.5e-5};
	size_t		i;
	int			distinct = 0;

	for (i = 0; i < sizeof(secs) / sizeof(secs[0]); i++)
	{
		volatile double s = secs[i];
		double		want = s * 1000.0;
		double		alt = s / 1e-3;
		double		got = pssc_ms_from_seconds(s);

		CHECK(same_double(got, want), "ms_from_seconds(%.17g) = %.17g, want %.17g",
			  (double) s, got, want);
		if (!same_double(alt, want))
			distinct++;
	}
	/* the samples tell s * 1000.0 apart from another plausible formula */
	CHECK(distinct > 0, "no sample distinguishes s * 1000.0 from s / 1e-3");
}

static void
test_usage(void)
{
	double		u;
	volatile double ref;
	int			i;

	u = pssc_usage_init();
	CHECK(same_double(u, PSSC_USAGE_INIT) && same_double(u, 1.0), "usage init %g", u);

	/* a new entry has usage 2.0 after its first call, as in pgss */
	pssc_usage_exec(&u);
	CHECK(same_double(u, 2.0), "usage after first call %g", u);
	for (i = 0; i < 8; i++)
		pssc_usage_exec(&u);
	CHECK(same_double(u, 10.0), "usage after 9 calls %g", u);

	/* decay: usage *= 0.99 per eviction pass */
	ref = u;
	for (i = 0; i < 50; i++)
	{
		pssc_usage_decay(&u);
		ref *= 0.99;
	}
	CHECK(same_double(u, ref), "usage after 50 decays %.17g want %.17g", u, (double) ref);
	CHECK(u < 10.0 && u > 6.0, "usage decays (%g)", u);

	/* an entry hit more often ranks above a decayed idle one */
	{
		double		busy = pssc_usage_init(),
					idle = pssc_usage_init();

		pssc_usage_exec(&busy);
		pssc_usage_exec(&busy);
		pssc_usage_exec(&idle);
		pssc_usage_decay(&busy);
		pssc_usage_decay(&idle);
		CHECK(busy > idle, "busy %g > idle %g", busy, idle);
		CHECK(same_double(idle, 2.0 * 0.99), "idle %.17g", idle);
	}

	/* the constants are pgss's */
	CHECK(same_double(PSSC_USAGE_EXEC, 1.0) && same_double(PSSC_USAGE_DECREASE_FACTOR, 0.99),
		  "pgss usage constants");
}

int
main(void)
{
	test_layout();
	test_init();
	test_relabel();
	test_roll();
	test_accum();
	test_merge();
	test_merge_matches_accum();
	test_ms_from_seconds();
	test_usage();

	if (failures)
	{
		fprintf(stderr, "test_counters: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("test_counters: all %d checks passed\n", checks);
	return 0;
}
