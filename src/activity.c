/*
 * activity.c
 *		Per-backend current tags (pg_stat_statement_context_activity): the
 *		shared slots, their writers and the lock-free reader. See activity.h.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "activity.h"
#include "compat.h"
#include "guc.h"

#define PSSC_ACTIVITY_NAME "pg_stat_statement_context activity"

typedef struct ActivitySlot
{
	/* odd while the owner writes the slot; see activity.h */
	uint32		changecount;
	int			pid;			/* 0: empty */
	Oid			userid;
	Oid			dbid;
	int64		queryid;
	int			encoding;
	bool		active;
	uint32		tags_len;
	char		tags[FLEXIBLE_ARRAY_MEMBER];	/* max_tagset_bytes */
} ActivitySlot;

typedef struct ActivityHeader
{
	int			nslots;
	int			max_tagset_bytes;
	Size		slotsize;
} ActivityHeader;

#define HEADER_SIZE MAXALIGN(sizeof(ActivityHeader))

static pssc_shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static ActivityHeader *act_hdr = NULL;

/* This backend's slot and what it holds (only this backend writes it). */
typedef enum
{
	MY_EMPTY,
	MY_ACTIVE,
	MY_IDLE
} MyState;

static volatile ActivitySlot *my_slot = NULL;
static MyState my_state = MY_EMPTY;
static uint64 my_seq = 0;		/* last publication; 0 after a clear */
static bool exit_registered = false;

static Size
slot_size_for(int max_tagset_bytes)
{
	return MAXALIGN(add_size(offsetof(ActivitySlot, tags),
							 (Size) Max(max_tagset_bytes, 0)));
}

static Size
activity_shmem_size(int nslots, int max_tagset_bytes)
{
	return add_size(HEADER_SIZE,
					mul_size((Size) nslots, slot_size_for(max_tagset_bytes)));
}

static inline volatile ActivitySlot *
slot_at(int i)
{
	return (volatile ActivitySlot *)
		((char *) act_hdr + HEADER_SIZE + (Size) i * act_hdr->slotsize);
}

static void
activity_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(activity_shmem_size(pssc_max_backends_for_shmem(),
											   pssc_max_tagset_bytes));
}

static void
activity_shmem_startup(void)
{
	bool		found;
	int			nslots = pssc_max_backends_for_shmem();
	ActivityHeader *hdr;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	act_hdr = NULL;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	hdr = ShmemInitStruct(PSSC_ACTIVITY_NAME,
						  activity_shmem_size(nslots, pssc_max_tagset_bytes),
						  &found);
	if (!found)
	{
		memset(hdr, 0, activity_shmem_size(nslots, pssc_max_tagset_bytes));
		hdr->nslots = nslots;
		hdr->max_tagset_bytes = pssc_max_tagset_bytes;
		hdr->slotsize = slot_size_for(pssc_max_tagset_bytes);
		if (MaxBackends > nslots)
			ereport(LOG,
					(errmsg("pg_stat_statement_context: %d activity slots for %d backends; backends beyond them are not shown",
							nslots, MaxBackends)));
	}
	act_hdr = hdr;
	LWLockRelease(AddinShmemInitLock);
}

void
pssc_activity_init(void)
{
	PSSC_INSTALL_SHMEM_REQUEST_HOOK(prev_shmem_request_hook, activity_shmem_request);
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = activity_shmem_startup;
}

bool
pssc_activity_available(void)
{
	return act_hdr != NULL;
}

int
pssc_activity_nslots(void)
{
	return act_hdr ? act_hdr->nslots : 0;
}

Size
pssc_activity_tags_max(void)
{
	return act_hdr ? (Size) act_hdr->max_tagset_bytes : 0;
}

/*
 * Writes are bracketed like PgBackendStatus updates
 * (PGSTAT_BEGIN/END_WRITE_ACTIVITY): in a critical section, so an error
 * cannot leave the counter odd and readers spinning.
 */
