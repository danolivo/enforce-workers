# enforce_workers

Three unrelated planner overrides in one loadable module: `enforce_workers`
below, then `nlguard`, then `seqguard`. Each lives in its own file and can be
switched off independently; `_PG_init()` is the only entry point.

**All three are active the moment the library is loaded.** `enforce_workers`
has no switch at all, and both `nlguard.mode` and `seqguard.mode` default to
`on`. Loading this library changes plans — that is what it is for — so do not
put it in `shared_preload_libraries` of a server you have not measured it on.

`seqguard` additionally needs `CREATE EXTENSION enforce_workers` in each
database where it should work, because it substitutes a function and that
function has to exist in `pg_proc`. Without it the feature is inert; the other
two are unaffected.

## enforce_workers

Removes the relation-size gate on parallelism. The module installs
`get_relation_info_hook` and sets `RelOptInfo->rel_parallel_workers` to
`max_parallel_workers_per_gather` for every base relation that does not already
carry an explicit `parallel_workers` storage parameter.

That is the same field the storage parameter writes, so the effect is identical
to running `ALTER TABLE ... SET (parallel_workers = N)` on everything — without
touching a single catalog row.

## nlguard

Refuses one shape of nested loop — the plain one: unparameterised, reading its
whole inner side once per outer row. The cost of such a loop is linear in the
outer row count while the cost of the equivalent hash join is almost flat, so
an N-fold underestimate of the outer side understates the nested loop by
roughly N and the hash join by nothing. The planner compares the two numbers
without knowing that, and picks the fragile plan whenever the estimate happens
to favour it.

That this is worth acting on, and worth acting on without a row threshold, is
not our finding:

> Viktor Leis, Andrey Gubichev, Atanas Mirchev, Peter Boncz, Alfons Kemper,
> Thomas Neumann. **How Good Are Query Optimizers, Really?**
> PVLDB 9(3):204–215, 2015.
> <https://www.vldb.org/pvldb/vol9/p204-leis.pdf>

Leis et al. identify the nested loop chosen without an index lookup, on the
strength of a low cardinality estimate, as a leading cause of catastrophic
plans; disabling that one join method removed the timeouts from their
benchmark outright. Their principle — an algorithm that seldom offers a large
benefit over a more robust one should not be chosen — is the whole of the
policy here.

Everything else is left alone: index nested loops (inner path parameterised by
the outer relation), which is the case Leis et al. exclude as well;
`inner_unique` and SEMI/ANTI joins that stop after the first match; and
anything under a LIMIT or a cursor.

There is no row-count threshold, deliberately. A threshold would be a
calibration against one workload dressed up as a rule, and the decision would
rest on the very estimate whose reliability is in question.

### How it acts

`set_join_pathlist_hook` runs at the end of `add_paths_to_joinrel()`, after
`add_path()` has already discarded the hash and merge paths that lost to the
nested loop. Bumping `disabled_nodes` at that point would leave the joinrel
with no alternative to fall back to.

So instead the module penalises the nested loop and then calls
`add_paths_to_joinrel()` again with `enable_nestloop = false`. The core
rebuilds its own hash and merge paths, which are no longer dominated; the
module carries no copy of `hash_inner_and_outer()` and nothing to re-sync each
major release. If no alternative is possible the second pass adds nothing, the
penalised loop stays, and planning still succeeds.

### GUCs

| name | type | default | meaning |
|---|---|---|---|
| `nlguard.mode` | enum | `on` | `off` / `log` / `on` |
| `nlguard.log_level` | enum | `debug1` | level for the report, as in `auto_explain` |

`off` gives the core planner back, unchanged. `log` evaluates the rule and
reports every join it matches, without changing a single plan. Raising
`log_level` to `log` makes the logger process part of the measurement, so on a
busy system prefer `debug1` with `log_min_messages` set for the duration of
the run.

The visible record of the module acting is `EXPLAIN`, which reports a
penalised node as `Disabled: true` when it survives anyway.

### Planning cost

The second pass is not free. Measured on a PG 18.6 debug build, EXPLAIN-only
pgbench, single client:

| query | mode=off | mode=on | |
|---|---|---|---|
| 4-way, 3 joins matched | 3489 tps | 3399 tps | −2.6% |
| 7-way chain, 4 joins matched | 1762 tps | 1568 tps | −11% |

