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
