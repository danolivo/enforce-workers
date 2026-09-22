/*-------------------------------------------------------------------------
 *
 * nlguard.c
 *		Refuse the plain nested loop - the one that reads its whole inner side
 *		once per outer row - wherever the core can offer anything else.
 *
 * The cost of a nested loop grows linearly with the number of outer rows:
 *
 *		run_cost += inner_run_cost;
 *		if (outer_path_rows > 1)
 *			run_cost += (outer_path_rows - 1) * inner_rescan_run_cost;
 *
 * The cost of a hash join barely grows with it at all - the inner side goes
 * into startup_cost and the outer side only adds a few operator costs per
 * row.  So an N-fold underestimate of the outer row count understates the
 * nested loop by roughly N and the hash join by almost nothing.  The planner
 * compares the two numbers without knowing that one of them is far more
 * sensitive to error than the other, and picks the fragile plan whenever the
 * estimate happens to favour it.
 *
 * That this is worth acting on, and that it is worth acting on without a row
 * threshold, is not our finding:
 *
 *		Viktor Leis, Andrey Gubichev, Atanas Mirchev, Peter Boncz,
 *		Alfons Kemper, Thomas Neumann.  "How Good Are Query Optimizers,
 *		Really?"  PVLDB 9(3):204-215, 2015.
 *		https://www.vldb.org/pvldb/vol9/p204-leis.pdf
 *
 * Leis et al. identify the nested loop chosen without an index lookup, on the
 * strength of a low cardinality estimate, as a leading cause of catastrophic
 * plans; disabling that one join method removed the timeouts from their
 * benchmark outright.  Their principle - an algorithm that seldom offers a
 * large benefit over a more robust one should not be chosen - is the whole of
 * the policy below.
 *
 * This is not a claim that nested loops are bad.  An index nested loop, where
 * the inner path draws parameters from the outer relation, is exactly the
 * case Leis et al. exclude, and so does this module.  The same goes for the
 * joins that stop after the first match, and for any query that wants rows
 * early.  There is deliberately no row-count threshold: a threshold would be
 * a calibration against one workload dressed up as a rule, and the decision
 * would rest on the very estimate whose reliability is in question.
 *
 * Mechanism
 * ---------
 * The obvious implementation - walk joinrel->pathlist in
 * set_join_pathlist_hook and bump disabled_nodes on the offending nested loop
 * - does not work on its own.  The hook runs at the very end of
 * add_paths_to_joinrel(), by which point add_path() has already discarded the
 * hash and merge paths that lost to the nested loop on cost.  Penalising the
 * nested loop after the fact leaves the joinrel with nothing to fall back to.
 *
 * Rather than rebuild those alternatives here - which would mean carrying a
 * copy of hash_inner_and_outer() and keeping it in step with every major
 * release - we penalise the nested loop and then ask the core to generate the
 * join paths again with enable_nestloop turned off.  The second pass rebuilds
 * the hash and merge paths with the same code that built them the first time;
 * they are no longer dominated, because the nested loop now carries a
 * disabled node, so add_path() keeps them.  If no alternative is possible -
 * no hashable or mergeable clause - the second pass adds nothing and the
 * penalised nested loop simply remains the only path, so planning cannot
 * fail.
 *
 * Two prices are paid for that.  add_paths_to_joinrel() runs twice for the
 * joins we act on, so anything it does besides generating paths also happens
 * twice; in particular GetForeignJoinPaths() is called a second time for a
 * foreign join, which postgres_fdw tolerates by way of its fdw_private check
 * but a third-party FDW need not.  And planning gets slower, in proportion to
 * how often the module fires - which, with no row threshold, is most joins
 * that have a plain nested loop at all.  Measure it before turning this on.
 *
 * There is one join we can rule out in advance, and we do: a join with no
 * clause at all.  Both hash_inner_and_outer() and select_mergejoin_clauses()
 * draw their clauses from extra->restrictlist, so an empty list means neither
 * can produce a path, and the second pass is provably wasted.  Without that
 * test a two-relation cross join is the worst case in the whole module: it
 * produces four candidates rather than one - match_unsorted_outer() offers a
 * plain and a materialised inner path, and populate_joinrel_with_paths() runs
 * both join orders - nothing dominates any of them, and each costs a pass
 * that comes back empty-handed.
 *
 * The test has to stop there, though.  "No clause we could hash or merge on"
 * looks like the natural generalisation, and it is wrong: an FDW's
 * GetForeignJoinPaths() can offer a path for a join qual of any shape, and
 * that path may have been dominated by the nested loop in the first pass just
 * like a hash path would be.  Hence the fdwroutine test alongside the empty
 * list - together they are the only case where "no alternative exists" can be
 * stated rather than guessed.
 *
 * Load it with
 *		LOAD 'enforce_workers';
 * and the module is active: nlguard.mode defaults to "on", in keeping with
 * enforce_workers in the same library, which also takes effect the moment it
 * is loaded.  Set nlguard.mode = off to get the core planner back, or = log
 * to see which joins would be affected without changing any plan.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "utils/guc.h"

#include "enforce_workers.h"

typedef enum NlguardMode
{
	NLGUARD_OFF = 0,			/* do nothing at all */
	NLGUARD_LOG,				/* evaluate and report, change no plan */
	NLGUARD_ON					/* evaluate, report, and act */
} NlguardMode;

