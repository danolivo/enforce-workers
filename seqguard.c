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
 * Mechanism
 * ---------
 * The module cannot change the marking of nextval() - proparallel is a catalog
 * property of the function, and the planner reads it long before any hook of
 * ours could intervene.  Note in particular that prosupport is too late:
 * SupportRequestSimplify runs from eval_const_expressions(), inside
 * subquery_planner(), whereas the hazard scan happens above it in
 * standard_planner(), which says so in as many words - "parallelModeOK can't
 * change after this point".
 *
 * The one place early enough is planner_hook, which runs before
 * standard_planner().  So:
 *
 *   1.  Decide from the Query header alone whether this is an INSERT into a
 *       temporary table of this backend.  Almost every statement is rejected
 *       on commandType, and the rest costs one syscache lookup.  Nothing else
 *       is walked; see seqguard_target_is_local_temp() for what that gives up
 *       and why.
 *
 *   2.  Walk that Query looking for nextval() calls whose argument is a
 *       constant naming a sequence that is temporary and belongs to this
 *       backend.  All of that is known at plan time: the argument is a Const
 *       of type regclass, because that is how the rewriter expands a column
 *       default.
 *
 *   3.  If any are found, point those calls at seqguard_nextval() instead,
 *       which is declared PARALLEL RESTRICTED.  The rewrite is made in place;
 *       seqguard_planner() says why that is safe.
 *
 *   4.  Hand the Query to standard_planner(), which now sees a restricted
 *       hazard rather than an unsafe one, allows parallel paths, and keeps the
 *       call above every Gather.
 *
 * At run time seqguard_nextval() delegates to nextval_internal() whenever it
 * is not in parallel mode, which is almost always - same cache, same values,
 * same currval() and lastval().  Only with a Gather open does it take its own
 * path, and there it re-checks that the sequence really is one of ours before
 * touching anything.
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
 * what this exists for.  Both are reasons to keep the substitution as narrow
 * as it is, and seqguard.mode = off turns it off outright.
 *
 * Note that community PostgreSQL will not put a Gather under an INSERT at all:
 * standard_planner() requires parse->commandType == CMD_SELECT, and the patch
 * that lifted that (05c8482f7f) was reverted two weeks later by 26acb54a13,
 * never having shipped.  So there the substitution is visible in EXPLAIN and
 * changes no plan.  This module is aimed at a build that does allow a parallel
 * SELECT underneath an INSERT, where the sequence default is what takes it
 * away again.  The regression test exercises both halves accordingly: the
 * planner side through an INSERT, and the run-time side through a direct call
 * in a SELECT that does go parallel.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sequence.h"
#include "access/xact.h"
#include "common/relpath.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_sequence.h"
#include "commands/extension.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

#include "enforce_workers.h"

PG_FUNCTION_INFO_V1(seqguard_nextval);

typedef enum SeqguardMode
{
	SEQGUARD_OFF = 0,			/* do nothing at all */
	SEQGUARD_LOG,				/* evaluate and report, change no plan */
	SEQGUARD_ON					/* evaluate, report, and act */
} SeqguardMode;

static const struct config_enum_entry seqguard_mode_options[] = {
	{"off", SEQGUARD_OFF, false},
	{"log", SEQGUARD_LOG, false},
	{"on", SEQGUARD_ON, false},
	{NULL, 0, false}
};

/* Same set nlguard offers, for consistency within the module. */
static const struct config_enum_entry seqguard_loglevel_options[] = {
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

static int	seqguard_mode = SEQGUARD_ON;
static int	seqguard_log_level = DEBUG1;

static planner_hook_type prev_planner_hook = NULL;

/*
 * OID of our replacement function, resolved on first use.  InvalidOid with
 * seqguard_func_resolved set means the SQL half of the extension is not
 * installed in this database, which leaves the feature inert rather than
 * broken.
 */
static Oid	seqguard_func_oid = InvalidOid;
static bool seqguard_func_resolved = false;

/*
 * The magic number every sequence page carries in its special space.  Defined
 * privately in sequence.c as SEQ_MAGIC, so it is repeated here; the point of
 * checking it is to refuse a page that is not a sequence page at all, which is
 * worth a duplicated constant.
 */
#define SEQGUARD_SEQ_MAGIC	0x1717

typedef struct SeqguardWalkContext
{
	bool		apply;			/* rewrite, rather than just count? */
	int			found;			/* candidate calls seen */
	Oid			funcoid;		/* replacement, resolved on the first candidate */
	bool		resolved;		/* has the resolution above been attempted? */
	bool		complained;		/* has the missing-function hint been given? */
} SeqguardWalkContext;

/*
 * seqguard_invalidate
 *		Forget the cached function OID.
 *
 * Registered on the pg_proc syscache, which covers the cases that matter:
 * dropping the extension drops the function, and creating it is what makes a
 * previously failed lookup succeed.  A backend that looked the function up
 * before it existed is otherwise stuck with the negative answer for the rest
 * of its life.
 */
static void
seqguard_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	seqguard_func_oid = InvalidOid;
	seqguard_func_resolved = false;
}

