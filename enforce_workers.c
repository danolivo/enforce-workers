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
 * never applies.  No catalogs are touched and nothing is persisted: set
 * enforce_workers.mode = off, or unload the library, and the planner is back to
 * its usual behaviour.
 *
 * Load it with
 *		LOAD 'enforce_workers';
 * or by listing it in session_preload_libraries / shared_preload_libraries.
 * enforce_workers.mode defaults to "on", in keeping with the rest of the
 * module, so the override is in effect the moment the library is loaded.
 *
 * The module has grown two further, unrelated planner tweaks, in nlguard.c and
 * seqguard.c, and one executor tweak, in idxdefer.c.  Each feature keeps to its
 * own file and exposes one initialiser; _PG_init() at the bottom of this file
 * is the only entry point and the only place that decides what gets installed.
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
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "enforce_workers.h"

PG_MODULE_MAGIC;

typedef enum EnforceWorkersMode
{
	ENFORCE_WORKERS_OFF = 0,	/* do nothing at all */
	ENFORCE_WORKERS_LOG,		/* evaluate and report, change no plan */
	ENFORCE_WORKERS_ON			/* evaluate, report, and act */
} EnforceWorkersMode;

static const struct config_enum_entry enforce_workers_mode_options[] = {
	{"off", ENFORCE_WORKERS_OFF, false},
	{"log", ENFORCE_WORKERS_LOG, false},
	{"on", ENFORCE_WORKERS_ON, false},
	{NULL, 0, false}
};

/* Same set the other features offer, for consistency within the module. */
static const struct config_enum_entry enforce_workers_loglevel_options[] = {
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

static int	enforce_workers_mode = ENFORCE_WORKERS_ON;
static int	enforce_workers_log_level = DEBUG1;

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;

static void
enforce_workers_get_relation_info(PlannerInfo *root, Oid relationObjectId,
								  bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

	if (enforce_workers_mode == ENFORCE_WORKERS_OFF)
		return;

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
	 * Both tests above come before the report, so that log mode names exactly
	 * the relations on mode would have acted on and no others.
	 *
	 * ereport() does not evaluate its arguments unless the level is
	 * interesting, so get_rel_name() - a syscache lookup and a palloc - costs
	 * nothing at the default DEBUG1.  It is worth remembering that this hook
	 * runs once per base relation per planning cycle before raising
	 * enforce_workers.log_level on a busy system.
	 */
	ereport(enforce_workers_log_level,
			(errmsg("enforce_workers: %s parallel_workers = %d on \"%s\"",
					(enforce_workers_mode == ENFORCE_WORKERS_ON) ?
					"setting" : "would set",
					max_parallel_workers_per_gather,
					get_rel_name(relationObjectId))));

	if (enforce_workers_mode != ENFORCE_WORKERS_ON)
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
 *		Define the GUCs and install the relation-size override.
 *
 * Kept separate from _PG_init() so that every feature in the module is
 * initialised the same way, whichever file it lives in.
 */
static void
enforce_workers_init(void)
{
	DefineCustomEnumVariable("enforce_workers.mode",
							 "Controls whether the relation-size gate on parallelism applies.",
							 "off leaves planning alone; log reports the relations that would be affected; on gives every relation as many workers as max_parallel_workers_per_gather allows, whatever its size.",
							 &enforce_workers_mode,
							 ENFORCE_WORKERS_ON,
							 enforce_workers_mode_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("enforce_workers.log_level",
							 "Message level at which affected relations are reported.",
							 NULL,
							 &enforce_workers_log_level,
							 DEBUG1,
							 enforce_workers_loglevel_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("enforce_workers");

	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = enforce_workers_get_relation_info;
}

void
_PG_init(void)
{
	enforce_workers_init();
	nlguard_init();
	seqguard_init();
	idxdefer_init();
}
