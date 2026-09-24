/*-------------------------------------------------------------------------
 *
 * idxdefer.c
 *		Build the indexes of a temporary table once, after a bulk
 *		INSERT ... SELECT, instead of maintaining them row by row.
 *
 * The problem
 * -----------
 * 1C creates an index on each temporary table right after creating the table
 * and before filling it, and then fills it with one large INSERT ... SELECT.
 * The rows arrive in whatever order the source query produces them, which for
 * an index key is effectively random.  As long as the index fits in
 * temp_buffers that costs a B-tree descent per row and nothing more.  Once it
 * does not, every insertion lands on a leaf page that was evicted a moment
 * ago: the dirty victim is written out and the leaf is read back in.
 *
 * Measured on the 1C load-test stand, one such statement inserts 13 million
 * rows into a table whose heap and index together come to about 1.5 GB, and
 * writes 29 GB of local buffers doing it, reading nearly as much back.  The
 * Insert node alone is 81-84 seconds of it.  A synthetic reproduction on the
 * same machine takes 84.7 s as 1C does it, 5.3 + 14.3 s if the index is built
 * after the rows are in, and 55.5 s with temp_buffers raised to 4 GB.  Across
 * a whole package run, the Insert nodes of indexed temporary-table inserts are
 * about half of all the time the database spends.
 *
 * What this does
 * --------------
 * For an INSERT that qualifies (see idxdefer_consider()), the executor is
 * simply not given the table's indexes, so the rows go into the heap only, and
 * once they are all in, the indexes are rebuilt with reindex_index() - a
 * sort-based build, which is what CREATE INDEX on a filled table would have
 * done.
 *
 * Nothing in the catalogs is touched to achieve the first half.  The obvious
 * alternative, clearing pg_index.indisready for the duration, is not used:
 * that is a transactional catalog update and a relcache invalidation twice per
 * statement, on a workload that creates temporary tables by the thousand.
 * (indisvalid would not have helped in any case: it only keeps the planner
 * from reading an index, and ExecInsertIndexTuples() tests
 * ii_ReadyForInserts, which comes from indisready.)
 *
 * Instead the hook relies on how ExecInsert() opens indexes in PG18: lazily,
 * on the first row, and only if ri_IndexRelationDescs is still NULL.  Putting
 * an empty array there, with ri_NumIndices = 0, makes the executor treat the
 * table as having no indexes, and there is nothing to undo afterwards -
 * ExecCloseIndices() closes the zero indexes it was given.
 *
 * When the rebuild happens
 * ------------------------
 * As early as it possibly can, so that nothing else runs while the heap has
 * rows the index does not know about:
 *
 *   - Normally, inside the ModifyTable node itself.  Its ExecProcNode is
 *     wrapped, through ExecSetExecProcNode(), and the rebuild runs the moment
 *     the node reports that it is done, before control returns to ExecutePlan.
 *     No hook of any library can run in between.  The rebuild is then part of
 *     the Insert node's time in EXPLAIN ANALYZE, and of queryDesc->totaltime,
 *     which is what auto_explain and pg_stat_statements report - just as the
 *     row-by-row maintenance it replaces was.
 *
 *   - For a plan that runs in parallel mode that is impossible: a catalog
 *     update is refused in parallel mode, and ExecutePlan() only leaves it
 *     after the node is done.  On Tantor SE this is the common case, not a
 *     corner: with enable_parallel_insert its planner puts a Gather under the
 *     Insert node, which the stock planner never does.  The earliest point
 *     after that is the ExecutorRun hook, right after standard_ExecutorRun()
 *     returns - and only if our hook is the innermost one, so that no other
 *     library's post-processing runs first.  Whether it is, is fixed when the
 *     library is loaded, so it is recorded then, and a parallel plan is simply
 *     not deferred when it is not.  The rebuild is charged to totaltime by
 *     hand on this path.
 *
 *   - ExecutorFinish and ExecutorEnd rebuild whatever is somehow still
 *     pending, before any processing of their own.  Neither is expected to
 *     find anything; they are there so that a path nobody thought of costs a
 *     late rebuild, never a missing one.  A rebuild that happens anywhere but
 *     where it is expected is reported as a WARNING.
 *
 * How much memory the rebuild gets
 * --------------------------------
 * When the rebuild starts, the rows are in and there is no need to guess any
 * more: the statement counted them, and the heap's size bounds how large they
 * are.  For a B-tree, whose build is one tuplesort of index tuples, that is
 * enough to estimate what the sort needs to stay in memory, and the rebuild
 * gets that much maintenance_work_mem - or idxdefer.maintenance_work_mem, the
 * module's own limit, if the estimate is larger.  See idxdefer_sort_mem().
 *
 * Why this is safe
 * ----------------
 * The target is empty when the statement starts, belongs to this backend, and
 * has no triggers, no unique or exclusion indexes and no ON CONFLICT clause:
 * nothing needs the index to decide what happens to a row.  So the state in
 * between - rows in the heap that the index does not know about - can only be
 * observed by reading the table, and can only outlive the statement on error.
 *
 * 1.  The statement itself reading the target.  Its own rows are invisible to
 *     its own snapshot, and the table was empty, so any scan in it sees
 *     nothing whether it goes through the index or not.  An index scan would
 *     in any case have opened the index before we look, and an index that is
 *     already open disqualifies the statement.
 *
 * 2.  Code called by the statement reading the target.  A VOLATILE function
 *     in the source query runs its queries with a newer command id and does
 *     see the rows inserted so far - through a plan that may use the index.
 *     So while a statement is deferred, get_relation_info_hook takes the
 *     indexes of its target away from the planner, and the target's relcache
 *     entry is invalidated and the invalidation processed at once, which makes
 *     every cached plan that depends on it be replanned on its next use.  Such
 *     a function therefore reads the table with a sequential scan and sees
 *     exactly the rows it would have seen without this module.  A STABLE or
 *     IMMUTABLE function runs with the statement's own snapshot and cannot see
 *     those rows at all.
 *
 * 3.  Other libraries' hooks.  None runs between the last row and the rebuild
 *     on the normal path, and on the parallel path the only code that does is
 *     the tail of standard_ExecutorRun() itself; see above.
 *
 * 4.  An error.  If the INSERT fails, nothing is rebuilt; the rows it did
 *     insert are dead, and an index that lacks entries for dead tuples is a
 *     perfectly valid index.  If the rebuild fails, the transaction aborts,
 *     which takes the new relfilenode with it and leaves the old index -
 *     consistent with the heap, since the rows that are missing from it are
 *     exactly the ones the abort killed.  Either way there is nothing to
 *     repair, and the transaction and subtransaction callbacks below only have
 *     to forget what they were told.  A deferral that is somehow still pending
 *     at commit is refused there, because the table outlives the transaction.
 *
 * What it costs
 * -------------
 * A rebuild is a full index build, so it only pays off when the statement
 * writes a lot of rows into the table.  idxdefer.min_rows sets the bar
 * against the planner's estimate; on the package this was written for, the
 * inserts of a million rows or more are 98% of the index maintenance time, and
 * those below a hundred thousand are 0.2%.  A misestimate costs one needless
 * rebuild, never a wrong answer.  The rebuild also moves the index to a new
 * relfilenode and updates pg_class, and the cached plans that the
 * invalidation above threw away are planned again.  Everything else is listed
 * in idxdefer-caveats.md.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/plancat.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "storage/itemid.h"
#include "storage/procnumber.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "enforce_workers.h"

typedef enum IdxdeferMode
{
	IDXDEFER_OFF = 0,			/* do nothing at all */
	IDXDEFER_LOG,				/* evaluate and report, change nothing */
	IDXDEFER_ON					/* evaluate, report, and act */
} IdxdeferMode;