/*
 * seqguard_lookup_func
 *		Find seqguard_nextval(regclass), or InvalidOid if it is not installed.
 *
 * The function is looked up in the schema the extension was created in rather
 * than through search_path, because the queries this module rewrites belong to
 * an application that sets its own search_path and has never heard of us.
 *
 * The properties are verified rather than assumed.  The whole substitution
 * rests on the replacement being PARALLEL RESTRICTED - a mangled or
 * half-upgraded install that says otherwise must leave the query alone, not
 * produce a plan whose safety nobody checked.
 */
static Oid
seqguard_lookup_func(void)
{
	Oid			extoid;
	Oid			nspoid;
	char	   *nspname;
	Oid			argtypes[1] = {REGCLASSOID};
	Oid			funcoid;

	if (seqguard_func_resolved)
		return seqguard_func_oid;

	/* Whatever happens below, do not come back here for every query. */
	seqguard_func_resolved = true;
	seqguard_func_oid = InvalidOid;

	extoid = get_extension_oid("enforce_workers", true);
	if (!OidIsValid(extoid))
		return InvalidOid;

	nspoid = get_extension_schema(extoid);
	if (!OidIsValid(nspoid))
		return InvalidOid;

	nspname = get_namespace_name(nspoid);
	if (nspname == NULL)
		return InvalidOid;

	funcoid = LookupFuncName(list_make2(makeString(nspname),
									   makeString("seqguard_nextval")),
							 1, argtypes, true);
	if (!OidIsValid(funcoid))
		return InvalidOid;

	if (func_parallel(funcoid) != PROPARALLEL_RESTRICTED ||
		get_func_rettype(funcoid) != INT8OID)
		return InvalidOid;

	seqguard_func_oid = funcoid;
	return funcoid;
}

/*
 * seqguard_is_local_temp
 *		Is this OID a relation of the given kind that only this backend sees?
 *
 * Everything the safety argument needs is decided here, and all of it is
 * catalog data available at plan time.  The temp namespace test is not
 * redundant with the persistence test: another session's temporary relation is
 * also RELPERSISTENCE_TEMP, and isTempNamespace() is what distinguishes ours.
 *
 * All three fields come out of one syscache entry rather than through
 * get_rel_relkind() / get_rel_persistence() / get_rel_namespace(), which would
 * be three separate lookups of the same tuple.  This runs on the fast path -
 * once per INSERT that reaches the planner - so the difference is worth the
 * open-coding.
 */
static bool
seqguard_is_local_temp(Oid relid, char relkind)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	bool		result;

	if (!OidIsValid(relid))
		return false;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		return false;

	reltup = (Form_pg_class) GETSTRUCT(tp);

	result = (reltup->relkind == relkind &&
			  reltup->relpersistence == RELPERSISTENCE_TEMP &&
			  isTempNamespace(reltup->relnamespace));

	ReleaseSysCache(tp);

	return result;
}

/*
 * seqguard_target_is_local_temp
 *		Is this an INSERT into a temporary table of this backend?
 *
 * The gate that keeps the tree walk off the fast path.  Every query that could
 * go parallel would otherwise be walked in full looking for a call that, for
 * the workload this module exists for, only ever appears in one place: the
 * source query of an INSERT whose target table has a sequence default.
 *
 * This is a deliberate narrowing, and it does give something up.  A plain
 * SELECT that calls nextval() on a temporary sequence still loses its
 * parallelism, and this module will no longer help it.  That case is rare, the
 * loss is the status quo rather than a regression, and paying a tree walk on
 * every parallel-capable query to catch it is the wrong trade on a workload
 * that plans thousands of statements a minute.
 *
 * Reading the result relation costs one list_nth() and one syscache lookup,
 * and only for an INSERT.  Everything else - which is almost everything - is
 * rejected on parse->commandType alone.
 *
 * A data-modifying CTE is not considered: the INSERT then sits inside a CTE
 * subquery rather than at parse->resultRelation, and standard_planner()
 * refuses parallelism for parse->hasModifyingCTE regardless of anything we do.
 */
