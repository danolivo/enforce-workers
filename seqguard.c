/*-------------------------------------------------------------------------
 *
 * seqguard.c
 *		Stop one nextval() on a temporary sequence from making a whole query
 *		parallel-unsafe.
 *
 * The problem
 * -----------
 * A table whose column has a sequence default - a serial column, an identity
 * column, or an explicit DEFAULT nextval(...) - gets that expression expanded
 * into the source query of any INSERT that omits the column.  The expansion
 * puts a nextval() call into a target list, and nextval() is marked
 * PROPARALLEL_UNSAFE.
 *
 * standard_planner() scans the whole Query for the worst parallel hazard it
 * contains and stops there:
 *
 *		glob->maxParallelHazard = max_parallel_hazard(parse);
 *		glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
 *
 * The scan does not care where in the tree the hazard was found, so a single
 * nextval() in the topmost target list costs the entire query its parallelism,
 * including the scans, sorts and joins far below it that never touch the
 * sequence.  This is the shape that 1C produces for essentially every
 * intermediate result: a temporary table with a serial column, filled by
 * INSERT ... SELECT, where the SELECT is the expensive part.
 *
 * Why nextval() is unsafe, and why "restricted" is not enough
 * ----------------------------------------------------------
 * Two separate reasons, and only the first is about workers:
 *
 * 1.  Sequence values are handed out from a backend-local cache (SeqTableData
 *     in sequence.c).  Two backends drawing from their own caches would
 *     interleave, and no part of that state is shared, so a worker cannot
 *     participate.
 *
 * 2.  Parallel mode is a blanket read-only regime, and nextval_internal()
 *     enforces it with PreventCommandIfParallelMode(), which tests
 *     IsInParallelMode() - true in the *leader* as well, for as long as a
 *     Gather is open.  Marking the function PARALLEL RESTRICTED would place
 *     the call above every Gather, which is correct as far as the planner
 *     goes, and the call would still fail at run time.
 *
 * So the marking cannot simply be relaxed.  What can be done is to notice that
 * for one class of sequence the reasons above have no force at all.
 *
 * Why a temporary sequence is different
 * -------------------------------------
 * The blanket rule exists to keep the state the workers share with the leader
 * from moving underneath them.  Advancing a temporary sequence moves nothing
 * they can see:
 *
 *   - It writes no WAL.  RelationNeedsWAL() is false for a temporary relation,
 *     so nextval_internal() skips both the XLogInsert() and the
 *     GetTopTransactionId() call that guards it.  That second point matters on
 *     its own: AssignTransactionId() refuses to run in parallel mode, so a
 *     permanent sequence would fail there even if the first check were lifted.
 *
 *   - It assigns no transaction id and bumps no command id.  Sequences are
 *     non-transactional; no tuple version becomes visible to anyone.
 *
 *   - Its page lives in local buffers, which belong to one backend.
 *
 *   - Nothing running below a Gather can read it.  The value is reachable only
 *     through nextval(), currval() and lastval(), none of which is
 *     parallel-safe, or by scanning the sequence relation - and a sequence
 *     never gets a partial path, so that scan is never parallel either.
 *
 * The write is therefore invisible by construction rather than by convention,
 * and performing it in the leader while a Gather is open changes nothing that
 * any worker could observe.
 *
 * What this file provides
 * -----------------------
 * seqguard_nextval(), declared PARALLEL RESTRICTED in the extension script.
 * That marking is the whole point of its existence: it tells the planner the
 * call must stay in the leader, which is where it was always going to run, and
 * costs the query nothing else.
 *
 * The function delegates to nextval_internal() whenever it is not in parallel
 * mode, which is almost always - same cache, same values, same currval() and
 * lastval().  Only with a Gather open does it take its own path, and there it
 * re-checks that the sequence really is a local temporary one before touching
 * anything, handing everything else back to the core so that the error raised
 * is the one the core would have raised.
 *
 * Getting the planner to use it in place of nextval() is a separate question
 * and is answered separately.  Nothing here depends on that: the function is
 * an ordinary SQL function and a call written by hand behaves the same way.
 *
 * What this costs
 * ---------------
 * The private path does not use the backend-local sequence cache, because that
 * cache is static in sequence.c and cannot be reached from here.  It takes one
 * value per call, as CACHE 1 would.  Two consequences, both visible only to a
 * session that runs a parallel query over a temporary sequence:
 *
 *   - currval() and lastval() do not see the values it produced.  They report
 *     whatever the core last cached for that sequence, or raise the usual "not
 *     yet defined in this session" error.
 *
 *   - If the core had already cached a block for the same sequence, the values
 *     handed out afterwards are not in ascending order.  They are still
 *     unique: the core owns its cached block exclusively, and this path takes
 *     values from beyond the end of it.  Sequences do not promise ordering
 *     across cached blocks in any case, and a sequence with CACHE 1 - which is
 *     the default, and what a serial column gets - has no block to diverge
 *     from.
 *
 * Neither matters for a surrogate row number in a temporary table, which is
 * what this exists for.  Both are reasons to use the function narrowly.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sequence.h"
#include "access/xact.h"
#include "catalog/pg_sequence.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_FUNCTION_INFO_V1(seqguard_nextval);

/*
 * The magic number every sequence page carries in its special space.  Defined
 * privately in sequence.c as SEQ_MAGIC, so it is repeated here; the point of
 * checking it is to refuse a page that is not a sequence page at all, which is
 * worth a duplicated constant.
 */