static const struct config_enum_entry idxdefer_mode_options[] = {
	{"off", IDXDEFER_OFF, false},
	{"log", IDXDEFER_LOG, false},
	{"on", IDXDEFER_ON, false},
	{NULL, 0, false}
};

/* Same set nlguard and seqguard offer, for consistency within the module. */
static const struct config_enum_entry idxdefer_loglevel_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

static int	idxdefer_mode = IDXDEFER_ON;
static int	idxdefer_log_level = DEBUG1;
static int	idxdefer_min_rows = 1000000;
static int	idxdefer_maintenance_work_mem = 1024 * 1024;	/* 1GB, in kB */

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static get_relation_info_hook_type prev_get_relation_info_hook = NULL;

/*
 * Was there no ExecutorRun_hook before ours when the library was loaded?
 * Then ours calls standard_ExecutorRun() directly, and the code right after
 * that call is the first thing to run once the plan is done - which is what
 * the parallel path depends on.  Libraries loaded later wrap around us and
 * cannot change that, so this is decided once, in idxdefer_init().
 */
static bool idxdefer_run_hook_innermost = false;

/*
 * Where a deferral is in its life.  The indexes stay hidden from the planner
 * until it is DONE, which includes the time the rebuild itself takes.
 */
typedef enum IdxdeferState
{
	IDXDEFER_PENDING,			/* rows going in, nothing rebuilt yet */
	IDXDEFER_REBUILDING,		/* inside idxdefer_rebuild() */
	IDXDEFER_DONE				/* rebuilt, or found nothing to rebuild */
} IdxdeferState;

/*
 * One INSERT whose index maintenance has been taken away, from its
 * ExecutorStart to its ExecutorEnd.
 */
typedef struct IdxdeferTarget
{
	QueryDesc  *queryDesc;		/* the statement, to find it again */
	ModifyTableState *mtstate;	/* its ModifyTable node, which we wrap */
	ExecProcNodeMtd orig_exec;	/* what the node ran before we wrapped it */
	Oid			relid;			/* its target table */
	List	   *indexes;		/* OIDs of the indexes to rebuild */
	double		plan_rows;		/* the planner's estimate, as a fallback */
	SubTransactionId subid;		/* subtransaction it was registered in */
	IdxdeferState state;
} IdxdeferTarget;

/* The places a rebuild can happen, for the report and the sanity checks. */
typedef enum IdxdeferPoint
{
	IDXDEFER_AT_NODE,			/* inside the ModifyTable node */
	IDXDEFER_AT_RUN,			/* in the ExecutorRun hook */
	IDXDEFER_AT_FINISH,			/* in the ExecutorFinish hook */
	IDXDEFER_AT_END				/* in the ExecutorEnd hook */
} IdxdeferPoint;

static const char *const idxdefer_point_names[] = {
	[IDXDEFER_AT_NODE] = "at the end of the insert",
	[IDXDEFER_AT_RUN] = "after the parallel plan finished",
	[IDXDEFER_AT_FINISH] = "in ExecutorFinish",
	[IDXDEFER_AT_END] = "in ExecutorEnd",
};