Measure your own workload before turning it on; the cost scales with how often
the module fires, and without a row threshold it fires on most joins that have
a plain nested loop at all.

One join is ruled out in advance rather than attempted: a join with no clause
at all. `hash_inner_and_outer()` and `select_mergejoin_clauses()` both build
from the restrict list, so an empty list means neither can produce a path, and
a nested loop is the only way to compute the join. Such a join is skipped
outright — not penalised, not reported, no second pass.

It is worth having. Without the test a two-relation cross join is the worst
case in the module: it produces four candidates rather than one —
`match_unsorted_outer()` offers a plain and a materialised inner path, and
both join orders are tried — nothing dominates any of them, and each costs a
pass that comes back empty-handed. Measured, the notices for one cross join go
from four to none.

The test stops there. "No clause that could be hashed or merged" looks like
the natural generalisation and is wrong: an FDW's `GetForeignJoinPaths()` can
offer a path for a join qual of any shape, and that path can have been
dominated by the nested loop in the first pass exactly as a hash path would
be. So the skip also requires `joinrel->fdwroutine == NULL`. Together the two
conditions are the only case where "no alternative exists" can be stated
rather than guessed; a non-equality clause such as `a.x > b.x` still costs a
pass to find out.

### Caveats

* `add_paths_to_joinrel()` runs twice for the joins that match, so everything
  it does besides generating paths happens twice — notably
  `GetForeignJoinPaths()`. `postgres_fdw` guards against that with its
  `fdw_private` check; a third-party FDW need not.
* The policy reads the path, never the hook's arguments. `pathlist`
  accumulates across the calls for different pairs of input relations, so a
  path is judged by its own outer relation and its own `inner_unique`. Judging
  it by the current call's mistakes an index nested loop for a plain one; a
  7-way join flags 14 paths that way instead of 3.
* Penalising a path in place means a loop pardoned on one call for a joinrel
  competes on later calls against loops of a different join order that have
  not been evaluated yet. There is no hook between the last
  `add_paths_to_joinrel()` for a joinrel and `set_cheapest()`, so this cannot
  be fixed from an extension.
* `disabled_nodes` outranks cost outright, and there is no dial. That is
  deliberate, and it needs no guard: the damage either way is asymmetric by
  construction. Replacing a nested loop that really did have a tiny outer side
  costs one hash build over the inner side — a small constant factor, or a log
  factor if lost pathkeys force a sort. Keeping a nested loop whose outer
  cardinality was underestimated N-fold costs N. A plan-time guard would have
  to be computed from the estimates the module exists to distrust, and in the
  workload that motivated it — a large unanalysed temp table on the inner side
  — it would fire against its own purpose. If a limit is ever wanted it has to
  come from measurement rather than estimation: observed per-query-form error,
  or runtime correction.
* `nlguard.mode` defaults to `on`, so loading this library for
  `enforce_workers` alone still changes join methods. Set `nlguard.mode = off`
  if that is not what you want.
* Setting `enable_nestloop = off` for the session disables the module
  implicitly: every nested loop already carries a penalty and it finds nothing
  to act on.

### Tests

    make check                  # temp instance
    make installcheck           # against a running server

`sql/nlguard.sql` covers:

* a plain nested loop being replaced by a hash join;
* an index nested loop surviving untouched;
* both together in a four-way join — the case where judging a path by the
  hook's arguments instead of by the path would misfire;
* every reason the policy has for standing aside: LIMIT, a cursor, a
  semijoin, an antijoin, and `inner_unique` proved through a DISTINCT
  subquery;
* every reason no alternative exists: a join with no hashable and no
  mergeable clause, and a join with no clause at all — planning must still
  succeed and the surviving loop is reported as `Disabled: true`;
* `enable_nestloop = off`, where the module must find nothing to do;
* `log` mode changing no plan;
* the rewritten plans returning the same rows as the originals.

## seqguard

Stops one `nextval()` on a temporary sequence from costing a whole query its
parallelism.

A table with a sequence default — a `serial` column, an identity column, an
explicit `DEFAULT nextval(...)` — gets that expression expanded into the source
query of any `INSERT` that omits the column. `nextval()` is marked
`PROPARALLEL_UNSAFE`, and `standard_planner()` takes the worst hazard anywhere
in the `Query` and applies it to all of it:

    glob->maxParallelHazard = max_parallel_hazard(parse);
    glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);

