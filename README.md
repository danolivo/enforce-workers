# enforce_workers

Removes the relation-size gate on parallelism. The module installs
`get_relation_info_hook` and sets `RelOptInfo->rel_parallel_workers` to
`max_parallel_workers_per_gather` for every base relation that does not already
carry an explicit `parallel_workers` storage parameter.

That is the same field the storage parameter writes, so the effect is identical
to running `ALTER TABLE ... SET (parallel_workers = N)` on everything — without
touching a single catalog row.

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