static bool
seqguard_target_is_local_temp(Query *parse)
{
	RangeTblEntry *rte;

	if (parse->commandType != CMD_INSERT)
		return false;

	if (parse->resultRelation <= 0 ||
		parse->resultRelation > list_length(parse->rtable))
		return false;

	rte = rt_fetch(parse->resultRelation, parse->rtable);

	if (rte->rtekind != RTE_RELATION)
		return false;

	return seqguard_is_local_temp(rte->relid, RELKIND_RELATION);
}

/*
 * seqguard_walker
 *		Count, report and optionally rewrite the nextval() calls in scope.
 *
 * Rewriting from a walker rather than a mutator is deliberate: only the funcid
 * of an existing node changes, the shape of the tree does not, and the caller
 * has already made a private copy of the Query before setting apply.  A
 * mutator would rebuild the whole tree to change one field.
 *
 * Only a call whose argument is already a Const can be judged here.  That
 * covers the case this module exists for - the rewriter plants the sequence
 * OID as a constant when it expands a column default - and a computed
 * argument, which could name a different sequence on every row, is precisely
 * the case where no plan-time answer exists.
 */
static bool
seqguard_walker(Node *node, SeqguardWalkContext *ctx)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node,
								 seqguard_walker, ctx, 0);

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *fe = (FuncExpr *) node;

		if (fe->funcid == F_NEXTVAL && list_length(fe->args) == 1)
		{
			Node	   *arg = (Node *) linitial(fe->args);

			if (IsA(arg, Const) &&
				((Const *) arg)->consttype == REGCLASSOID &&
				!((Const *) arg)->constisnull)
			{
				Oid			seqid = DatumGetObjectId(((Const *) arg)->constvalue);

				if (seqguard_is_local_temp(seqid, RELKIND_SEQUENCE))
				{
					ctx->found++;

					if (ctx->apply)
					{
						fe->funcid = ctx->funcoid;
						return false;
					}

					/*
					 * Resolving the replacement is deferred to the first
					 * candidate, so that a query without one never touches the
					 * catalog and a database without the SQL half of the
					 * extension is not nagged about queries that would not
					 * have been rewritten anyway.
					 *
					 * It also has to happen before the message below, or the
					 * module would announce a substitution it then turns out
					 * it cannot make.
					 */
					if (seqguard_mode == SEQGUARD_ON && !ctx->resolved)
					{
						ctx->funcoid = seqguard_lookup_func();
						ctx->resolved = true;
					}

					if (seqguard_mode == SEQGUARD_ON &&
						!OidIsValid(ctx->funcoid) && !ctx->complained)
					{
						ctx->complained = true;
						ereport(seqguard_log_level,
								(errmsg("seqguard: seqguard_nextval() is not installed in this database"),
								 errhint("Run CREATE EXTENSION enforce_workers to enable the substitution.")));
					}

					ereport(seqguard_log_level,
							(errmsg("seqguard: %s nextval(\"%s\")",
									OidIsValid(ctx->funcoid) ?
									"substituting" : "would substitute",
									get_rel_name(seqid))));
				}
			}
		}
	}

	return expression_tree_walker(node, seqguard_walker, ctx);
}

/*
 * seqguard_planner
 *		planner_hook entry point.
 *
 * The cheap tests come first and in the same order standard_planner() applies
 * them, so that a query which could not have gone parallel anyway never pays
 * for the tree walk.
 *
 * One of standard_planner()'s tests is deliberately not repeated: commandType
 * == CMD_SELECT.  Community PostgreSQL will not put a Gather under an INSERT
 * at all, but a build that lifts that restriction is exactly where this module
 * earns its keep, and copying the test would quietly disable it there.
 * Substituting in a statement that turns out not to go parallel costs nothing
 * at run time, because the replacement is nextval().
 */
