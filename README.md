# enforce_workers

Three unrelated planner overrides and one executor override in one loadable
module: `enforce_workers` below, then `nlguard`, `seqguard` and `idxdefer`.
Each lives in its own file and can be switched off independently; `_PG_init()`
is the only entry point.

**All four are active the moment the library is loaded.** `enforce_workers`
has no switch at all, and `nlguard.mode`, `seqguard.mode` and `idxdefer.mode`
all default to `on`. Loading this library changes plans — that is what it is
for — so do not put it in `shared_preload_libraries` of a server you have not
measured it on.

How much it changes, measured: with the library preloaded, PostgreSQL's own
regression suite fails **24 of 231** tests. `enforce_workers` alone accounts for
8 of them, `nlguard` for the other 16, and `seqguard` for none — it fired on 20
statements and changed no output, which is what the `CMD_SELECT` restriction in
`standard_planner()` predicts. Every one of the 24 is a change of plan shape or
of row order in a query without `ORDER BY`; in `numeric`, `point` and `geometry`
the rows are the same multiset in a different order, and nowhere does a value, a
row count, or an error message differ. To repeat it, note that `pg_regress`
builds its database from `template0`, so the extension has to come in through
`EXTRA_REGRESS_OPTS='--load-extension=enforce_workers'` rather than by being
installed in `template1`. `idxdefer` came later and has not been through that
measurement.

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
per call site, is not a good trade. It is also unnecessary — scribbling on the
`Query` is what a planner is expected to do, and the one caller that plans the
same tree twice protects itself, in `BuildCachedPlan()`:

> If we don't already have a copy of the querytree list that can be scribbled
> on by the planner, make one. For a one-shot plan, we assume it's okay to
> scribble on the original `query_list`.

So a prepared statement hands us a fresh copy every planning cycle, and a
simple query hands us a tree built for that execution alone. Nothing written
here outlives the plan it was written for.

At run time `seqguard_nextval()` delegates to `nextval_internal()` whenever it
is not in parallel mode — same cache, same values, same `currval()`. Only with
a Gather open does it take its own path, and there it re-checks
`rd_islocaltemp` before touching anything.

That private path keeps its own per-sequence table, the counterpart of
`SeqTableData` in `sequence.c`, holding two things: whether the relation lock
has already been taken in this transaction, and the parameters from
`pg_sequence`. Without it every call would go through `LockRelationOid()`,
`relation_open()` and a syscache lookup — once per row, in the leader, above a
Gather, which is the one place in the plan where serial work hurts most. The
lock is charged to `TopTransactionResourceOwner`, as the core charges it and
for the same reason. What is **not** cached is unissued values: that would open
gaps, widen the `currval()` divergence below, and buy nothing where the
sequence behind a `serial` column has `CACHE 1` anyway.

The cached parameters are invalidated by a counter rather than by walking the
table. The `pg_sequence` callback fires on every sequence invalidation in the
database, and `CREATE TEMP TABLE` with a `serial` column is one of those — so on
this workload it fires as often as the table grows, which would be quadratic in
the operation the workload does most.

### GUCs

| GUC | Default | Meaning |
| --- | --- | --- |
| `seqguard.mode` | `on` | `off` leaves planning alone; `log` reports the calls that would be substituted; `on` substitutes. |
| `seqguard.log_level` | `debug1` | Level at which substituted calls are reported. |

### Caveats

* Requires `CREATE EXTENSION enforce_workers` in the database. Without it the
  module says so once per statement with a candidate call (at
  `seqguard.log_level`) and plans normally. Creating or dropping the extension
  takes effect in sessions that are already open, and the replacement is found
  in the schema the extension was created in rather than through `search_path`
  — including after `ALTER EXTENSION ... SET SCHEMA`.