/*
 * The deferrals in progress, innermost last.  Everything here, the list
 * included, lives in TopTransactionContext: an entry is still consulted in
 * ExecutorEnd, after standard processing has begun to tear the executor down,
 * and it must not survive the transaction, because by then the QueryDesc it
 * points to is long gone.  The transaction callback resets the pointer before
 * that context is deleted.
 *
 * Almost always empty, and never longer than the nesting depth of deferred
 * INSERTs, so a list is the right structure.
 *
 * One list for the backend assumes one transaction at a time.  A fork with
 * autonomous transactions, which would commit and abort inside ours, would
 * need the entries tagged with the transaction they belong to; Tantor SE 1C
 * has none.
 */
static List *idxdefer_targets = NIL;

/* Name of the index being rebuilt, for the error context callback. */
typedef struct IdxdeferRebuildContext
{
	const char *indexname;
} IdxdeferRebuildContext;

/*
 * idxdefer_find_target
 *		The deferral of this statement, if it has one.
 *
 * Both the QueryDesc and its top plan node must match.  Entries never outlive
 * their statement on any known path, but if one ever did, and its addresses
 * were reused, matching on the QueryDesc alone would hand back a stranger's
 * deferral.
 */
static IdxdeferTarget *
idxdefer_find_target(QueryDesc *queryDesc)
{
	ListCell   *lc;

	foreach(lc, idxdefer_targets)
	{
		IdxdeferTarget *target = (IdxdeferTarget *) lfirst(lc);

		if (target->queryDesc == queryDesc &&
			(PlanState *) target->mtstate == queryDesc->planstate)
			return target;
	}

	return NULL;
}

/*
 * idxdefer_find_target_by_node
 *		The deferral whose ModifyTable node this is.
 *
 * Searched from the end: the node being executed belongs to the innermost
 * statement, which registered last.
 */
static IdxdeferTarget *
idxdefer_find_target_by_node(PlanState *pstate)
{
	for (int i = list_length(idxdefer_targets) - 1; i >= 0; i--)
	{
		IdxdeferTarget *target = (IdxdeferTarget *) list_nth(idxdefer_targets, i);

		if ((PlanState *) target->mtstate == pstate)
			return target;
	}

	return NULL;
}

/*
 * idxdefer_is_target
 *		Must this relation's indexes be hidden from the planner?
 *
 * Yes until its rebuild is over, the rebuild itself included: an index
 * expression that queries the table would otherwise be planned through the
 * index being rebuilt.
 */
static bool
idxdefer_is_target(Oid relid)
{
	ListCell   *lc;

	foreach(lc, idxdefer_targets)
	{
		IdxdeferTarget *target = (IdxdeferTarget *) lfirst(lc);

		if (target->relid == relid && target->state != IDXDEFER_DONE)
			return true;
	}

	return false;
}

/*
 * idxdefer_forget_target
 *		Unregister a deferral and free it.
 */
static void
idxdefer_forget_target(IdxdeferTarget *target)
{
	idxdefer_targets = list_delete_ptr(idxdefer_targets, target);
	list_free(target->indexes);
	pfree(target);
}

/*
 * idxdefer_get_relation_info
 *		get_relation_info_hook entry point.
 *
 * Hide the indexes of a table whose INSERT is deferred from any query planned
 * while it runs; see point 2 at the top of the file.  Removing entries from
 * indexlist is what this hook is documented to allow, and all it can do is
 * make a plan slower.  It runs after the previous hook, so an index another
 * library adds here - a hypothetical one, say - is hidden as well.
 */
static void
idxdefer_get_relation_info(PlannerInfo *root, Oid relationObjectId,
						   bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

	if (idxdefer_targets != NIL && idxdefer_is_target(relationObjectId))
		rel->indexlist = NIL;
}

/*
 * idxdefer_collect_indexes
 *		List the table's indexes, or NIL if any of them rules the table out.
 *
 * A unique or exclusion index is checked while each row goes in, so it cannot
 * be left behind; an index that is not ready, not valid or not live is in the
 * middle of something we should not interfere with.  One that is already open
 * - by a cursor, or by a scan in this very statement - would make the rebuild
 * fail in CheckTableNotInUse(), so it rules the table out too.
 *
 * The indexes are locked in RowExclusiveLock, which is what ExecOpenIndices()
 * would have taken for the insert, and the locks are kept.
 */
static List *
idxdefer_collect_indexes(Relation rel)
{
	List	   *indexoids = RelationGetIndexList(rel);
	ListCell   *lc;

	foreach(lc, indexoids)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	irel;
		Form_pg_index index;
		bool		usable;

		irel = index_open(indexoid, RowExclusiveLock);
		index = irel->rd_index;

		usable = (index->indisvalid && index->indisready &&
				  index->indislive &&
				  !index->indisunique && !index->indisexclusion &&
				  irel->rd_refcnt == 1);

		index_close(irel, NoLock);

		if (!usable)
		{
			list_free(indexoids);
			return NIL;
		}
	}

	return indexoids;
}

/*
 * idxdefer_report
 *		Say what is being, or would be, done to the indexes of one table.
 */