#define SEQGUARD_SEQ_MAGIC	0x1717

/*
 * seqguard_nextval_local
 *		Advance a temporary sequence without consulting parallel mode.
 *
 * A stripped-down nextval_internal(): no backend-local cache, and none of the
 * WAL machinery, because RelationNeedsWAL() is false for every relation that
 * reaches this point and the whole logging branch would be dead code.  The
 * arithmetic, the bounds handling and the buffer protocol are kept in the same
 * shape as the original so that the two can be compared side by side.
 */
static int64
seqguard_nextval_local(Relation seqrel)
{
	Buffer		buf;
	Page		page;
	ItemId		lp;
	HeapTupleData seqtuple;
	Form_pg_sequence_data seq;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	uint32	   *magic;
	int64		incby,
				maxv,
				minv,
				next;
	bool		cycle;

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(RelationGetRelid(seqrel)));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u",
			 RelationGetRelid(seqrel));
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	incby = pgsform->seqincrement;
	maxv = pgsform->seqmax;
	minv = pgsform->seqmin;
	cycle = pgsform->seqcycle;
	ReleaseSysCache(pgstuple);

	/* seqcache is read but not honoured; see the header comment. */

	buf = ReadBuffer(seqrel, 0);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	magic = (uint32 *) PageGetSpecialPointer(page);

	if (*magic != SEQGUARD_SEQ_MAGIC)
		elog(ERROR, "bad magic number in sequence \"%s\": %08X",
			 RelationGetRelationName(seqrel), *magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsNormal(lp));

	seqtuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	seqtuple.t_len = ItemIdGetLength(lp);
	seq = (Form_pg_sequence_data) GETSTRUCT(&seqtuple);

	/*
	 * read_seq_tuple() also clears a stale xmax left by a pre-8.2 SELECT FOR
	 * UPDATE on a sequence.  No such tuple can reach this function: the
	 * sequence was created by this backend, in this session.
	 */

	next = seq->last_value;

	if (seq->is_called)
	{
		if (incby > 0)
		{
			if ((maxv >= 0 && next > maxv - incby) ||
				(maxv < 0 && next + incby > maxv))
			{
				if (!cycle)
				{
					UnlockReleaseBuffer(buf);
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached maximum value of sequence \"%s\" (%" PRId64 ")",
									RelationGetRelationName(seqrel), maxv)));
				}
				next = minv;
			}
			else
				next += incby;
		}
		else
		{
			if ((minv < 0 && next < minv - incby) ||
				(minv >= 0 && next + incby < minv))
			{
				if (!cycle)
				{
					UnlockReleaseBuffer(buf);
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached minimum value of sequence \"%s\" (%" PRId64 ")",
									RelationGetRelationName(seqrel), minv)));
				}
				next = maxv;
			}
			else
				next += incby;
		}
	}

	/*
	 * log_cnt counts how many values ahead of the page the last WAL record
	 * covered.  Nothing here writes WAL, so the field is bookkeeping for a
	 * path that cannot run; zero is the conservative value, because it makes a
	 * later nextval_internal() on the same sequence take its "must log" branch
	 * - which for a temporary sequence writes nothing and merely fetches a few
	 * extra values.
	 */
	START_CRIT_SECTION();

	MarkBufferDirty(buf);

	seq->last_value = next;
	seq->is_called = true;
	seq->log_cnt = 0;

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);

	return next;
}

/*
 * seqguard_nextval
 *		SQL-callable replacement for nextval(regclass).
 *
 * Declared PARALLEL RESTRICTED, which is what the planner needs to see, and
 * which also guarantees that this never runs in a worker.
 */
Datum
seqguard_nextval(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	seqrel;
	int64		result;

	/*
	 * The ordinary case, and the one that keeps the module honest: outside
	 * parallel mode this is nextval(), cache and all, so currval(), lastval()
	 * and the sequence's value sequence are exactly what they would have been.
	 */
	if (!IsInParallelMode())
		PG_RETURN_INT64(nextval_internal(relid, true));

	Assert(!IsParallelWorker());

	seqrel = sequence_open(relid, RowExclusiveLock);

	if (pg_class_aclcheck(relid, GetUserId(),
						  ACL_USAGE | ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	/*
	 * The planner only substitutes for a sequence it has established is ours,
	 * but a plan outlives the catalog lookup that produced it, and this is the
	 * check the entire safety argument rests on.  Verify it again here, and
	 * hand anything else back to the core - which raises exactly the error it
	 * would have raised had we never intervened.
	 *
	 * rd_islocaltemp is precisely "temporary, and belonging to this session",
	 * which is the property wanted; spelling it out as a persistence test plus
	 * a backend comparison would only be a second copy of relcache's own rule.
	 */
	if (!seqrel->rd_islocaltemp)
	{
		sequence_close(seqrel, NoLock);
		PG_RETURN_INT64(nextval_internal(relid, true));
	}

	result = seqguard_nextval_local(seqrel);

	sequence_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}
