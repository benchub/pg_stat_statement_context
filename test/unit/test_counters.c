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

/* §5.2: floor division, also below zero (clock before the epoch) */
static void
test_floor_div(void)
{
	struct
	{
		int64		a,
					b,
					want;
	}			c[] = {
		{0, 10, 0}, {9, 10, 0}, {10, 10, 1}, {19, 10, 1},
		{-1, 10, -1}, {-9, 10, -1}, {-10, 10, -1}, {-11, 10, -2}, {-20, 10, -2},
		{-21, 10, -3}, {7, 1, 7}, {-7, 1, -7},
		{INT64_MAX, 1, INT64_MAX}, {INT64_MIN, 1, INT64_MIN},
		{INT64_MIN, 2, INT64_MIN / 2}, {INT64_MIN + 1, 2, INT64_MIN / 2},
		{INT64_MAX, INT64_MAX, 1}, {INT64_MIN, INT64_MAX, -2}, {-1, INT64_MAX, -1},
	};

	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		CHECK(pssc_bucket_floor_div(c[i].a, c[i].b) == c[i].want,
			  "floor_div(%lld, %lld) = %lld, want %lld", (long long) c[i].a,
			  (long long) c[i].b, (long long) pssc_bucket_floor_div(c[i].a, c[i].b),
			  (long long) c[i].want);
}

static void
test_bucket_for_time(void)
{
	const int64 s = 1000000;	/* 1 s in us */
	const int64 epoch = 1000 * s;

	CHECK(pssc_bucket_for_time(epoch, epoch, s) == 0, "at the epoch: bucket 0");
	CHECK(pssc_bucket_for_time(epoch + s - 1, epoch, s) == 0, "just before 1 s: 0");
	CHECK(pssc_bucket_for_time(epoch + s, epoch, s) == 1, "at 1 s: 1");
	CHECK(pssc_bucket_for_time(epoch + 300 * s, epoch, 300 * s) == 1, "300 s interval");
	CHECK(pssc_bucket_for_time(epoch + 599 * s, epoch, 300 * s) == 1, "599 s / 300 s");
	CHECK(pssc_bucket_for_time(epoch - 1, epoch, s) == -1, "1 us before the epoch: -1");
	CHECK(pssc_bucket_for_time(epoch - s, epoch, s) == -1, "1 s before the epoch: -1");
	CHECK(pssc_bucket_for_time(epoch - s - 1, epoch, s) == -2, "1 s + 1 us before: -2");
	CHECK(pssc_bucket_for_time(0, epoch, 300 * s) == -4, "1000 s before, 300 s: -4");
	/* the difference saturates instead of overflowing */
	CHECK(pssc_bucket_for_time(INT64_MAX, -epoch, s) == INT64_MAX / s,
		  "saturates high: %lld", (long long) pssc_bucket_for_time(INT64_MAX, -epoch, s));
	CHECK(pssc_bucket_for_time(INT64_MIN, epoch, s) == pssc_bucket_floor_div(INT64_MIN, s),
		  "saturates low: %lld", (long long) pssc_bucket_for_time(INT64_MIN, epoch, s));
}

static void
test_slot_index(void)
{
	CHECK(pssc_bucket_slot_index(0, 12) == 0, "0 mod 12");
	CHECK(pssc_bucket_slot_index(13, 12) == 1, "13 mod 12");
	CHECK(pssc_bucket_slot_index(-1, 12) == 11, "-1 mod 12 = 11");
	CHECK(pssc_bucket_slot_index(-12, 12) == 0, "-12 mod 12 = 0");
	CHECK(pssc_bucket_slot_index(-13, 12) == 11, "-13 mod 12 = 11");
	CHECK(pssc_bucket_slot_index(12345, 1) == 0, "one-slot ring");
	CHECK(pssc_bucket_slot_index(INT64_MIN + 1, 10000) ==
		  (int) (((INT64_MIN + 1) % 10000) + 10000), "INT64_MIN + 1");
	CHECK(pssc_bucket_slot_index(INT64_MAX, 10000) == (int) (INT64_MAX % 10000), "INT64_MAX");
	for (int64 b = -50; b < 50; b++)
		CHECK(pssc_bucket_slot_index(b, 7) == pssc_bucket_slot_index(b + 7, 7) &&
			  pssc_bucket_slot_index(b, 7) >= 0 && pssc_bucket_slot_index(b, 7) < 7,
			  "slot index of %lld is periodic and in range", (long long) b);
}

