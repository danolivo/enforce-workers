/*-------------------------------------------------------------------------
 *
 * enforce_workers.c
 *		Make parallel workers available on every relation, regardless of size,
 *		and host the module's single entry point.
 *
 * The planner decides how many parallel workers a scan may use in
 * compute_parallel_worker().  When RelOptInfo->rel_parallel_workers is left at
 * -1, that number is derived from the relation size, and a relation smaller
 * than min_parallel_table_scan_size gets no partial paths at all.  Setting the
 * field explicitly - which is exactly what the parallel_workers storage
 * parameter does - skips the size test entirely.
 *
 * This module fills the field in from get_relation_info_hook, so the size test
 * never applies.  No catalogs are touched and nothing is persisted: unload the
 * library and the planner is back to its usual behaviour.
 *
 * Load it with
 *		LOAD 'enforce_workers';
 * or by listing it in session_preload_libraries / shared_preload_libraries.
 *
 * The module has grown two further, unrelated planner tweaks, in nlguard.c and
 * seqguard.c.  Each feature keeps to its own file and exposes one initialiser;
 * _PG_init() at the bottom of this file is the only entry point and the only
 * place that decides what gets installed.
 *
 * seqguard is the one feature with an SQL half: its replacement function has
 * to exist in pg_proc for the planner to point a call at it.  Loading the
 * library is still enough for everything else, and seqguard simply does
 * nothing in a database where CREATE EXTENSION has not been run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"

#include "enforce_workers.h"

PG_MODULE_MAGIC;

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;

static void
enforce_workers_get_relation_info(PlannerInfo *root, Oid relationObjectId,
								  bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

	/*
	 * The parent of an inheritance or partitioning tree is never scanned
	 * itself; its children come through this hook separately.
	 */
	if (inhparent)
		return;

	/*
	 * A value other than -1 came from the parallel_workers storage parameter.
	 * That is an explicit decision made by the DBA - including the value 0,
	 * which disables parallelism for the relation - so leave it alone.
	 */
	if (rel->rel_parallel_workers != -1)
		return;

	/*
	 * compute_parallel_worker() clamps the result to the maximum its caller
	 * supplies anyway, so asking for more than max_parallel_workers_per_gather
	 * would change nothing for scan paths.
	 */
	rel->rel_parallel_workers = max_parallel_workers_per_gather;
}

/*
 * enforce_workers_init
 *		Install the relation-size override.
 *
 * Kept separate from _PG_init() so that every feature in the module is
 * initialised the same way, whichever file it lives in.
 */
static void
enforce_workers_init(void)
{
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = enforce_workers_get_relation_info;
}

void
_PG_init(void)
{
	enforce_workers_init();
	nlguard_init();
	seqguard_init();
}