* **Only `INSERT` into a temporary table is considered**, and not one with `ON
  CONFLICT`. A `SELECT`, an `UPDATE`, or an `INSERT` into a permanent table
  never reaches the walk — see the gate above.
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
* The run-time guard is a real test, not an `Assert`. `rd_islocaltemp` is true
  in a parallel worker for the leader's temporary relations — `relcache.c` takes
  `rd_backend` from `ProcNumberForTempRelations()`, which is the *leader's* proc
  number there — so a worker that somehow reached the private path would advance
  the leader's sequence in its own local buffer pool, silently. `PARALLEL
  RESTRICTED` is a plan-time promise and a `PARALLEL SAFE` wrapper breaks it
  without any malice, so the function checks at run time too.
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
* `INSERT ... ON CONFLICT`, both forms — rejected by the gate;
* the same prepared statement planned three times under `force_custom_plan`,
  which reports three times and so shows the in-place rewrite never reaching
  the cached tree;
* `log` mode reporting and changing nothing.

Run time, reached by calling the function directly in a `SELECT` that does go
parallel:

* 20 000 values — one per row, no duplicates, no gaps;
* the guard, both halves: a permanent sequence inside parallel mode, and the
  function reaching a worker through a `PARALLEL SAFE` plpgsql wrapper with
  `parallel_leader_participation = off`. Both are handed back to the core,
  which raises `cannot execute nextval() during a parallel operation`;
* outside parallel mode, plain `nextval()` behaviour including `currval()`;
* cached parameters following `ALTER SEQUENCE ... INCREMENT BY`;
* a `serial` column on a temporary table behaving as before;
* the documented `currval()` limitation, and the sequence having advanced
  anyway;
* the feature going inert after `DROP EXTENSION` and coming back after
  `CREATE EXTENSION`, in the same session both times;
* the replacement found in a schema that is not in `search_path`, and after
  `ALTER EXTENSION ... SET SCHEMA`.

## idxdefer

Builds the indexes of a temporary table once, after a bulk
`INSERT ... SELECT`, instead of maintaining them row by row.

1C creates an index on every temporary table straight after `CREATE TEMPORARY
TABLE`, then fills the table with one large `INSERT ... SELECT` whose rows come
in effectively random order for the index key. Once the index outgrows
`temp_buffers`, nearly every insertion lands on an evicted leaf: the dirty
victim is written out, the leaf read back. On the 1C stand one such statement
inserts 13 million rows into a table that ends up about 1.5 GB with its index,
and writes 29 GB of local buffers doing it; a synthetic reproduction takes
84.7 s as 1C does it and 5.3 + 14.3 s with the index built afterwards. Across a
package run, the `Insert` nodes of indexed temporary-table inserts are about
half of the time the database spends.

### How it acts

`ExecutorStart_hook` runs after the standard processing, when the result
relation is set up but its indexes are not yet open — PG18's `ExecInsert()`
opens them lazily on the first row, and only if `ri_IndexRelationDescs` is
still `NULL`. For an `INSERT` that qualifies, the hook puts an empty array
there with `ri_NumIndices = 0`, so the rows go into the heap only, and wraps
the `ModifyTable` node's `ExecProcNode` with `ExecSetExecProcNode()`. The
moment the node reports that it is done, still inside it, the wrapper rebuilds
the real indexes with `reindex_index()` — a sort-based build, as `CREATE INDEX`
on a filled table would do. No hook of any library can run between the last
row and the rebuild, whatever the load order, and the rebuild counts as part of
the `Insert` node for `EXPLAIN ANALYZE`, `auto_explain` and
`pg_stat_statements`, just as row-by-row maintenance did.

A plan that runs in parallel mode cannot update catalogs until `ExecutePlan()`
leaves that mode. Tantor SE, with `enable_parallel_insert`, plans
`INSERT ... SELECT` into a temporary table as `Insert` over `Gather`, so on the
stand this is the common case, not a corner. For such a plan the rebuild
happens in the `ExecutorRun` hook, right after `standard_ExecutorRun()` returns.
That is only safe if the hook is the innermost one, so such plans are deferred
only when `enforce_workers` was loaded before any other library with an
`ExecutorRun` hook; otherwise the statement is left alone, the reason is
reported, and a `LOG` line at startup says so once. `ExecutorFinish` and
`ExecutorEnd` rebuild anything still pending, as a safety net that is not
expected to fire; if it ever does, it says so with a `WARNING`.

No catalog row is touched to switch maintenance off. Clearing
`pg_index.indisready` would be a transactional catalog update and a relcache
invalidation twice per statement, only visible to the backend itself after a
command counter increment. (`indisvalid` would not have helped: it keeps the
planner from reading an index, while `ExecInsertIndexTuples()` tests
`ii_ReadyForInserts`, which comes from `indisready`.)

An `INSERT` qualifies when all of these hold:

* it is a plain `INSERT` with one target, no `RETURNING`, no `ON CONFLICT` and
  no data-modifying CTE, and not under `EXPLAIN` without `ANALYZE`;
* the target is an ordinary temporary table of this backend, not a parallel
  worker's view of the leader's;
* the table has no triggers, and every index is valid, ready, live, neither
  unique nor exclusion, and not currently open — by a cursor, or by a scan in
  the statement itself;
* the table has no pages when the statement starts;
* the planner expects at least `idxdefer.min_rows` rows.

### Why it is safe

While the statement runs, the heap has rows the index does not know about.
Nothing that needs the index to decide what happens to a row is allowed, so
that state can only be observed by reading the table, and it can only outlive
the statement on an error.

* **The statement itself.** Its own rows are invisible to its own snapshot and
  the table started empty, so any scan in it sees nothing, index or not.
* **Code it calls.** A `VOLATILE` function in the source query runs with a newer
  command id and does see the rows inserted so far — possibly through the
  index. So while an `INSERT` is deferred, `get_relation_info_hook` removes its
  target's indexes from any query planned in the meantime, and the target's
  relcache entry is invalidated and the invalidation processed at once, which
  makes every cached plan that depends on the table be replanned on its next
  use. The function reads the table with a sequential scan and sees exactly
  what it would have seen without the module. A `STABLE` or `IMMUTABLE` function
  uses the statement's own snapshot and cannot see those rows at all.
* **Other libraries.** On a serial plan none of their code runs between the
  last row and the rebuild; on a parallel-mode plan, see above.
* **Errors.** A failed `INSERT` rebuilds nothing; its rows are dead,
  and an index without entries for dead tuples is a valid index. A failed
  rebuild aborts the transaction, which discards the new relfilenode and keeps
  the old index — consistent with the heap, because the rows it lacks are the
  ones the abort killed. There is never anything to repair. Transaction and
  subtransaction callbacks forget registered deferrals, and a deferral still
  pending at commit is refused rather than let the table outlive it.

### GUCs

| GUC | Default | Meaning |
| --- | --- | --- |
| `idxdefer.mode` | `on` | `off` leaves inserts alone; `log` reports the inserts that would be deferred; `on` defers them. |
| `idxdefer.log_level` | `debug1` | Level at which deferrals and rebuilds are reported; the estimate is in the `DETAIL`. |
| `idxdefer.min_rows` | `1000000` | Planner estimate below which an insert keeps its index maintenance. |

The default for `min_rows` comes from the stand: in a baseline run of the
`cherkizovo` package, indexed temporary-table inserts of a million rows or more
are 98% of the index maintenance time, and those below a hundred thousand are
0.2%.

### Caveats

* Put `enforce_workers` first in `shared_preload_libraries`; otherwise inserts
  whose plans run in parallel mode are not deferred.
* A rebuild is a full index build, using up to `maintenance_work_mem` and, if
  that is not enough, temporary files.
* The rebuild fills in `reltuples` and `relpages`, so later queries on the same
  table may be planned differently.
* An error in an index expression is raised after all rows are in, from the
  rebuild, rather than on the offending row.
* The protection for readers works through the planner, so a C function that
  opens the target's index directly while the `INSERT` runs sees it
  incomplete until the rebuild.
* The mechanism depends on how PostgreSQL 18's `ExecInsert()` opens indexes.
  The regression suite passes against Tantor SE 1C 18.4, and the parallel-mode
  path was checked there by hand; run both again on any other build.

### Tests

`sql/idxdefer.sql` checks every rebuilt index with `amcheck`'s
`bt_index_check(..., heapallindexed => true)` where `amcheck` is available
(`make check` installs it), and reads each through an index scan against the
heap in any case. Covered:

* the deferral itself, on a plain, an expression and a partial index, with the
  relfilenode changing; after `TRUNCATE` again; an insert of no rows, which
  defers and rebuilds nothing; `EXPLAIN` without `ANALYZE`, which defers nothing,
  and with it, which does; a prepared insert with a cached generic plan,
  deferred on each execution into an empty table and left alone otherwise;
* what is left alone: an insert below `min_rows`, into a non-empty table, into a
  permanent table, with a unique index or a primary key (and the duplicate
  still caught on its row), with `ON CONFLICT DO NOTHING`, with `RETURNING`,
  inside a data-modifying CTE, on a table with a trigger, and with an index held
  open by a cursor; a plain CTE in the source is deferred;
* `log` and `off`;
* errors: a failing insert, a failing rebuild (an index expression dividing by
  zero), a rollback to a savepoint after the rebuild, and a failure inside a
  PL/pgSQL exception block followed by a normal commit;
* a `VOLATILE` function reading the target during the insert, called for the
  first time half way through, with its generic plan cached through the index
  beforehand — it sees every row inserted so far — under read committed,
  repeatable read and serializable;
* a `VOLATILE` function updating (HOT and not) and deleting rows of the target
  while the insert runs;
* a deferred insert nested in a function called by another query, and one
  nested inside a deferred insert into the same table;
* a GIN index.

Each of the three protections was checked by disabling it: without the rebuild,
`amcheck` reports heap tuples missing from the index and index scans return
nothing; without hiding the indexes from the planner, the `VOLATILE` function
sees none of the 2 500 rows it should; without processing the invalidation
at once, its first call still goes through the stale cached plan and misses one.

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
`shared_preload_libraries`. All four overrides take effect immediately.

`seqguard` needs one more step, per database, because it points query trees at
a function that has to exist:

    CREATE EXTENSION enforce_workers;

Two things to know before deploying this under an application rather than under
psql.

`LOAD` is superuser-only — a plain user gets *access to library
"enforce_workers" is not allowed* — and so is `CREATE EXTENSION`, since the
control file is not `trusted`. An application connecting as an ordinary role
therefore cannot load this itself: put the library in
`shared_preload_libraries` or `session_preload_libraries` and have a superuser
run `CREATE EXTENSION` once per database. `CREATE EXTENSION` takes effect in
sessions that are already open, so the order does not matter.

And if the library is *not* loaded, `SET seqguard.mode = off` still succeeds.
An unrecognised `prefix.name` setting becomes a placeholder, so the statement
reports success and changes nothing. `SHOW seqguard.mode` returning a value is
not evidence that the module is running; a substitution reported at
`seqguard.log_level`, or `seqguard_nextval` appearing in `EXPLAIN VERBOSE`, is.

To load the library for one override only:

    LOAD 'enforce_workers';
    SET nlguard.mode = off;          -- and leave seqguard.mode alone, or
    SET seqguard.mode = off;         -- parallelism override only
    SET idxdefer.mode = off;         -- no index maintenance deferral

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