static void
idxdefer_report(Relation rel, int nindexes, double rows, bool acting)
{
	if (acting)
		ereport(idxdefer_log_level,
				(errmsg("idxdefer: deferring index maintenance on \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_plural("%d index will be rebuilt after the insert; the planner expects %.0f rows.",
								  "%d indexes will be rebuilt after the insert; the planner expects %.0f rows.",
								  nindexes, nindexes, rows)));
	else
		ereport(idxdefer_log_level,
				(errmsg("idxdefer: would defer index maintenance on \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_plural("%d index would be rebuilt after the insert; the planner expects %.0f rows.",
								  "%d indexes would be rebuilt after the insert; the planner expects %.0f rows.",
								  nindexes, nindexes, rows)));
}

/*
 * idxdefer_rebuild_callback
 *		Error context: say which index was being rebuilt, and why.
 *
 * The name is looked up before the rebuild rather than here: an error context
 * callback runs while the error is being reported, which is no time to go to
 * the catalogs.
 */
static void
idxdefer_rebuild_callback(void *arg)
{
	IdxdeferRebuildContext *ctx = (IdxdeferRebuildContext *) arg;

	if (ctx->indexname != NULL)
		errcontext("rebuilding index \"%s\" after an INSERT that deferred its maintenance",
				   ctx->indexname);
}

/*
 * idxdefer_sort_mem
 *		How much maintenance_work_mem to give the rebuild of one index, in kB.
 *
 * maintenance_work_mem is a ceiling, not an allocation: a tuplesort takes
 * memory as the tuples arrive, so a large setting costs nothing by itself.
 * What a ceiling close to the real need buys is the memtuples array.  It grows
 * by doubling while the sort has used less than half its budget, so under a
 * budget far above the data it can end up almost twice as large as the tuples
 * need - at 24 bytes a slot, a few hundred megabytes on the inserts this module
 * is for.  Under a budget sized to the data, its last growth step is
 * proportional instead (see grow_memtuples()).  With many sessions rebuilding
 * at once that is the difference that matters, and the report can say what
 * each rebuild was given.
 *
 * The estimate is for a B-tree, whose build is one tuplesort holding a
 * SortTuple and an IndexTuple for every heap row; a non-unique index has only
 * the one spool (see _bt_spools_heapscan()).  Other access methods use
 * maintenance_work_mem in their own ways and get the limit as it is.
 *
 * Two things are known exactly by now: the number of rows, which the statement
 * counted, and the size of the heap.  The width of an index tuple is exact
 * when every column of the index is a plain column of fixed length.  If not,
 * it is bounded by the heap's footprint per row: an index tuple's header is
 * smaller than a heap tuple's, and a plain column's value takes no more room
 * in the index than in the heap.  (That fails for a value the heap keeps in
 * TOAST, and is only a heuristic for an expression.)  Erring high costs
 * little, since the ceiling is not allocated; erring low sends the sort to
 * disk.  So the estimate errs high and gets a margin on top.
 *
 * Returns the setting to use, and the estimate itself in *estimate_kb, or -1
 * there if the access method has none.
 */
static int
idxdefer_sort_mem(IdxdeferTarget *target, Relation heap, Oid indexoid,
				  BlockNumber heap_nblocks, int *estimate_kb)
{
	Relation	irel;
	TupleDesc	itupdesc;
	TupleDesc	heapdesc = RelationGetDescr(heap);
	Form_pg_index index;
	double		ntuples;
	double		heap_width;
	double		itup_width;
	double		bytes;
	Size		data = 0;
	Size		header = sizeof(IndexTupleData);
	bool		exact = true;

	/* Still locked since idxdefer_collect_indexes(). */
	irel = index_open(indexoid, NoLock);

	if (irel->rd_rel->relam != BTREE_AM_OID)
	{
		index_close(irel, NoLock);
		*estimate_kb = -1;
		return idxdefer_maintenance_work_mem;
	}

	/*
	 * es_processed counts the rows of this ModifyTable node, the only one in
	 * the statement.  It stays zero for a statement that does not set the
	 * command tag, such as a rule action; the planner's estimate is all there
	 * is then.
	 */
	ntuples = (double) target->mtstate->ps.state->es_processed;
	if (!target->mtstate->canSetTag || ntuples < 1.0)
		ntuples = target->plan_rows;
	ntuples = Max(ntuples, 1.0);

	/* The heap tuple, its line pointer and its share of the free space. */
	heap_width = (double) heap_nblocks * BLCKSZ / ntuples;

	itupdesc = RelationGetDescr(irel);
	index = irel->rd_index;

	for (int i = 0; i < itupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(itupdesc, i);
		AttrNumber	heapattno = index->indkey.values[i];

		/*
		 * A column that can be null means a null bitmap in some tuples, which
		 * moves the data to the next MAXALIGN boundary.
		 */
		if (heapattno == 0 ||
			!TupleDescAttr(heapdesc, heapattno - 1)->attnotnull)
			header = sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData);

		if (heapattno == 0 || att->attlen <= 0)
			exact = false;
		else
			data = att_align_nominal(data, att->attalign) + att->attlen;
	}

	index_close(irel, NoLock);

	itup_width = (double) MAXALIGN(MAXALIGN(header) + data);
	if (!exact)
		itup_width = Max(itup_width, heap_width);

	bytes = ntuples * (sizeof(SortTuple) + itup_width);

	/* A quarter on top, and a megabyte for the sort's own bookkeeping. */
	bytes = bytes * 1.25 + 1024.0 * 1024.0;

	*estimate_kb = (int) Min(bytes / 1024.0, (double) MAX_KILOBYTES);

	return Max(Min(*estimate_kb, idxdefer_maintenance_work_mem), 64);
}

