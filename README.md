# enforce_workers

Two unrelated planner overrides in one loadable module: `enforce_workers`
below, and `nlguard` further down. Each lives in its own file and is switched
on independently; `_PG_init()` is the only entry point.

## enforce_workers

Removes the relation-size gate on parallelism. The module installs
`get_relation_info_hook` and sets `RelOptInfo->rel_parallel_workers` to
`max_parallel_workers_per_gather` for every base relation that does not already
carry an explicit `parallel_workers` storage parameter.

That is the same field the storage parameter writes, so the effect is identical
to running `ALTER TABLE ... SET (parallel_workers = N)` on everything — without
touching a single catalog row.

## nlguard

Finds one shape of nested loop — the plain one: unparameterised, reading its
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

### GUCs

| name | type | default | meaning |
|---|---|---|---|
| `nlguard.mode` | enum | `off` | `off` / `log` |
| `nlguard.log_level` | enum | `debug1` | level for the report, as in `auto_explain` |

`log` mode evaluates the rule and reports every join it matches, without
changing a single plan. Raising `log_level` to `log` makes the logger process
part of the measurement, so on a busy system prefer `debug1` with
`log_min_messages` set for the duration of the run.

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

## Caveats

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