static inline void
begin_write(volatile ActivitySlot *s)
{
	START_CRIT_SECTION();
	s->changecount++;
	pg_write_barrier();
}

static inline void
end_write(volatile ActivitySlot *s)
{
	pg_write_barrier();
	s->changecount++;
	Assert((s->changecount & 1) == 0);
	END_CRIT_SECTION();
}

static void
activity_exit(int code, Datum arg)
{
	pssc_activity_clear();
	my_slot = NULL;
}

/* This backend's slot, or NULL if it has none. */
static volatile ActivitySlot *
get_my_slot(void)
{
	int			i;

	if (my_slot != NULL)
		return my_slot;
	if (act_hdr == NULL)
		return NULL;
	i = PSSC_MY_BACKEND_SLOT();
	if (i < 0 || i >= act_hdr->nslots)
		return NULL;

	/*
	 * Registered on first use, so it runs (LIFO) before the backend gives
	 * up its slot index (ProcKill), and a later backend with the same index
	 * finds the slot empty.
	 */
	if (!exit_registered)
	{
		on_shmem_exit(activity_exit, (Datum) 0);
		exit_registered = true;
	}
	my_slot = slot_at(i);
	return my_slot;
}

uint64
pssc_activity_publish(const PsscFrame *frame)
{
	volatile ActivitySlot *s = get_my_slot();
	uint32		len;
	Oid			userid = GetUserId();

	if (s == NULL)
		return 0;
	len = frame->tags_len;
	if (len > (uint32) act_hdr->max_tagset_bytes || frame->tags == NULL)
		len = 0;				/* not expected: extraction honors the limit */

	begin_write(s);
	s->pid = MyProcPid;
	s->userid = userid;
	s->dbid = frame->dbid;
	s->queryid = frame->queryId;
	s->encoding = frame->encoding;
	s->active = true;
	s->tags_len = len;
	if (len > 0)
		memcpy((char *) s->tags, frame->tags, len);
	end_write(s);
	my_state = MY_ACTIVE;
	return ++my_seq;
}

bool
pssc_activity_resume(uint64 seq)
{
	volatile ActivitySlot *s;

	if (seq == 0 || seq != my_seq || my_state != MY_IDLE)
		return false;
	s = my_slot;
	begin_write(s);
	s->active = true;
	end_write(s);
	my_state = MY_ACTIVE;
	return true;
}

void
pssc_activity_set_idle(void)
{
	volatile ActivitySlot *s;

	if (my_state != MY_ACTIVE)
		return;
	s = my_slot;
	begin_write(s);
	s->active = false;
	end_write(s);
	my_state = MY_IDLE;
}

void
pssc_activity_clear(void)
{
	volatile ActivitySlot *s;

	if (my_state == MY_EMPTY)
		return;
	s = my_slot;
	begin_write(s);
	s->pid = 0;
	s->active = false;
	s->tags_len = 0;
	end_write(s);
	my_state = MY_EMPTY;
	my_seq++;
}

void
pssc_activity_read(int i, PsscActivityRow *row)
{
	volatile ActivitySlot *s;
	Size		max = (Size) act_hdr->max_tagset_bytes;

	Assert(i >= 0 && i < act_hdr->nslots);
	s = slot_at(i);
	for (;;)
	{
		uint32		before;
		uint32		after;

		before = s->changecount;
		pg_read_barrier();
		row->pid = s->pid;
		row->userid = s->userid;
		row->dbid = s->dbid;
		row->queryid = s->queryid;
		row->encoding = s->encoding;
		row->active = s->active;
		row->tags_len = Min(s->tags_len, max);
		if (row->pid != 0 && row->tags_len > 0)
			memcpy(row->tags, (const char *) s->tags, row->tags_len);
		pg_read_barrier();
		after = s->changecount;
		if (before == after && (before & 1) == 0)
			break;
		CHECK_FOR_INTERRUPTS();
	}
}