static PlannedStmt *
seqguard_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams)
{
	if (seqguard_mode != SEQGUARD_OFF &&
		(cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
		max_parallel_workers_per_gather > 0 &&
		!IsParallelWorker() &&
		seqguard_target_is_local_temp(parse))
	{
		SeqguardWalkContext ctx;

		memset(&ctx, 0, sizeof(ctx));

		(void) seqguard_walker((Node *) parse, &ctx);

		if (ctx.found > 0 && OidIsValid(ctx.funcoid))
		{
			/*
			 * The rewrite goes into the caller's Query, not into a copy.
			 *
			 * The copy was the cautious choice, and it is not free: these are
			 * 1C's INSERT ... SELECT trees, and copyObject() would deep-copy
			 * one per planning cycle to change a single Oid per call site.
			 *
			 * What the copy protected was a Query that gets planned more than
			 * once - the query_list of a cached plan.  Rewriting that one in
			 * place is safe here because the rewrite is idempotent and cannot
			 * go stale: on the next planning cycle the walker finds our
			 * function rather than nextval() and does nothing, and the fact
			 * that decided the substitution - the target being a temporary
			 * table of this session - cannot change underneath a cached plan.
			 * A temporary table does not become permanent, and if it is
			 * dropped the plan is invalidated with it.
			 *
			 * Two consequences worth knowing.  Setting seqguard.mode = off
			 * afterwards does not un-rewrite a statement that was already
			 * planned; it still executes correctly, because outside parallel
			 * mode the replacement *is* nextval(), but its plan keeps whatever
			 * shape it was given.  And a cached statement rewritten before
			 * DROP EXTENSION refers to a function that no longer exists.
			 * Re-preparing it, or reconnecting, is the answer to both.
			 */
			ctx.apply = true;

			(void) seqguard_walker((Node *) parse, &ctx);
		}
	}

	if (prev_planner_hook)
		return prev_planner_hook(parse, query_string, cursorOptions,
								 boundParams);

	return standard_planner(parse, query_string, cursorOptions, boundParams);
}

/*
 * Per-sequence state, the counterpart of SeqTableData in sequence.c.
 *
 * What is cached here is deliberately *not* the same thing.  The core caches
 * unissued values, which is what the sequence's CACHE setting controls; this
 * caches only the two things that make a call cheap and have nothing to do
 * with the values:
 *
 *   - whether the relation lock has already been taken in this transaction;
 *   - the parameters from pg_sequence, which change only under ALTER SEQUENCE.
 *
 * Caching values as well is not an oversight.  It would open gaps in the
 * numbering, widen the currval() divergence documented at the top of this
 * file, and buy nothing for the workload this exists for, where the sequence
 * behind a serial column has CACHE 1 and the core takes one value per call
 * too.  If a sequence with CACHE > 1 ever turns up in a profile, that is the
 * moment to revisit it - with a measurement.
 *
 * Entries live for the life of the backend, as the core's do.  DISCARD
 * SEQUENCES needs no hook here for the same reason: it exists to forget cached
 * values and currval() state, and this table holds neither.
 */
typedef struct SeqguardSeqEntry
{
	Oid			relid;			/* hash key - must be first */
	LocalTransactionId lxid;	/* lock already held in this transaction? */
	RelFileNumber filenumber;	/* to notice a sequence rewritten under us */
	bool		params_valid;
	int64		incby;
	int64		maxv;
	int64		minv;
	bool		cycle;
} SeqguardSeqEntry;

static HTAB *seqguard_seqhash = NULL;

/*
 * seqguard_seq_invalidate
 *		Drop the cached pg_sequence parameters.
 *
 * ALTER SEQUENCE forces a rewrite for every option that affects the values it
 * will generate, so the relfilenumber test in seqguard_nextval_local() would
 * catch those on its own.  This callback is the belt to that pair of braces:
 * it costs nothing at run time and means the cache does not depend on that
 * property of init_params() staying true.
 */
static void
seqguard_seq_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	SeqguardSeqEntry *entry;

	if (seqguard_seqhash == NULL)
		return;

	hash_seq_init(&status, seqguard_seqhash);
	while ((entry = (SeqguardSeqEntry *) hash_seq_search(&status)) != NULL)
		entry->params_valid = false;
}

/*
 * seqguard_seq_lock
 *		Find the cache entry for this sequence, holding its lock.
 *
 * This is lock_and_open_sequence() without the open.  Taking RowExclusiveLock
 * on every call - which is what a straightforward implementation does, because
 * sequence_open() offers to do it for you - would put a lock manager
 * acquisition on a path that runs once per row, in the leader, above a Gather.
 * The core does not do that, and the reason it does not is this flag.
 *
 * The lock is charged to TopTransactionResourceOwner rather than to whatever
 * owner happens to be current, exactly as the core does and for the same
 * reason: the entry claims the lock is held for the whole transaction, so it
 * must not be released when some portal or executor resource owner goes away.
 * Nothing restores CurrentResourceOwner if LockRelationOid() throws, and that
 * is fine for the same reason it is fine in the core - the error aborts the
 * (sub)transaction, which resets it.
 */