static void
test_live(void)
{
	/* window [current - count + 1, current] */
	CHECK(pssc_bucket_is_live(100, 100, 12), "current is live");
	CHECK(pssc_bucket_is_live(89, 100, 12), "current - 11 is live (12 buckets)");
	CHECK(!pssc_bucket_is_live(88, 100, 12), "current - 12 has expired");
	CHECK(!pssc_bucket_is_live(101, 100, 12), "a newer id is not live");
	CHECK(!pssc_bucket_is_live(PSSC_BUCKET_NONE, 100, 12), "an empty slot is not live");
	CHECK(pssc_bucket_is_live(5, 5, 1) && !pssc_bucket_is_live(4, 5, 1), "one bucket");
	CHECK(pssc_bucket_is_live(-3, -1, 3) && !pssc_bucket_is_live(-4, -1, 3),
		  "negative ids");
	/* no overflow forming the window near the int64 limits */
	CHECK(pssc_bucket_is_live(INT64_MIN + 1, INT64_MIN + 1, 10000), "near INT64_MIN");
	CHECK(!pssc_bucket_is_live(INT64_MIN + 1, INT64_MAX, 10000), "far apart");
	CHECK(pssc_bucket_is_live(INT64_MAX, INT64_MAX, 10000) &&
		  pssc_bucket_is_live(INT64_MAX - 9999, INT64_MAX, 10000) &&
		  !pssc_bucket_is_live(INT64_MAX - 10000, INT64_MAX, 10000), "near INT64_MAX");
	CHECK(!pssc_bucket_is_live(PSSC_BUCKET_NONE, INT64_MIN + 1, 10000),
		  "empty is never live, even next to INT64_MIN");

	CHECK(!pssc_bucket_entry_is_dead(100, 100, 12), "written now: alive");
	CHECK(!pssc_bucket_entry_is_dead(89, 100, 12), "oldest live: alive");
	CHECK(pssc_bucket_entry_is_dead(88, 100, 12), "every slot expired: dead");
	CHECK(pssc_bucket_entry_is_dead(PSSC_BUCKET_NONE, 100, 12), "never written: dead");
}

static void
test_bucket_start(void)
{
	const int64 s = 1000000;

	CHECK(pssc_bucket_start(0, 7 * s, 300 * s) == 7 * s, "bucket 0 starts at the epoch");
	CHECK(pssc_bucket_start(3, 7 * s, 300 * s) == 907 * s, "bucket 3");
	CHECK(pssc_bucket_start(-1, 7 * s, 300 * s) == -293 * s, "bucket -1");
	for (int64 b = -20; b < 20; b++)
		CHECK(pssc_bucket_for_time(pssc_bucket_start(b, 7 * s, 300 * s), 7 * s, 300 * s) == b &&
			  pssc_bucket_for_time(pssc_bucket_start(b, 7 * s, 300 * s) - 1, 7 * s, 300 * s) == b - 1,
			  "bucket_start(%lld) round-trips", (long long) b);
}

static void
test_ring_check(void)
{
	PsscSlot	r[4];
	int			bad;
	const char *e;

	for (int i = 0; i < 4; i++)
		pssc_slot_init(&r[i]);
	CHECK(pssc_ring_check(r, 4, PSSC_BUCKET_NONE, 0, &bad) == NULL && bad == -1,
		  "an empty ring is fine");
	r[1] = mk(9, 1, 0.5);		/* 9 mod 4 = 1 */
	r[2] = mk(-2, 3, 0.0);		/* -2 mod 4 = 2 */
	CHECK(pssc_ring_check(r, 4, 9, 9, &bad) == NULL, "a valid ring");
	CHECK(pssc_ring_check(r, 4, 9, 12, &bad) == NULL, "current ahead of last_bucket");
	e = pssc_ring_check(r, 4, 9, 8, &bad);
	CHECK(e != NULL && bad == -1, "last_bucket newer than current: %s", e ? e : "-");
	e = pssc_ring_check(r, 4, 8, 9, &bad);
	CHECK(e != NULL && bad == -1, "last_bucket not the newest: %s", e ? e : "-");
	e = pssc_ring_check(r, 4, PSSC_BUCKET_NONE, 9, &bad);
	CHECK(e != NULL, "last_bucket NONE with written slots: %s", e ? e : "-");

	r[3] = mk(5, 1, 0.0);		/* 5 mod 4 = 1, not 3 */
	e = pssc_ring_check(r, 4, 9, 9, &bad);
	CHECK(e != NULL && bad == 3, "incongruent slot: %s (%d)", e ? e : "-", bad);
	r[3] = mk(11, 1, 0.0);		/* 11 mod 4 = 3, newer than current 9 */
	e = pssc_ring_check(r, 4, 9, 9, &bad);
	CHECK(e != NULL && bad == 3, "slot newer than current: %s (%d)", e ? e : "-", bad);
	r[3] = mk(7, 0, 0.0);
	e = pssc_ring_check(r, 4, 9, 9, &bad);
	CHECK(e != NULL && bad == 3, "written slot without calls: %s (%d)", e ? e : "-", bad);
	r[3] = mk(PSSC_BUCKET_NONE, 1, 0.0);
	e = pssc_ring_check(r, 4, 9, 9, &bad);
	CHECK(e != NULL && bad == 3, "empty slot with calls: %s (%d)", e ? e : "-", bad);
	r[3] = mk(PSSC_BUCKET_NONE, 0, 0.25);
	e = pssc_ring_check(r, 4, 9, 9, &bad);
	CHECK(e != NULL && bad == 3, "empty slot with time: %s (%d)", e ? e : "-", bad);
	r[3] = mk(7, 2, 1.0);
	CHECK(pssc_ring_check(r, 4, 9, 9, &bad) == NULL, "valid again");
}

int
main(void)
{
	test_ring_check();
	test_floor_div();
	test_bucket_for_time();
	test_slot_index();
	test_live();
	test_bucket_start();
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