/*
 * idxdefer_rebuild
 *		Rebuild the indexes a deferred INSERT did not maintain.
 *
 * The executor never opened these indexes, so reindex_index() finds them
 * unused wherever this is called from, and the statement's locks are held
 * throughout.  reindex_index() does everything a REINDEX INDEX would - it runs
 * index expressions as the table owner under a restricted search_path, and
 * keeps its own GUC changes local - and it is transactional, which is the
 * whole of the error-path argument at the top of the file.  The command
 * counter is advanced after each index, as reindex_relation() does.
 *
 * Why transactional, when the table was empty and a cheaper way exists?
 * RelationTruncateIndexes() - truncate the index file in place, then
 * index_build() into it - would avoid the new relfilenode, the pg_class
 * update, the file creation and unlink, and the WAL-logged exclusive lock.
 * It is what ON COMMIT DELETE ROWS does.  But it is not safe here: ON COMMIT
 * DELETE ROWS builds empty indexes with a dummy IndexInfo and runs no user
 * code, whereas this build sorts the whole table and evaluates index
 * expressions, and anything from a failing expression to a query cancel or
 * statement_timeout can interrupt it.  In place, that leaves a B-tree with no
 * metapage, and the next statement to touch the table fails reading it, for
 * the rest of the session.  A transactional rebuild leaves the old index.
 *
 * 'account' is the instrumentation to charge the work to when the caller is
 * outside the executor's own accounting (the hooks), or NULL when the work
 * happens inside a plan node and is counted already.  'point' says where the
 * caller is; anywhere but the two expected places is reported.
 */
static void
idxdefer_rebuild(IdxdeferTarget *target, Instrumentation *account,
				 IdxdeferPoint point)
{
	Relation	rel;
	BlockNumber nblocks;
	ErrorContextCallback errcallback;
	IdxdeferRebuildContext ctx;
	bool		pushed_snapshot = false;
	bool		report = message_level_is_interesting(idxdefer_log_level);
	ListCell   *lc;

	Assert(target->state == IDXDEFER_PENDING);
	Assert(!IsInParallelMode());

	/*
	 * No second caller may start the rebuild again, and the indexes stay
	 * hidden until it is over.  If it fails, the transaction aborts, and the
	 * entry goes with it.
	 */
	target->state = IDXDEFER_REBUILDING;

	rel = table_open(target->relid, NoLock);

	/*
	 * An INSERT that brought no rows left the table empty and the indexes
	 * correct as they are.  The heap is the thing to ask, not es_processed,
	 * which a rule action that does not set the command tag never counts in.
	 */
	nblocks = RelationGetNumberOfBlocks(rel);
	if (nblocks == 0)
	{
		table_close(rel, NoLock);
		target->state = IDXDEFER_DONE;
		return;
	}

	/*
	 * The two expected places are the node itself and, for a plan that ran in
	 * parallel mode, the ExecutorRun hook.  Anything else means the node's
	 * end went unnoticed - another library replaced its ExecProcNode, say -
	 * and the rebuild is late: correct, but after code that the design meant
	 * to keep out of the window.  That should never be silent.
	 */
	if (point == IDXDEFER_AT_FINISH || point == IDXDEFER_AT_END ||
		(point == IDXDEFER_AT_RUN &&
		 !target->queryDesc->plannedstmt->parallelModeNeeded))
		ereport(WARNING,
				(errmsg("idxdefer: rebuilding the indexes of \"%s\" %s, later than expected",
						RelationGetRelationName(rel),
						idxdefer_point_names[point]),
				 errdetail("The insert's plan node finished without idxdefer noticing; another library may have replaced its execution function.")));

	if (account)
		InstrStartNode(account);

	/*
	 * Index expressions may need a snapshot to run.  The callers of the
	 * executor normally have one pushed at this point; make sure of it.
	 */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_snapshot = true;
	}

	ctx.indexname = NULL;
	errcallback.callback = idxdefer_rebuild_callback;
	errcallback.arg = &ctx;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	foreach(lc, target->indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		ReindexParams params = {0};
		char	   *indexname;
		char		buf[32];
		int			mem_kb;
		int			estimate_kb;
		int			save_nestlevel;
		instr_time	start;
		instr_time	elapsed;

		/*
		 * Code called by the statement could have dropped an index in the
		 * meantime; the table was open, but its indexes were not.
		 */
		indexname = get_rel_name(indexoid);
		if (indexname == NULL)
			continue;
		ctx.indexname = indexname;

		mem_kb = idxdefer_sort_mem(target, rel, indexoid, nblocks,
								   &estimate_kb);

		if (report)
			INSTR_TIME_SET_CURRENT(start);

		save_nestlevel = NewGUCNestLevel();
		snprintf(buf, sizeof(buf), "%dkB", mem_kb);
		(void) set_config_option("maintenance_work_mem", buf,
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

		params.options = REINDEXOPT_MISSING_OK;
		reindex_index(NULL, indexoid, false, RELPERSISTENCE_TEMP, &params);

		AtEOXact_GUC(true, save_nestlevel);

		CommandCounterIncrement();

		/* What follows is a report, not something that went wrong. */
		ctx.indexname = NULL;

		if (report)
		{
			INSTR_TIME_SET_CURRENT(elapsed);
			INSTR_TIME_SUBTRACT(elapsed, start);

			if (estimate_kb >= 0)
				ereport(idxdefer_log_level,
						(errmsg("idxdefer: rebuilt index \"%s\"", indexname),
						 errdetail("Rebuilt %s in %.3f ms with maintenance_work_mem = %d kB (estimated sort size %d kB, limit %d kB).",
								   idxdefer_point_names[point],
								   INSTR_TIME_GET_MILLISEC(elapsed),
								   mem_kb, estimate_kb,
								   idxdefer_maintenance_work_mem)));
			else
				ereport(idxdefer_log_level,
						(errmsg("idxdefer: rebuilt index \"%s\"", indexname),
						 errdetail("Rebuilt %s in %.3f ms with maintenance_work_mem = %d kB (the limit; no estimate for this access method).",
								   idxdefer_point_names[point],
								   INSTR_TIME_GET_MILLISEC(elapsed),
								   mem_kb)));
		}
	}

	error_context_stack = errcallback.previous;

	if (pushed_snapshot)
		PopActiveSnapshot();

	table_close(rel, NoLock);

	if (account)
		InstrStopNode(account, 0);

	target->state = IDXDEFER_DONE;
}

