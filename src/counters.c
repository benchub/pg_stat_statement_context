/*
 * counters.c
 *		Per-bucket counter slot functions. See counters.h and DESIGN.md §5.1,
 *		§5.2, §5.3, §7. Backend-independent (test/unit builds it with
 *		-DPSSC_STANDALONE and the c.h shim pssc_standalone.h).
 */
#ifdef PSSC_STANDALONE
#include "pssc_standalone.h"
#else
#include "postgres.h"
#endif

#include "counters.h"

void
pssc_slot_init(PsscSlot *slot)
{
	slot->bucket_id = PSSC_BUCKET_NONE;
	slot->calls = 0;
	slot->total_exec_time = 0.0;
}

void
pssc_slot_relabel(PsscSlot *slot, int64 bucket_id)
{
	slot->bucket_id = bucket_id;
	slot->calls = 0;
	slot->total_exec_time = 0.0;
}

bool
pssc_slot_roll(PsscSlot *slot, int64 bucket_id)
{
	Assert(slot->bucket_id <= bucket_id);
	if (slot->bucket_id >= bucket_id)
		return false;
	pssc_slot_relabel(slot, bucket_id);
	return true;
}

void
pssc_slot_accum(PsscSlot *slot, double elapsed_ms)
{
	slot->calls += 1;
	slot->total_exec_time += elapsed_ms;
}

void
pssc_slot_merge(PsscSlot *dst, const PsscSlot *src)
{
	if (src->bucket_id == PSSC_BUCKET_NONE)
		return;
	if (dst->bucket_id == PSSC_BUCKET_NONE || src->bucket_id < dst->bucket_id)
		dst->bucket_id = src->bucket_id;
	dst->calls += src->calls;
	dst->total_exec_time += src->total_exec_time;
}

double
pssc_usage_init(void)
{
	return PSSC_USAGE_INIT;
}

void
pssc_usage_exec(double *usage)
{
	*usage += PSSC_USAGE_EXEC;
}

void
pssc_usage_decay(double *usage)
{
	*usage *= PSSC_USAGE_DECREASE_FACTOR;
}

int64
pssc_evict_target(int64 max_entries)
{
	/* max_entries <= INT_MAX, so the product cannot overflow an int64 */
	int64		target = max_entries * PSSC_EVICT_PERCENT / 100;

	return target < 1 ? 1 : target;
}

int64
pssc_evict_live_count(int64 target, int64 dead_freed, int64 nlive)
{
	int64		n;

	if (dead_freed >= target)
		return 0;
	n = target - dead_freed;
	return n < nlive ? n : nlive;
}

int
pssc_evict_cmp(const void *a, const void *b)
{
	const PsscEvictCandidate *ca = (const PsscEvictCandidate *) a;
	const PsscEvictCandidate *cb = (const PsscEvictCandidate *) b;

	if (ca->last_bucket != cb->last_bucket)
		return ca->last_bucket < cb->last_bucket ? -1 : 1;
	if (ca->usage < cb->usage)
		return -1;
	if (ca->usage > cb->usage)
		return 1;
	if (ca->seq != cb->seq)
		return ca->seq < cb->seq ? -1 : 1;
	return 0;
}

void
pssc_evict_sort(PsscEvictCandidate *cands, size_t n)
{
	if (n > 1)
		qsort(cands, n, sizeof(PsscEvictCandidate), pssc_evict_cmp);
}

void
pssc_evict_select_init(PsscEvictSelect *sel, PsscEvictCandidate *buf, size_t cap)
{
	sel->buf = buf;
	sel->cap = cap;
	sel->size = 0;
	sel->next_seq = 0;
}

static inline bool
evict_less(const PsscEvictCandidate *a, const PsscEvictCandidate *b)
{
	return pssc_evict_cmp(a, b) < 0;
}

/* restores the max-heap below position i of heap[0 .. size) */
static void
evict_sift_down(PsscEvictCandidate *heap, size_t size, size_t i)
{
	PsscEvictCandidate x = heap[i];

	for (;;)
	{
		size_t		child = 2 * i + 1;

		if (child >= size)
			break;
		if (child + 1 < size && evict_less(&heap[child], &heap[child + 1]))
			child++;
		if (!evict_less(&x, &heap[child]))
			break;
		heap[i] = heap[child];
		i = child;
	}
	heap[i] = x;
}