static SeqguardSeqEntry *
seqguard_seq_lock(Oid relid)
{
	SeqguardSeqEntry *entry;
	bool		found;
	LocalTransactionId thislxid = MyProc->vxid.lxid;

	if (seqguard_seqhash == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(SeqguardSeqEntry);

		seqguard_seqhash = hash_create("seqguard sequences", 16, &ctl,
									   HASH_ELEM | HASH_BLOBS);
	}

	entry = (SeqguardSeqEntry *) hash_search(seqguard_seqhash, &relid,
											 HASH_ENTER, &found);

	if (!found)
	{
		entry->lxid = InvalidLocalTransactionId;
		entry->filenumber = InvalidRelFileNumber;
		entry->params_valid = false;
	}

	if (entry->lxid != thislxid)
	{
		ResourceOwner currentOwner;

		currentOwner = CurrentResourceOwner;
		CurrentResourceOwner = TopTransactionResourceOwner;

		LockRelationOid(relid, RowExclusiveLock);

		CurrentResourceOwner = currentOwner;

		entry->lxid = thislxid;
	}

	return entry;
}

/*
 * seqguard_load_params
 *		Refresh the cached pg_sequence parameters for this entry.
 */
static void
seqguard_load_params(SeqguardSeqEntry *entry)
{
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(entry->relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", entry->relid);

	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	entry->incby = pgsform->seqincrement;
	entry->maxv = pgsform->seqmax;
	entry->minv = pgsform->seqmin;
	entry->cycle = pgsform->seqcycle;
	/* seqcache is deliberately not honoured; see SeqguardSeqEntry. */

	ReleaseSysCache(pgstuple);

	entry->params_valid = true;
}

/*
 * seqguard_nextval_local
 *		Advance a temporary sequence without consulting parallel mode.
 *
 * A stripped-down nextval_internal(): no cache of unissued values, and none of
 * the WAL machinery, because RelationNeedsWAL() is false for every relation
 * that reaches this point and the whole logging branch would be dead code.
 * The arithmetic, the bounds handling and the buffer protocol are kept in the
 * same shape as the original so that the two can be compared side by side.
 *
 * The caller has already taken the lock through seqguard_seq_lock() and opened
 * the relation, and has established that it is a local temporary sequence.
 */
static int64
seqguard_nextval_local(Relation seqrel, SeqguardSeqEntry *entry)
{
	Buffer		buf;
	Page		page;
	ItemId		lp;
	HeapTupleData seqtuple;
	Form_pg_sequence_data seq;
	uint32	   *magic;
	int64		incby,
				maxv,
				minv,
				next;
	bool		cycle;

	/*
	 * A sequence replaced transactionally - ALTER SEQUENCE, or a TRUNCATE of
	 * an owning table - gets a new relfilenumber, and the parameters cached
	 * against the old one may describe something else entirely.  The core
	 * makes the same test for the same reason, one line further on in
	 * init_sequence().
	 */
	if (seqrel->rd_rel->relfilenode != entry->filenumber)
	{
		entry->filenumber = seqrel->rd_rel->relfilenode;
		entry->params_valid = false;
	}

	if (!entry->params_valid)
		seqguard_load_params(entry);

	incby = entry->incby;
	maxv = entry->maxv;
	minv = entry->minv;
	cycle = entry->cycle;

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
	SeqguardSeqEntry *entry;
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

	/* Lock first, then open, in that order and for the reasons the core has. */
	entry = seqguard_seq_lock(relid);
	seqrel = sequence_open(relid, NoLock);

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

	result = seqguard_nextval_local(seqrel, entry);

	sequence_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

/*
 * seqguard_init
 *		Define the GUCs and chain the hook.  Called from _PG_init() only.
 */
void
seqguard_init(void)
{
	DefineCustomEnumVariable("seqguard.mode",
							 "Controls how nextval() on a temporary sequence is treated.",
							 "off leaves planning alone; log reports the calls that would be substituted; on substitutes a parallel-restricted equivalent so the rest of the query can still go parallel.",
							 &seqguard_mode,
							 SEQGUARD_ON,
							 seqguard_mode_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("seqguard.log_level",
							 "Message level at which substituted calls are reported.",
							 NULL,
							 &seqguard_log_level,
							 DEBUG1,
							 seqguard_loglevel_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("seqguard");

	CacheRegisterSyscacheCallback(PROCOID, seqguard_invalidate, (Datum) 0);
	CacheRegisterSyscacheCallback(SEQRELID, seqguard_seq_invalidate, (Datum) 0);

	prev_planner_hook = planner_hook;
	planner_hook = seqguard_planner;
}