/*
 * idxdefer_ExecModifyTable
 *		The ExecProcNode of a deferred INSERT's ModifyTable node.
 *
 * Without RETURNING, ExecModifyTable() consumes its whole input in one call
 * and returns NULL, so this runs once per statement and the rebuild happens
 * the moment the last row is in.  In parallel mode it cannot, and the
 * ExecutorRun hook does it instead.
 */
static TupleTableSlot *
idxdefer_ExecModifyTable(PlanState *pstate)
{
	IdxdeferTarget *target = idxdefer_find_target_by_node(pstate);
	TupleTableSlot *slot;

	/*
	 * The node was wrapped while it was registered, and nothing unregisters
	 * it while the executor can still call it: ExecutorEnd comes after the
	 * last call, and an aborted subtransaction takes the executor with it.
	 */
	if (target == NULL)
		elog(ERROR, "idxdefer: ModifyTable node of a deferred insert is not registered");

	slot = target->orig_exec(pstate);

	if (TupIsNull(slot) && target->state == IDXDEFER_PENDING &&
		!IsInParallelMode())
		idxdefer_rebuild(target, NULL, IDXDEFER_AT_NODE);

	return slot;
}

/*
 * idxdefer_defer
 *		Take the indexes away from this INSERT and arrange for their rebuild.
 *
 * The order matters.  The entry is registered before the executor state is
 * touched, so that the indexes are never taken away without a record of what
 * has to be rebuilt; and the invalidation is processed before the first row
 * goes in, so that there is no window in which a cached plan could still
 * reach the index.
 */
static void
idxdefer_defer(QueryDesc *queryDesc, ModifyTableState *mtstate,
			   ResultRelInfo *rri, List *indexes, double rows)
{
	Relation	rel = rri->ri_RelationDesc;
	MemoryContext oldcontext;
	IdxdeferTarget *target;

	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	target = (IdxdeferTarget *) palloc(sizeof(IdxdeferTarget));
	target->queryDesc = queryDesc;
	target->mtstate = mtstate;
	target->orig_exec = mtstate->ps.ExecProcNodeReal;
	target->relid = RelationGetRelid(rel);
	target->indexes = list_copy(indexes);
	target->plan_rows = rows;
	target->subid = GetCurrentSubTransactionId();
	target->state = IDXDEFER_PENDING;

	idxdefer_targets = lappend(idxdefer_targets, target);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Make every cached plan that depends on the table be replanned before
	 * its next use, which will then go through idxdefer_get_relation_info().
	 * The invalidation is normally processed when the command ends, which is
	 * too late: the calls we are guarding against happen during it.
	 *
	 * A command counter increment processes it now, and it is the sanctioned
	 * way to do that.  Processing the queued messages without the increment,
	 * with CommandEndInvalidationMessages(), would be wrong in general: the
	 * queue holds whatever the current command has done to the catalogs, and
	 * rebuilding the relcache entry of a relation that command created would
	 * read pg_class with a snapshot that cannot see the new row yet.
	 *
	 * The increment does not disturb the statement.  Its rows are written
	 * with es_output_cid, which standard_ExecutorStart() has already fixed,
	 * and its scans use a registered copy of the snapshot, whose command id
	 * does not move - so its own rows stay invisible to it exactly as before.
	 * It is also what a VOLATILE function in the source query does before
	 * each of its own queries.  It costs one command id per deferred
	 * statement.
	 */
	CacheInvalidateRelcache(rel);
	CommandCounterIncrement();

	/*
	 * ExecInsert() opens the indexes on the first row if, and only if, this
	 * array is still NULL.  An empty one, with a count of zero, stands for
	 * "no indexes" from here on; nothing reads its elements, and
	 * ExecCloseIndices() closes nothing.
	 */
	rri->ri_NumIndices = 0;
	rri->ri_IndexRelationDescs = (RelationPtr)
		MemoryContextAllocZero(queryDesc->estate->es_query_cxt,
							   sizeof(Relation));
	rri->ri_IndexRelationInfo = (IndexInfo **)
		MemoryContextAllocZero(queryDesc->estate->es_query_cxt,
							   sizeof(IndexInfo *));

	/* Rebuild the moment the node is done; see the top of the file. */
	ExecSetExecProcNode(&mtstate->ps, idxdefer_ExecModifyTable);

	idxdefer_report(rel, list_length(indexes), rows, true);
}