So a single call in the topmost target list disables parallelism for the scans,
sorts and joins underneath it that never touch the sequence. This is the shape
1C produces for nearly every intermediate result: a temporary table with a
serial column, filled by `INSERT ... SELECT`, where the `SELECT` is all of the
work.

### Why the marking cannot simply be relaxed

Two independent reasons, and only the first is about workers:

1. Sequence values come from a backend-local cache (`SeqTableData` in
   `sequence.c`). Nothing about it is shared, so a worker cannot take part.
2. Parallel mode is a blanket read-only regime. `nextval_internal()` enforces
   it with `PreventCommandIfParallelMode()`, which tests `IsInParallelMode()` —
   true in the **leader** too, for as long as a Gather is open. `PARALLEL
   RESTRICTED` would place the call above every Gather, which is what the
   planner needs, and it would still fail at run time.

### Why a temporary sequence is different

The regime exists to stop state the workers share with the leader from moving
underneath them. Advancing a temporary sequence moves nothing they can see:

* **No WAL.** `RelationNeedsWAL()` is false, so `nextval_internal()` skips both
  the `XLogInsert()` and the `GetTopTransactionId()` that guards it. The second
  matters on its own — `AssignTransactionId()` refuses to run in parallel mode,
  so a *permanent* sequence would fail there even if the first check were
  lifted. That is why this function never takes its own path for one.
* **No transaction id, no command id.** Sequences are non-transactional.
* **Local buffers**, which belong to one backend.
* **Unreachable from below a Gather.** The value is visible only through
  `nextval()`, `currval()` and `lastval()`, none of them parallel-safe, or by
  scanning the sequence relation — and a sequence never gets a partial path, so
  that scan is never parallel either.

### How it acts

`proparallel` is a catalog property read long before any hook could intervene,
and `prosupport` is too late: `SupportRequestSimplify` runs from
`eval_const_expressions()` inside `subquery_planner()`, below the hazard scan,
which says so itself — *"parallelModeOK can't change after this point"*.

The one place early enough is `planner_hook`, in two steps.

**The gate.** Is this an `INSERT` into a temporary table of this backend? Almost
every statement is rejected on `commandType` alone; the rest costs one
`list_nth()` and one syscache lookup. Only what survives is walked. This is a
deliberate narrowing — a plain `SELECT` calling `nextval()` on a temporary
sequence keeps losing its parallelism, as it does today — and it is what keeps
a tree walk off a workload that plans thousands of statements a minute.

**The walk.** Find `nextval()` calls whose argument is a `Const` naming a
sequence that is temporary and belongs to this backend — all known at plan
time, because the rewriter plants the sequence OID as a constant when it
expands a column default — and point them at `seqguard_nextval()`.
`standard_planner()` then sees a restricted hazard instead of an unsafe one,
allows parallel paths, and keeps the call above every Gather.

The rewrite goes into the caller's `Query` rather than a copy: `copyObject()`
on a 1C `INSERT ... SELECT` tree, once per planning cycle, to change one `Oid`
per call site, is not a good trade. It is safe because it is idempotent — the
next planning cycle finds our function instead of `nextval()` and does nothing
— and because the fact that decided it, the target being a temporary table of
this session, cannot change underneath a cached plan.

At run time `seqguard_nextval()` delegates to `nextval_internal()` whenever it
is not in parallel mode — same cache, same values, same `currval()`. Only with
a Gather open does it take its own path, and there it re-checks
`rd_islocaltemp` before touching anything.

### GUCs

| GUC | Default | Meaning |
| --- | --- | --- |
| `seqguard.mode` | `on` | `off` leaves planning alone; `log` reports the calls that would be substituted; `on` substitutes. |
| `seqguard.log_level` | `debug1` | Level at which substituted calls are reported. |

### Caveats

* Requires `CREATE EXTENSION enforce_workers` in the database. Without it the
  module says so once per statement with a candidate call (at
  `seqguard.log_level`) and plans normally.
* **Only `INSERT` into a temporary table is considered.** A `SELECT`, an
  `UPDATE`, or an `INSERT` into a permanent table never reaches the walk — see
  the gate above.