static const struct config_enum_entry nlguard_mode_options[] = {
	{"off", NLGUARD_OFF, false},
	{"log", NLGUARD_LOG, false},
	{"on", NLGUARD_ON, false},
	{NULL, 0, false}
};

/* Same set auto_explain offers, for consistency. */
static const struct config_enum_entry nlguard_loglevel_options[] = {
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

static int	nlguard_mode = NLGUARD_ON;
static int	nlguard_log_level = DEBUG1;

static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;

/*
 * Set while we are inside our own call to add_paths_to_joinrel().  The core
 * calls set_join_pathlist_hook at the end of every such call, so without this
 * we would recurse until the stack ran out.
 */
static bool nlguard_regenerating = false;

/* Enough for the relid sets that appear in a log line; longer sets truncate. */
#define NLGUARD_RELIDS_BUFLEN	128

/*
 * nlguard_format_relids
 *		Render a relid set as "1 3 4" into a caller-supplied buffer.
 *
 * Deliberately allocation-free: this runs inside path generation, and the
 * result only ever goes into a log message, so an oversized set is truncated
 * rather than grown.
 */
static void
nlguard_format_relids(Relids relids, char *buf, int buflen)
{
	int			x = -1;
	int			off = 0;

	Assert(buf != NULL);
	Assert(buflen >= 4);

	buf[0] = '\0';

	while ((x = bms_next_member(relids, x)) >= 0)
	{
		int			n;

		n = snprintf(buf + off, buflen - off, "%s%d", (off > 0) ? " " : "", x);

		if (n < 0 || n >= buflen - off)
		{
			strcpy(buf + buflen - 4, "...");
			return;
		}

		off += n;
	}
}

/*
 * nlguard_should_penalise
 *		Is this nested loop the plain kind, the one we refuse?
 *
 * Everything this reads comes out of the path itself.  That is not a matter
 * of taste: add_paths_to_joinrel() runs once per pair of input relations for
 * the same joinrel, and joinrel->pathlist accumulates across those calls, so
 * the hook's own outerrel and extra arguments describe only the most recent
 * pair.  Judging a path built by an earlier call against them misreads it -
 * and the way it misreads it is to mistake an index nested loop for a plain
 * one, which is the single worst thing this module could do.  The caller
 * filters by pair as well, but the policy does not depend on it having done
 * so.
 */
static bool
nlguard_should_penalise(NestPath *nl)
{
	Path	   *inner = nl->jpath.innerjoinpath;
	Relids		outer_relids;

	Assert(nl != NULL);

	outer_relids = nl->jpath.outerjoinpath->parent->relids;

	/*
	 * A join path that is itself parameterised sits on the inner side of some
	 * higher join and will be rescanned from there.  That is a different
	 * trade-off from the one being judged here, and the row counts that would
	 * drive the decision are the parameterised ones.
	 */
	if (nl->jpath.path.param_info != NULL)
		return false;

	/*
	 * The test that matters most.  An index nested loop - inner path drawing
	 * parameters from the outer relation - does not read the inner side
	 * whole; it fetches a few rows per outer row, and the linear term this
	 * module worries about is exactly what makes it fast.
	 *
	 * Note that such a path has param_info == NULL at the join level, because
	 * the parameters are satisfied inside the join, so the test above does
	 * not cover this case.  Checking only param_info would put every ordinary
	 * index nested loop in scope, which is the opposite of the intent.
	 */
	if (bms_overlap(PATH_REQ_OUTER(inner), outer_relids))
		return false;

	/*
	 * initial_cost_nestloop() takes a separate branch when the executor stops
	 * after the first match.  There the inner side is not read whole either,
	 * and the nested loop wins honestly.
	 *
	 * Use the path's own flag rather than extra->inner_unique for the reason
	 * given in the function comment.
	 */
	if (nl->jpath.inner_unique)
		return false;

	switch (nl->jpath.jointype)
	{
		case JOIN_SEMI:
		case JOIN_ANTI:
			return false;
		default:
			break;
	}

	return true;
}

/*
 * nlguard_is_candidate
 *		Is this path one we should act on, and have we not already?
 *
 * outerrel and innerrel are the pair add_paths_to_joinrel() is currently
 * working on.
 */
static bool
nlguard_is_candidate(Path *path, RelOptInfo *outerrel, RelOptInfo *innerrel)
{
	NestPath   *nl;
	int			undisturbed;

	if (!IsA(path, NestPath))
		return false;

	nl = (NestPath *) path;

	/*
	 * Only touch paths built by the current call.  The alternatives we are
	 * about to ask for are generated for this pair of input relations, so
	 * acting on a path left over from another pair would penalise it without
	 * rebuilding the alternative that would naturally replace it.  A path
	 * from an earlier pair has already been through this on the call that
	 * created it.
	 */
	if (!bms_equal(nl->jpath.outerjoinpath->parent->relids, outerrel->relids) ||
		!bms_equal(nl->jpath.innerjoinpath->parent->relids, innerrel->relids))
		return false;

	/*
	 * Skip a path that already carries a penalty.  initial_cost_nestloop()
	 * sets disabled_nodes to the sum over the two input paths, plus one when
	 * enable_nestloop is off, so anything above that sum was either penalised
	 * by us or produced by our own regeneration pass.
	 *
	 * This also makes the module keep its hands off entirely when the user
	 * has set enable_nestloop = off for the session: every nested loop is
	 * then already above the sum, and there is nothing left for us to say.
	 */
	undisturbed = nl->jpath.outerjoinpath->disabled_nodes +
		nl->jpath.innerjoinpath->disabled_nodes;

	if (path->disabled_nodes > undisturbed)
		return false;

	return nlguard_should_penalise(nl);
}

/*
 * nlguard_set_join_pathlist
 *		set_join_pathlist_hook entry point.
 */
static void
nlguard_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
						  RelOptInfo *outerrel, RelOptInfo *innerrel,
						  JoinType jointype, JoinPathExtraData *extra)
{
	List	   *victims = NIL;
	ListCell   *lc;
	bool		save_enable_nestloop;
	char		relidbuf[NLGUARD_RELIDS_BUFLEN];

	/*
	 * Do nothing on the way back in from our own add_paths_to_joinrel() call.
	 * The previous hook in the chain is skipped too: it has already been
	 * shown this joinrel once, and showing it the same joinrel a second time
	 * because of an internal pass of ours would be the more surprising of the
	 * two behaviours.
	 */
	if (nlguard_regenerating)
		return;

	if (prev_set_join_pathlist_hook)
		prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									jointype, extra);

	if (nlguard_mode == NLGUARD_OFF)
		return;

	/*
	 * The query-level test comes before the path list scans, so that a query
	 * with a LIMIT costs us nothing beyond this comparison.
	 *
	 * Somebody upstream wants rows early: a cheap start is worth more than a
	 * cheap total, and a plain nested loop is often how you get one.  A
	 * negative tuple_fraction is an absolute row count rather than a
	 * fraction, so test against zero rather than for a value below one.
	 *
	 * joinrel->consider_startup is not tested separately: build_join_rel()
	 * sets it to exactly (root->tuple_fraction > 0), so this covers it and
	 * the negative case as well.  Nor is consider_param_startup, which
	 * set_base_rel_consider_startup() in allpaths.c only ever sets on base
	 * relations - "there is no provision for consider_param_startup to get
	 * set at all on joinrels".
	 */
	if (root->tuple_fraction != 0.0)
		return;

	/*
	 * A join with no clause is the one case where the second pass can be
	 * ruled out in advance rather than attempted and found useless.
	 * hash_inner_and_outer() and select_mergejoin_clauses() both build from
	 * extra->restrictlist, so an empty list means neither can produce a path,
	 * and an unparameterised nested loop is the only way to compute this join
	 * at all.  Leaving it unpenalised also keeps a disabled node out of every
	 * path built on top of it, and out of EXPLAIN, where it would have said
	 * only that this module had been past.
	 *
	 * The fdwroutine test is not decoration.  GetForeignJoinPaths() runs from
	 * add_paths_to_joinrel() whatever the clauses look like - including none -
	 * and its path competes in add_path() like any other, so for a foreign
	 * join the second pass still has something to rebuild.
	 */
	if (extra->restrictlist == NIL && joinrel->fdwroutine == NULL)
		return;

	/*
	 * Collect first, act later.  add_path() deletes and pfrees the paths it
	 * rejects, so the path lists must not be walked while they are being
	 * modified, and nothing collected here may be dereferenced once
	 * add_paths_to_joinrel() has run again below.
	 */
	foreach(lc, joinrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (nlguard_is_candidate(path, outerrel, innerrel))
			victims = lappend(victims, path);
	}

	foreach(lc, joinrel->partial_pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (nlguard_is_candidate(path, outerrel, innerrel))
			victims = lappend(victims, path);
	}

	if (victims == NIL)
		return;

	/*
	 * ereport() does not evaluate its arguments unless the level is
	 * interesting, so the formatting below costs nothing at the default
	 * DEBUG1.  Raising nlguard.log_level for a calibration run makes the
	 * logger process part of the measurement; keep that in mind before
	 * setting it to "log" on a busy system.
	 */
	nlguard_format_relids(joinrel->relids, relidbuf, sizeof(relidbuf));

	foreach(lc, victims)
	{
		NestPath   *nl = (NestPath *) lfirst(lc);

		/*
		 * The estimates go in the detail field, not the primary message, so
		 * that a caller who only wants to know whether the module fired is
		 * not reading numbers that shift with the statistics.
		 */
		ereport(nlguard_log_level,
				(errmsg("nlguard: %s nested loop over join (%s)",
						(nlguard_mode == NLGUARD_ON) ? "penalising" : "would penalise",
						relidbuf),
				 errdetail("Outer rows %.0f, inner rows %.0f, total cost %.2f.",
						   nl->jpath.outerjoinpath->rows,
						   nl->jpath.innerjoinpath->rows,
						   nl->jpath.path.total_cost)));
	}

	if (nlguard_mode != NLGUARD_ON)
	{
		list_free(victims);
		return;
	}

	/*
	 * Penalise before regenerating.  An untouched nested loop would still
	 * dominate the alternatives we are about to ask for, and add_path() would
	 * throw them away again on arrival.
	 */
	foreach(lc, victims)
	{
		Path	   *path = (Path *) lfirst(lc);

		path->disabled_nodes++;
	}

	list_free(victims);
	victims = NIL;

	/*
	 * Ask the core for the join paths a second time, with nested loops
	 * disabled, so that the hash and merge paths discarded during the first
	 * pass are rebuilt by the code that owns that job.  Duplicates of the
	 * paths that are already present cost nothing: add_path() finds them
	 * equal in cost, pathkeys and parameterisation, and rejects them.
	 *
	 * We assign to enable_nestloop directly rather than going through the GUC
	 * machinery, which is what try_nestloop_path() reads and is all we need.
	 * It does mean that anything executing SQL underneath this call - an FDW
	 * callback, say - would both see and plan with nested loops off.
	 *
	 * The restore goes in PG_FINALLY rather than at the end of the block
	 * because anything under add_paths_to_joinrel() may throw, and leaving a
	 * planner GUC flipped would then affect every later statement in the
	 * session.
	 */
	save_enable_nestloop = enable_nestloop;
	enable_nestloop = false;
	nlguard_regenerating = true;

	PG_TRY();
	{
		add_paths_to_joinrel(root, joinrel, outerrel, innerrel, jointype,
							 extra->sjinfo, extra->restrictlist);
	}
	PG_FINALLY();
	{
		enable_nestloop = save_enable_nestloop;
		nlguard_regenerating = false;
	}
	PG_END_TRY();
}

/*
 * nlguard_init
 *		Define the GUCs and chain the hook.  Called from _PG_init() only.
 */
void
nlguard_init(void)
{
	DefineCustomEnumVariable("nlguard.mode",
							 "Controls how plain nested loops are treated.",
							 "off leaves planning alone; log reports the joins that would be affected; on penalises them and lets the planner rebuild the alternatives.",
							 &nlguard_mode,
							 NLGUARD_ON,
							 nlguard_mode_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("nlguard.log_level",
							 "Message level at which affected joins are reported.",
							 NULL,
							 &nlguard_log_level,
							 DEBUG1,
							 nlguard_loglevel_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("nlguard");

	prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = nlguard_set_join_pathlist;
}