/*
 * idxdefer_consider
 *		Decide whether this INSERT qualifies, and act on the decision.
 *
 * Called after standard_ExecutorStart(), so the result relation is set up but
 * has not had its indexes opened yet.  The tests are ordered by cost; nearly
 * every statement is rejected before any catalog is consulted.
 *
 * What is deliberately not required:
 *
 *   - that the source query does not read the target.  It could only read an
 *     empty table through its own snapshot, and a scan of an index of the
 *     target would already have opened that index, which is caught below.
 *
 *   - anything about the source query's functions; see point 2 at the top of
 *     the file for why a function reading the table is handled rather than
 *     refused.
 */
static void
idxdefer_consider(QueryDesc *queryDesc)
{
	PlannedStmt *pstmt = queryDesc->plannedstmt;
	ModifyTableState *mtstate;
	ModifyTable *mtplan;
	ResultRelInfo *rri;
	Relation	rel;
	Plan	   *subplan;
	List	   *indexes;

	/*
	 * RETURNING and data-modifying CTEs put the INSERT in a portal that other
	 * work can interleave with, or next to other ModifyTable nodes; neither
	 * is the statement this is for.
	 */
	if (pstmt->commandType != CMD_INSERT ||
		pstmt->hasReturning ||
		pstmt->hasModifyingCTE)
		return;

	if (queryDesc->planstate == NULL ||
		!IsA(queryDesc->planstate, ModifyTableState))
		return;

	mtstate = (ModifyTableState *) queryDesc->planstate;
	mtplan = (ModifyTable *) mtstate->ps.plan;

	/* ON CONFLICT needs the indexes for every row, whatever its form. */
	if (mtstate->operation != CMD_INSERT ||
		mtstate->mt_nrels != 1 ||
		mtplan->onConflictAction != ONCONFLICT_NONE)
		return;

	rri = mtstate->resultRelInfo;
	rel = rri->ri_RelationDesc;

	/*
	 * A plain table that only this backend can see.  rd_islocaltemp alone is
	 * not enough: in a parallel worker the leader's temporary tables have it
	 * set too (see the long comment in seqguard_nextval()), which is why the
	 * caller also refuses to run in a worker at all.
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION ||
		!RelationUsesLocalBuffers(rel) ||
		!rel->rd_islocaltemp ||
		rel->rd_backend != MyProcNumber)
		return;

	/*
	 * Nothing to defer, or already opened (which only ON CONFLICT does before
	 * the first row, but do not rely on that), or a trigger that might read
	 * the table through an index, or a foreign table's own write path.
	 */
	if (!rel->rd_rel->relhasindex ||
		rri->ri_IndexRelationDescs != NULL ||
		rri->ri_TrigDesc != NULL ||
		rri->ri_FdwRoutine != NULL)
		return;

	subplan = outerPlan(&mtplan->plan);
	if (subplan == NULL || subplan->plan_rows < (double) idxdefer_min_rows)
		return;

	/*
	 * Rebuilding the index costs about as much as building it on the whole
	 * table, which only pays when the statement brings in all of it.
	 */
	if (RelationGetNumberOfBlocks(rel) != 0)
		return;

	indexes = idxdefer_collect_indexes(rel);
	if (indexes == NIL)
		return;

	/*
	 * A plan that runs in parallel mode can only be rebuilt for once
	 * ExecutePlan() has left it, which is after standard_ExecutorRun() - and
	 * the rebuild is only guaranteed to be the first thing to run after that
	 * if our ExecutorRun hook is the innermost one.  If it is not, another
	 * library's hook would see the table with its index incomplete, so leave
	 * the statement alone and say why.
	 */
	if (pstmt->parallelModeNeeded && !idxdefer_run_hook_innermost)
	{
		ereport(idxdefer_log_level,
				(errmsg("idxdefer: not deferring index maintenance on \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("The plan runs in parallel mode, and another library's ExecutorRun hook would run between the insert and the rebuild."),
				 errhint("List enforce_workers before every other library in shared_preload_libraries.")));
		list_free(indexes);
		return;
	}

	if (idxdefer_mode == IDXDEFER_ON)
		idxdefer_defer(queryDesc, mtstate, rri, indexes, subplan->plan_rows);
	else
		idxdefer_report(rel, list_length(indexes), subplan->plan_rows, false);

	list_free(indexes);
}

/*
 * idxdefer_ExecutorStart
 *		ExecutorStart_hook entry point.
 */
static void
idxdefer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (idxdefer_mode != IDXDEFER_OFF &&
		queryDesc->operation == CMD_INSERT &&
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0 &&
		!IsParallelWorker())
		idxdefer_consider(queryDesc);
}

/*
 * idxdefer_ExecutorRun
 *		ExecutorRun_hook entry point.
 *
 * The rebuild point for a plan that ran in parallel mode; see the top of the
 * file.  standard_ExecutorRun() has stopped queryDesc->totaltime by now, so
 * the work is charged to it here.
 */
static void
idxdefer_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					 uint64 count)
{
	IdxdeferTarget *target;

	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);

	if (idxdefer_targets == NIL)
		return;

	target = idxdefer_find_target(queryDesc);
	if (target != NULL && target->state == IDXDEFER_PENDING &&
		target->mtstate->mt_done)
		idxdefer_rebuild(target, queryDesc->totaltime, IDXDEFER_AT_RUN);
}