/*
 * The slow path of pssc_evict_select_offer(): adds a candidate to a heap
 * that is not full, or replaces the top of a full one with a candidate
 * that sorts before it.
 */
void
pssc_evict_select_push(PsscEvictSelect *sel, int64 last_bucket, double usage,
					   void *entry, uint64 seq)
{
	PsscEvictCandidate x;
	PsscEvictCandidate *heap = sel->buf;

	x.last_bucket = last_bucket;
	x.usage = usage;
	x.entry = entry;
	x.seq = seq;
	if (sel->size < sel->cap)
	{
		size_t		i = sel->size++;

		while (i > 0)
		{
			size_t		parent = (i - 1) / 2;

			if (!evict_less(&heap[parent], &x))
				break;
			heap[i] = heap[parent];
			i = parent;
		}
		heap[i] = x;
	}
	else
	{
		Assert(sel->cap > 0 && evict_less(&x, &heap[0]));
		heap[0] = x;
		evict_sift_down(heap, sel->size, 0);
	}
}

size_t
pssc_evict_select_finish(PsscEvictSelect *sel, size_t k)
{
	PsscEvictCandidate *heap = sel->buf;
	size_t		size = sel->size;

	Assert(k <= sel->cap);
	/* heapsort: move the largest to the end, one at a time */
	while (size > 1)
	{
		PsscEvictCandidate top = heap[0];

		size--;
		heap[0] = heap[size];
		heap[size] = top;
		evict_sift_down(heap, size, 0);
	}
	return k < sel->size ? k : sel->size;
}

int64
pssc_bucket_floor_div(int64 a, int64 b)
{
	int64		q = a / b;		/* truncates toward zero; b > 0 */

	Assert(b > 0);
	if (a % b != 0 && a < 0)
		q--;
	return q;
}

int64
pssc_bucket_for_time(int64 now_us, int64 epoch_us, int64 interval_us)
{
	int64		diff;

	/* now - epoch, saturating instead of overflowing */
	if (epoch_us > 0 && now_us < INT64_MIN + epoch_us)
		diff = INT64_MIN;
	else if (epoch_us < 0 && now_us > INT64_MAX + epoch_us)
		diff = INT64_MAX;
	else
		diff = now_us - epoch_us;
	return pssc_bucket_floor_div(diff, interval_us);
}

int
pssc_bucket_slot_index(int64 bucket_id, int bucket_count)
{
	int64		i = bucket_id % bucket_count;

	Assert(bucket_count >= 1);
	return (int) (i < 0 ? i + bucket_count : i);
}

bool
pssc_bucket_is_live(int64 bucket_id, int64 current, int bucket_count)
{
	if (bucket_id == PSSC_BUCKET_NONE || bucket_id > current)
		return false;
	/* current - bucket_id >= 0 and exact in uint64 */
	return (uint64) current - (uint64) bucket_id < (uint64) bucket_count;
}

bool
pssc_bucket_entry_is_dead(int64 last_bucket, int64 current, int bucket_count)
{
	return !pssc_bucket_is_live(last_bucket, current, bucket_count);
}

int64
pssc_bucket_start(int64 bucket_id, int64 epoch_us, int64 interval_us)
{
	return epoch_us + bucket_id * interval_us;
}

const char *
pssc_ring_check(const PsscSlot *slots, int bucket_count, int64 last_bucket,
				int64 current_bucket, int *bad_slot)
{
	int64		newest = PSSC_BUCKET_NONE;

	*bad_slot = -1;
	if (last_bucket != PSSC_BUCKET_NONE && last_bucket > current_bucket)
		return "last_bucket is newer than current_bucket";
	for (int i = 0; i < bucket_count; i++)
	{
		const PsscSlot *s = &slots[i];

		*bad_slot = i;
		if (s->bucket_id == PSSC_BUCKET_NONE)
		{
			if (s->calls != 0 || s->total_exec_time != 0.0)
				return "empty slot has counts";
			continue;
		}
		if (s->bucket_id > current_bucket)
			return "slot holds a bucket newer than current_bucket";
		if (pssc_bucket_slot_index(s->bucket_id, bucket_count) != i)
			return "slot holds a bucket that does not map to its index";
		if (s->calls < 1)
			return "written slot has no calls";
		if (s->bucket_id > newest)
			newest = s->bucket_id;
	}
	*bad_slot = -1;
	if (newest != last_bucket)
		return "last_bucket is not the newest bucket in the ring";
	return NULL;
}
