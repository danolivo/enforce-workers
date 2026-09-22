/*-------------------------------------------------------------------------
 *
 * nlguard.c
 *		Find the plain nested loops in a plan - the ones that read the whole
 *		inner side once per outer row - and report them.
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
 * early.
 *
 * At this stage the module only reports.  Load it with
 *		LOAD 'enforce_workers';
 * and set nlguard.mode = log; it does nothing by default.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "utils/guc.h"

#include "enforce_workers.h"

typedef enum NlguardMode
{
	NLGUARD_OFF = 0,			/* do nothing at all */
	NLGUARD_LOG					/* evaluate and report, change no plan */
} NlguardMode;

static const struct config_enum_entry nlguard_mode_options[] = {
	{"off", NLGUARD_OFF, false},
	{"log", NLGUARD_LOG, false},
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

static int	nlguard_mode = NLGUARD_OFF;
static int	nlguard_log_level = DEBUG1;

static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;

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
 * nlguard_should_report
 *		Is this nested loop the plain kind?
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
nlguard_should_report(NestPath *nl)
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
 *		Is this path one we should report?
 *
 * outerrel and innerrel are the pair add_paths_to_joinrel() is currently
 * working on.
 */
static bool
nlguard_is_candidate(Path *path, RelOptInfo *outerrel, RelOptInfo *innerrel)
{
	NestPath   *nl;

	if (!IsA(path, NestPath))
		return false;

	nl = (NestPath *) path;

	/*
	 * Only look at paths built by the current call, so that each one is
	 * reported once rather than once per pair of input relations that can
	 * form this joinrel.
	 */
	if (!bms_equal(nl->jpath.outerjoinpath->parent->relids, outerrel->relids) ||
		!bms_equal(nl->jpath.innerjoinpath->parent->relids, innerrel->relids))
		return false;

	return nlguard_should_report(nl);
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
	List	   *found = NIL;
	ListCell   *lc;
	char		relidbuf[NLGUARD_RELIDS_BUFLEN];

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

	foreach(lc, joinrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (nlguard_is_candidate(path, outerrel, innerrel))
			found = lappend(found, path);
	}

	foreach(lc, joinrel->partial_pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (nlguard_is_candidate(path, outerrel, innerrel))
			found = lappend(found, path);
	}

	if (found == NIL)
		return;

	/*
	 * ereport() does not evaluate its arguments unless the level is
	 * interesting, so the formatting below costs nothing at the default
	 * DEBUG1.  Raising nlguard.log_level for a calibration run makes the
	 * logger process part of the measurement; keep that in mind before
	 * setting it to "log" on a busy system.
	 */
	nlguard_format_relids(joinrel->relids, relidbuf, sizeof(relidbuf));

	foreach(lc, found)
	{
		NestPath   *nl = (NestPath *) lfirst(lc);

		/*
		 * The estimates go in the detail field, not the primary message, so
		 * that a caller who only wants to know whether the module fired is
		 * not reading numbers that shift with the statistics.
		 */
		ereport(nlguard_log_level,
				(errmsg("nlguard: would penalise nested loop over join (%s)",
						relidbuf),
				 errdetail("Outer rows %.0f, inner rows %.0f, total cost %.2f.",
						   nl->jpath.outerjoinpath->rows,
						   nl->jpath.innerjoinpath->rows,
						   nl->jpath.path.total_cost)));
	}

	list_free(found);
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
							 "off leaves planning alone; log reports the joins that would be affected.",
							 &nlguard_mode,
							 NLGUARD_OFF,
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