* The private path does not cache unissued values, so it takes one value per
  call, as `CACHE 1` does. Two consequences, and only for a session that runs a
  parallel query over a temporary sequence:
  * `currval()` and `lastval()` do not see the values it produced;
  * if the core had already cached a block for the same sequence, values handed
    out afterwards are not ascending. They are still **unique** — the core owns
    its block exclusively and this path takes values beyond the end of it — and
    sequences promise no ordering across cached blocks anyway. A sequence with
    `CACHE 1`, which is the default and what a `serial` column gets, has no
    block to diverge from.
* Writing the page sets `log_cnt = 0`, which is honest for a relation that
  writes no WAL but makes the core's next `nextval()` on that sequence take its
  "must log" branch and fetch `SEQ_LOG_VALS` = 32 values ahead. Alternating
  between the two paths therefore burns 32 values per switch.
* A permanent sequence is never substituted, for the `GetTopTransactionId()`
  reason above. Another session's temporary sequence is never substituted
  either; `rd_islocaltemp` / `isTempNamespace()` is what tells the two apart.
* A computed `nextval()` argument is left alone — it may name a different
  sequence on every row, so there is no plan-time answer.
* The rewrite is in place, so `seqguard.mode = off` does not un-rewrite a
  statement that was already planned and cached, and a cached statement
  rewritten before `DROP EXTENSION` refers to a function that is gone.
  Re-preparing, or reconnecting, answers both.
* **A plain `INSERT` does not become parallel just because this ran.**
  `standard_planner()` also requires `parse->commandType == CMD_SELECT`; the
  patch that lifted that (`05c8482f7f`) was reverted two weeks later by
  `26acb54a13` and never shipped. On community PostgreSQL the substitution is
  therefore visible in `EXPLAIN` and changes no plan. This module is aimed at a
  build that does allow a parallel `SELECT` underneath an `INSERT`.

### Tests

`sql/seqguard.sql` covers the planner and the run time separately, because on
community PostgreSQL no `INSERT` will open a Gather for us.

Planner:

* `INSERT` into a temporary table with a `serial` column — substituted, and
  visible as `seqguard_nextval(...)` in `EXPLAIN VERBOSE`;
* `INSERT` into a permanent table — rejected by the gate;
* `INSERT` into a temporary table whose default uses a *permanent* sequence —
  passes the gate, rejected per call;
* a plain `SELECT` calling `nextval()` — rejected by the gate, which is the
  documented narrowing;
* `log` mode reporting and changing nothing.

Run time, reached by calling the function directly in a `SELECT` that does go
parallel:

* 20 000 values — one per row, no duplicates, no gaps;
* the guard: a permanent sequence inside parallel mode is handed back to the
  core, which raises `cannot execute nextval() during a parallel operation`;
* outside parallel mode, plain `nextval()` behaviour including `currval()`;
* a `serial` column on a temporary table behaving as before;
* the documented `currval()` limitation, and the sequence having advanced
  anyway;
* the feature going inert, not broken, after `DROP EXTENSION`.

## Build

In-tree:

    cd contrib/enforce-workers && make && make install

Against an installed server:

    make USE_PGXS=1 && make USE_PGXS=1 install

With meson, add `subdir('enforce-workers')` to `contrib/meson.build` first.

## Use

Load the library:

    LOAD 'enforce_workers';

or put `enforce_workers` in `session_preload_libraries` or
`shared_preload_libraries`. All three overrides take effect immediately.

`seqguard` needs one more step, per database, because it points query trees at
a function that has to exist:

    CREATE EXTENSION enforce_workers;

To load the library for one override only:

    LOAD 'enforce_workers';
    SET nlguard.mode = off;          -- and leave seqguard.mode alone, or
    SET seqguard.mode = off;         -- parallelism override only

`enforce_workers` has no equivalent switch; unload the library to be rid of
it.

## Caveats (enforce_workers)

* Making a partial path *available* is not the same as making it *win*.
  `parallel_setup_cost` (1000 by default) will sink the parallel plan on a small
  table almost every time. For experiments set `parallel_setup_cost = 0` and
  `parallel_tuple_cost = 0` alongside.
* `compute_parallel_worker()` clamps to `max_parallel_workers_per_gather`, so
  that GUC remains the real ceiling for scan paths.
* `max_parallel_workers` and `max_worker_processes` still cap what gets launched
  at run time — expect `Workers Planned: 4, Workers Launched: 1`.
* The query must be parallel-safe to begin with; this module does not change
  `consider_parallel`.