/*
 * idxdefer_ExecutorFinish
 *		ExecutorFinish_hook entry point: a safety net.
 *
 * The rebuild runs before the standard processing - before AFTER triggers,
 * which a deferred table does not have, and before the post-processing of any
 * library whose hook we call.
 */
static void
idxdefer_ExecutorFinish(QueryDesc *queryDesc)
{
	if (idxdefer_targets != NIL)
	{
		IdxdeferTarget *target = idxdefer_find_target(queryDesc);

		if (target != NULL && target->state == IDXDEFER_PENDING)
			idxdefer_rebuild(target, queryDesc->totaltime,
							 IDXDEFER_AT_FINISH);
	}

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

/*
 * idxdefer_ExecutorEnd
 *		ExecutorEnd_hook entry point: the last safety net, and the cleanup.
 *
 * The deferral is unregistered before the standard processing, so that a
 * failure there never leaves it behind.
 */
static void
idxdefer_ExecutorEnd(QueryDesc *queryDesc)
{
	if (idxdefer_targets != NIL)
	{
		IdxdeferTarget *target = idxdefer_find_target(queryDesc);

		if (target != NULL)
		{
			if (target->state == IDXDEFER_PENDING)
				idxdefer_rebuild(target, queryDesc->totaltime,
								 IDXDEFER_AT_END);
			idxdefer_forget_target(target);
		}
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * idxdefer_xact_callback
 *		Forget the deferrals of a transaction that is ending.
 *
 * Every deferral ends in a rebuild or in an error, so at commit there should
 * be none pending.  If there were, the table would go into the next
 * transaction with an index that is missing rows, so that is refused rather
 * than assumed.
 */
static void
idxdefer_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			{
				ListCell   *lc;

				foreach(lc, idxdefer_targets)
				{
					IdxdeferTarget *target = (IdxdeferTarget *) lfirst(lc);

					if (target->state != IDXDEFER_DONE)
						elog(ERROR, "idxdefer: index rebuild for relation %u is still pending at commit",
							 target->relid);
				}
			}
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/* The list lived in TopTransactionContext, which is going away. */
			idxdefer_targets = NIL;
			break;

		default:
			/* Tantor SE has pre-abort events as well; nothing to do there. */
			break;
	}
}

/*
 * idxdefer_subxact_callback
 *		Forget the deferrals of a subtransaction that is being rolled back.
 *
 * An INSERT that failed inside a savepoint, or inside a PL/pgSQL exception
 * block, never reaches ExecutorEnd.  Subtransaction ids only grow, so the
 * deferrals registered in the aborted subtransaction and in anything nested
 * below it are exactly those with an id no smaller than its own.
 */
static void
idxdefer_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;

	foreach(lc, idxdefer_targets)
	{
		IdxdeferTarget *target = (IdxdeferTarget *) lfirst(lc);

		if (target->subid >= mySubid)
			idxdefer_targets = foreach_delete_current(idxdefer_targets, lc);
	}
}

/*
 * idxdefer_init
 *		Define the GUCs and chain the hooks.  Called from _PG_init() only.
 */
void
idxdefer_init(void)
{
	DefineCustomEnumVariable("idxdefer.mode",
							 "Controls whether bulk inserts into temporary tables maintain their indexes row by row.",
							 "off leaves inserts alone; log reports the inserts that would be affected; on inserts the rows into the heap only and rebuilds the indexes once they are all in.",
							 &idxdefer_mode,
							 IDXDEFER_ON,
							 idxdefer_mode_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("idxdefer.log_level",
							 "Message level at which deferred inserts and rebuilt indexes are reported.",
							 NULL,
							 &idxdefer_log_level,
							 DEBUG1,
							 idxdefer_loglevel_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("idxdefer.min_rows",
							"Planner row estimate below which an insert keeps its index maintenance.",
							NULL,
							&idxdefer_min_rows,
							1000000,
							0, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("idxdefer.maintenance_work_mem",
							"Upper limit on the memory for rebuilding one index of a deferred insert.",
							"A B-tree rebuild gets the sort size estimated from the inserted rows, if that is smaller; other index types get this value.",
							&idxdefer_maintenance_work_mem,
							1024 * 1024,
							64, MAX_KILOBYTES,
							PGC_USERSET,
							GUC_UNIT_KB,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("idxdefer");

	RegisterXactCallback(idxdefer_xact_callback, NULL);
	RegisterSubXactCallback(idxdefer_subxact_callback, NULL);

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = idxdefer_ExecutorStart;

	/*
	 * Must be decided before we install ourselves; see its declaration.  A
	 * wrong load order costs every parallel-mode insert its deferral, which
	 * on a build that runs INSERT ... SELECT in parallel is most of them, so
	 * say so once rather than per statement at a debug level.
	 */
	idxdefer_run_hook_innermost = (ExecutorRun_hook == NULL);
	if (!idxdefer_run_hook_innermost)
		ereport(process_shared_preload_libraries_in_progress ? LOG : WARNING,
				(errmsg("idxdefer: inserts whose plans run in parallel mode will keep maintaining their indexes"),
				 errdetail("Another library installed an ExecutorRun hook before enforce_workers was loaded."),
				 errhint("List enforce_workers before every other library in shared_preload_libraries.")));
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = idxdefer_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = idxdefer_ExecutorFinish;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = idxdefer_ExecutorEnd;

	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = idxdefer_get_relation_info;
}
