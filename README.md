# enforce_workers

Two unrelated planner overrides in one loadable module: `enforce_workers`
below, and `nlguard` further down. Each lives in its own file and can be
switched off independently; `_PG_init()` is the only entry point.

**Both are active the moment the library is loaded.** `enforce_workers` has no
switch at all, and `nlguard.mode` defaults to `on`. Loading this library
changes plans — that is what it is for — so do not put it in
`shared_preload_libraries` of a server you have not measured it on.

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

`sql/nlguard.sql` covers: a plain nested loop being replaced; an index nested
loop surviving; both together in a four-way join; LIMIT and cursor; a
semijoin; a join with no hashable or mergeable clause, where planning must
still succeed; `enable_nestloop = off`; `log` mode changing nothing; and the
rewritten plan returning the same rows.

## Build

In-tree:

    cd contrib/enforce-workers && make && make install

Against an installed server:

    make USE_PGXS=1 && make USE_PGXS=1 install

With meson, add `subdir('enforce-workers')` to `contrib/meson.build` first.

## Use

There are no SQL objects, so no `CREATE EXTENSION`:

    LOAD 'enforce_workers';

or put `enforce_workers` in `session_preload_libraries` or
`shared_preload_libraries`.

Both overrides take effect immediately. To load the library for one of them
only:

    LOAD 'enforce_workers';
    SET nlguard.mode = off;          -- parallelism override only

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
