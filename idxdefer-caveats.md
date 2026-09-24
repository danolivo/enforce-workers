# idxdefer: caveats

This is the full list of what `idxdefer` changes, what it costs, what it
depends on and where it has not been tested. The [README](README.md#idxdefer)
explains the mechanism; this document assumes you have read it.

In short: for a qualifying `INSERT ... SELECT` into an empty temporary table,
the executor is given no indexes, the rows go into the heap only, and the
indexes are rebuilt with `reindex_index()` — normally the moment the
`ModifyTable` node has inserted its last row. Everything below follows from one
of those three steps.

The target is Tantor SE 1C 18, where the planner runs `INSERT ... SELECT` in
parallel. Community PostgreSQL 18 is where the regression suite is developed,
and it passes there too, but it never exercises the parallel-mode path.

Contents:

1. [When it acts, and when it silently does not](#1-when-it-acts-and-when-it-silently-does-not)
2. [Hook order and load order](#2-hook-order-and-load-order)
3. [Observability](#3-observability)
4. [Behaviour that changes](#4-behaviour-that-changes)
5. [Costs](#5-costs)
6. [Transactions and errors](#6-transactions-and-errors)
7. [Versions, forks and untested ground](#7-versions-forks-and-untested-ground)
8. [Checklist before enabling it on the 1C stand](#8-checklist-before-enabling-it-on-the-1c-stand)

---

## 1. When it acts, and when it silently does not

An `INSERT` is deferred only when every condition in the README holds. When one
does not, the statement runs exactly as it would without the module, and
nothing is reported unless the refusal is about hook order (see
[§2](#2-hook-order-and-load-order)). The cases that are easy to miss:

* **The table must have no pages, not merely no rows.** A table that held rows
  from an `INSERT` that was rolled back, or from which everything was
  `DELETE`d, still has pages, and its next `INSERT` maintains the indexes row by
  row. Only `TRUNCATE` (or a new table) makes it eligible again.
* **Any trigger disqualifies the table**, including the internal triggers of a
  foreign key — on either side of it — and statement-level triggers.
* **Any unique, primary-key or exclusion index disqualifies the whole table**,
  not just that index: one index that must be maintained row by row means the
  executor has to open them all.
* **An index that is already open disqualifies the table.** A cursor in the same
  transaction that scans the target through an index is enough.
* **`RETURNING`, `ON CONFLICT` (any form) and data-modifying CTEs are never
  deferred.** A plain CTE in the source query is fine.
* **Inserting through a partitioned parent is never deferred.** Only a plain
  table qualifies; an `INSERT` directly into a temporary partition does.
* **The decision uses the planner's estimate**, `plan_rows` of the source plan,
  against `idxdefer.min_rows`. An underestimate loses the benefit; an
  overestimate costs a rebuild of an index that was cheap to maintain anyway.
  Underestimates are not rare: on Tantor SE, `WHERE a % 2 = 0` over two million
  rows was estimated at 10 000 and produced a million. A prepared `INSERT` with
  a generic plan is judged by the generic estimate — for
  `generate_series(1, $1)`, that is 1000 rows whatever `$1` is.
* **`EXPLAIN` without `ANALYZE` defers nothing**, because it runs nothing.
  `EXPLAIN ANALYZE` defers, and rebuilds.
* **Parallel workers never defer**, and a statement whose plan runs in parallel
  mode is deferred only under the load-order condition in
  [§2](#2-hook-order-and-load-order).

To see which statements are deferred, set `idxdefer.mode = log` (report, change
nothing) or keep `on` and raise `idxdefer.log_level` to `log`. Each deferral
logs the number of indexes and the planner's estimate in its `DETAIL`, and each
rebuilt index logs where it was rebuilt, how long it took, and how much memory
it was given.

## 2. Hook order and load order

### The guarantee, and why it does not depend on load order

The rows are in the heap and not in the index between the last row of the
`INSERT` and the end of the rebuild. The rebuild must therefore come before
anything that could read the table. `idxdefer` achieves that without relying on
its place in any hook chain: it wraps the `ExecProcNode` of the statement's
`ModifyTable` node with `ExecSetExecProcNode()`, and rebuilds inside that node,
the moment it reports that it is done. Control has not yet returned to
`ExecutePlan()`, let alone to `standard_ExecutorRun()` or to any library's
`ExecutorRun`, `ExecutorFinish` or `ExecutorEnd` hook.

So on a serial plan **the position of `enforce_workers` in
`shared_preload_libraries` does not matter**, and no other library can run code
in the window, whichever order they are loaded in.

### The exception that is the rule on Tantor: plans that run in parallel mode

A catalog update is refused while the backend is in parallel mode, and
`ExecutePlan()` leaves parallel mode only after the node is done. Tantor SE,
with `enable_parallel_insert = on` (the default on the stand), plans an
`INSERT ... SELECT` from a permanent table as

```
Insert on t
  ->  Gather
        ->  Parallel Seq Scan on src
```

with `parallelModeNeeded` set. For such a plan the earliest possible rebuild
point is right after `standard_ExecutorRun()` returns — in `idxdefer`'s
`ExecutorRun` hook.

That point is only safe if `idxdefer`'s hook calls `standard_ExecutorRun()`
directly, that is, if it is the **innermost** `ExecutorRun_hook`. Hooks chain
last-loaded-outermost, so this is fixed at load time: `idxdefer` is innermost
exactly when `ExecutorRun_hook` was `NULL` when `enforce_workers` was loaded,
and no library loaded later can get between it and the standard function.

**The code checks this**, in two places. At load, if another library's hook is
already there, the server log says once:

```
LOG:  idxdefer: inserts whose plans run in parallel mode will keep maintaining their indexes
DETAIL:  Another library installed an ExecutorRun hook before enforce_workers was loaded.
HINT:  List enforce_workers before every other library in shared_preload_libraries.
```

and each parallel-mode insert it leaves alone is reported at
`idxdefer.log_level`:

```
LOG:  idxdefer: not deferring index maintenance on "t"
DETAIL:  The plan runs in parallel mode, and another library's ExecutorRun hook would run between the insert and the rebuild.
HINT:  List enforce_workers before every other library in shared_preload_libraries.
```

The failure mode of a wrong load order is therefore a lost optimisation, never
an index that is read while incomplete. Both orders were checked on a scratch
Tantor SE 1C 18.4 cluster: with `'auto_explain,enforce_workers'` the two
messages above appear and the parallel insert maintains its index row by row;
with `'enforce_workers,auto_explain'` it is deferred, and the index is rebuilt
"after the parallel plan finished" and passes `bt_index_check(..., true)`.

### Recommended order

```
shared_preload_libraries = 'enforce_workers, <everything else>'
```

On the 1C stand, `enforce_workers` is currently not loaded at all (the
baseline run); `shared_preload_libraries` is `'auto_explain'`. When it comes
back, it must come back first: `'enforce_workers,auto_explain'`. Its previous
position, `'auto_explain,enforce_workers'`, would cost every parallel-mode
insert its deferral. Nothing else about `auto_explain` changes (see
[§3](#3-observability)).

Loading the module with `LOAD` or `session_preload_libraries` after another
library with executor hooks has the same effect as a late position in
`shared_preload_libraries`; the load-time message is then a `WARNING` to the
session.

### get_relation_info_hook

While an `INSERT` is deferred, `idxdefer` empties `rel->indexlist` for its target
after calling the previous hook, so indexes that libraries loaded **before**
`enforce_workers` add are hidden too. A library loaded **after** it runs
outermost, and whatever it adds to `indexlist` after calling `idxdefer` is not
hidden. The known cases are harmless:

* **hypopg** adds hypothetical indexes, and only for `EXPLAIN` without
  `ANALYZE`, which reads no data.
* **pg_hint_plan** only removes indexes from the list; it cannot bring back one
  that `idxdefer` removed, and an `IndexScan` hint for a hidden index is simply
  not applied.

Loading `enforce_workers` first covers both hooks with one rule.

### Other libraries on the stand, and those that might return

* **online_analyze** (currently not loaded; relevant if it comes back). It runs
  `ANALYZE` on a temporary table after an `INSERT`, from its `ExecutorEnd` hook.
  Every `ExecutorEnd` hook runs after the rebuild — inside the node on a serial
  plan, in the innermost `ExecutorRun` hook on a parallel one — so the index is
  complete by then in any load order, and no ordering rule is needed for it.
  `ANALYZE` does not read indexes anyway, only heap samples and index
  expressions, and the rebuild overwrites the index's `relpages` and `reltuples`
  in `pg_class`. The statement count `online_analyze` uses (`es_processed`) is
  unaffected. This is based on the way `online_analyze` is written, not on a
  test: it is not part of the build on the stand.
* **auto_explain**, **pg_stat_statements**, **pg_stat_kcache**,
  **pg_wait_sampling** and similar only account for time and resources; none of
  them reads the target table. See [§3](#3-observability) for what they see.
* **A library that replaces a node's `ExecProcNode`** after `idxdefer` has
  wrapped it would normally save and call `idxdefer`'s wrapper, so nothing
  changes. If it called `ExecModifyTable()` directly instead, the wrapper would
  be bypassed and the rebuild would move to the `ExecutorRun` hook (or later,
  see below), with a `WARNING` saying it happened later than expected. No
  library on the stand does this.
* **A library that runs the plan without `standard_ExecutorRun()`**, or calls
  `ExecutorEnd` without `ExecutorRun`, is covered by the safety nets: the
  `ExecutorFinish` and `ExecutorEnd` hooks rebuild whatever is still pending
  before any processing of their own, and say so with a `WARNING`. If the node
  never ran, the table is still empty and there is nothing to rebuild. On these
  paths other libraries' outer `ExecutorFinish`/`ExecutorEnd` code could run
  before the rebuild; that is acceptable only because none of them reads the
  table. These paths are not expected to be reached at all.

## 3. Observability

Where the rebuild time is reported, by path:

| Rebuild point | EXPLAIN ANALYZE `Insert` node | EXPLAIN ANALYZE `Execution Time` | auto_explain `duration`, pg_stat_statements `total_exec_time` |
| --- | --- | --- | --- |
| Inside `ModifyTable` (serial plans) | yes | yes | yes |
| `ExecutorRun` hook (parallel-mode plans) | no | yes | yes, charged to `queryDesc->totaltime` by hand |
| `ExecutorFinish` hook (safety net) | no | yes | yes |
| `ExecutorEnd` hook (safety net) | no | yes | only if the other library's hook is inner to `idxdefer`'s |

On the serial path the rebuild is simply part of the `Insert` node, just as the
row-by-row maintenance it replaces was. Checked with `auto_explain`
(`log_analyze`) in both load orders on a 1.5-million-row insert: the logged
duration, the `Insert` node's actual time, and the log timestamps all include
the 860 ms rebuild. On the parallel path, on Tantor, `Execution Time` of a
million-row insert was 793 ms, the 446 ms rebuild included, while the `Insert`
node's own time does not contain it.

Other points:

* **Buffer and I/O counters move with the time.** `totaltime` is allocated
  with full instrumentation, so the rebuild's local-buffer reads and its sort's
  temporary files appear in `pg_stat_statements` and in `EXPLAIN (ANALYZE,
  BUFFERS)` on the same paths as the time.
* **New temporary files.** The rebuild sorts the whole table. If the sort does
  not fit in the memory it is given (see [§5](#5-costs)), it spills, which
  `log_temp_files` reports and `pg_stat_database.temp_bytes` counts. The
  row-by-row path never did.
* **Messages.** At `idxdefer.log_level`, one message per deferral, with the
  index count and the planner's estimate, and one per rebuilt index, whose
  `DETAIL` says where it was rebuilt, how long it took, what
  `maintenance_work_mem` it got, the estimated sort size and the limit. The
  default level is `debug1`, so none of that reaches the log unless it is
  raised. Two things are reported regardless: the load-order problem, once at
  startup as `LOG`, and a rebuild later than expected, as `WARNING`.
* **Row counts are unaffected**: the command tag, `ROW_COUNT` in PL/pgSQL and
  the `rows` column of `pg_stat_statements` report what they always did.
* **`log_min_duration_statement`** measures the whole statement, rebuild
  included, on every path.

## 4. Behaviour that changes

* **Errors in index expressions are raised later.** Row by row, a failing
  index expression stops the `INSERT` on the offending row. With the deferral,
  every row is inserted first, and the error comes from the rebuild, with a
  `CONTEXT` line naming the index. The statement fails either way and nothing
  is left behind, but the time spent inserting is wasted and the error looks
  different to anything parsing it.
* **Index expressions run in a maintenance context.** `reindex_index()` runs
  them as the table owner, under `SECURITY_RESTRICTED_OPERATION`, with
  `search_path` set to `pg_catalog, pg_temp` — as `CREATE INDEX` and `REINDEX`
  do. Row by row, they run as the current user with the session's
  `search_path`. An expression calling a function whose body depends on
  `search_path`, or that does something forbidden in a security-restricted
  operation, can work row by row and fail in the rebuild. 1C does not create
  such indexes; `CREATE INDEX` on an empty table would not have caught it either,
  because it evaluates nothing.
* **Table statistics are filled in.** The rebuild updates `relpages` and
  `reltuples` of the index and — when autovacuum is on and not disabled for the
  table — of the table itself, as `CREATE INDEX` on a filled table would. Row by
  row, a never-analysed temporary table keeps `reltuples = -1`, and the planner
  estimates its size from the number of pages and the tuple width. After a
  deferred insert it uses the real count. **Later queries on the same table can
  therefore get different plans**, usually better estimated ones, but different.
  This is the one change that can move timings outside the statement itself,
  and it should be kept in mind when comparing runs.
* **Command ids.** Each deferral uses one command counter increment at the start
  and one after each rebuilt index. The limit is 2³² − 2 commands per
  transaction, so this only matters in theory.
* **Plans are replanned.** Every cached plan that depends on the target table is
  invalidated when the deferral starts, and again by the rebuild. That includes
  the prepared `INSERT` itself and every other prepared statement in the
  session that reads the table. Their next execution plans afresh.
* **The index gets a new relfilenode.** Anything that remembers the file of an
  index — `pg_relation_filenode()` results, a pre-warm list — goes stale.
* **Readers that bypass the planner see the index incomplete while the
  `INSERT` runs.** The protection works by hiding the indexes from queries
  planned during the statement, the rebuild itself included. A `VOLATILE`
  function that opens the index directly — `amcheck`, `pageinspect`,
  `pgstatindex()`, C code — sees it missing the rows inserted so far. After the
  statement, everything is consistent. DDL on the target during the insert
  (`DROP INDEX`, `CREATE INDEX`, `TRUNCATE`, `ALTER TABLE`) is refused by
  PostgreSQL itself, because the table is in use by the `INSERT`.
* **Writers do not need the planner.** A `VOLATILE` function that updates or
  deletes rows of the target during the insert opens the indexes for its own
  statement and maintains them, stale as they are; the rebuild replaces them
  anyway. This is tested, HOT updates included.
* **Cursors.** A cursor opened before the `INSERT` through an index on the
  target stops the deferral (the index is open). A cursor opened during it by a
  `VOLATILE` function is planned without the indexes, so it uses a sequential
  scan for as long as it lives, even after the rebuild.

## 5. Costs

* **A rebuild is a full index build.** It sorts the whole table, which is what
  makes it cheaper than random insertions into an index larger than
  `temp_buffers`, but it is still work proportional to the table. The
  `min_rows` threshold exists for this. A table with N indexes is scanned N
  times, once per `reindex_index()`.
* **Parallel index build.** Community PostgreSQL never uses parallel workers to
  build an index on a temporary table (`plan_create_index_workers()` returns 0
  for them). Whether Tantor SE does, with `max_parallel_maintenance_workers = 2`
  on the stand, has not been checked. If it does, note that a parallel build
  needs 32MB of `maintenance_work_mem` per participant, so the estimate below
  also caps the number of workers of a small rebuild — which is what a small
  sort wants anyway.
* **Memory.** Each B-tree rebuild gets `maintenance_work_mem` set to the sort
  size estimated from the inserted rows, or to `idxdefer.maintenance_work_mem`
  (1GB by default) if the estimate is larger; other index types get
  `idxdefer.maintenance_work_mem`. The session's own `maintenance_work_mem` —
  64MB on the stand — plays no part. This is a ceiling, not an allocation: a
  tuplesort takes memory as tuples arrive, so the real use is what the data
  needs, up to the ceiling. The estimate errs high by design, 1.2 to 2 times
  the measured use (README), because erring low would send the sort to disk.
  With many sessions rebuilding at once, `idxdefer.maintenance_work_mem` is the
  per-backend bound to size against the machine's memory.
* **`temp_file_limit`.** A sort that spills counts against it, so a large
  rebuild can fail with "temporary file size exceeds temp_file_limit" where row
  by row it would not have written a temporary file at all. It is `-1` on the
  stand.
* **Catalog churn.** Each rebuilt index gets a new relfilenode through a
  regular update of its `pg_class` row, which leaves a dead tuple in
  `pg_class`; the statistics updates are in place. The old index file is
  unlinked at commit. On a workload that already creates and drops temporary
  tables by the thousand this is a small addition, but it is not zero. The
  in-place alternative that would avoid it is not safe; the comment on
  `idxdefer_rebuild()` explains why.
* **Invalidation traffic.** The invalidation at the start is processed
  immediately in this backend, and its relcache callbacks walk the session's
  plan cache. At commit, these messages and the rebuild's own go to the shared
  invalidation queue, and every backend connected to the same database runs the
  same callbacks, including a walk of its plan cache, even though no other
  backend can see the table. `CREATE TEMPORARY TABLE`, `CREATE INDEX` and
  `DROP TABLE`, which 1C runs far more often, send the same kinds of messages.
  Not measured under 1C concurrency.
* **Locks.** The rebuild takes `ShareLock` on the table and
  `AccessExclusiveLock` on each index. With `wal_level` at `replica` or above,
  PostgreSQL WAL-logs every `AccessExclusiveLock` on a relation — temporary ones
  included — and standbys replay it, holding an entry in their lock table until
  the transaction ends. 1C's own `CREATE INDEX` on the empty table does the same.
* **WAL.** The index data is not WAL-logged (the relation is temporary), but the
  `pg_class` updates and the lock record are. Row by row, the insert writes no
  WAL for the table at all.

## 6. Transactions and errors

* **The `INSERT` fails**: nothing is rebuilt. The rows it wrote are dead, and an
  index without entries for dead tuples is a valid index.
* **The rebuild fails**, a query cancel and `statement_timeout` included: the
  transaction (or subtransaction) aborts, the new relfilenode is discarded, and
  the old index is consistent with the heap, because the rows it lacks are the
  ones the abort killed.
* **Savepoints and PL/pgSQL exception blocks**: a deferral registered in a
  subtransaction that aborts is forgotten with it, together with anything nested
  deeper. Rolling back to a savepoint after the rebuild restores the old index
  and kills the rows, which is again consistent.
* **Isolation levels**: tested under read committed, repeatable read and
  serializable.
* **Commit with a rebuild still pending** is refused with an internal error.
  This cannot happen on any known path; if it ever does, it is a bug, and the
  refusal keeps the table from leaving the transaction with an incomplete index.
* **Autonomous transactions** would break the bookkeeping, which assumes one
  transaction at a time per backend. Tantor SE 1C has none.
* **`PREPARE TRANSACTION`** is impossible for a transaction that touched a
  temporary table, so two-phase commit never meets a deferral.
* **`ON COMMIT DELETE ROWS` / `ON COMMIT DROP`** are unaffected; the rebuild
  finishes long before commit.

## 7. Versions, forks and untested ground

* **It depends on an executor detail.** The whole mechanism rests on
  `ExecInsert()` opening the indexes lazily, on the first row, and only when
  `ri_IndexRelationDescs` is `NULL` (`nodeModifyTable.c` in PostgreSQL 18). If a
  version or a fork opens the indexes eagerly in `ExecInitModifyTable()`,
  `idxdefer` sees them already open and does nothing. If a fork changes the
  test in some other way, the regression suite — which checks every rebuilt
  index with `amcheck`, `heapallindexed` included — is what will tell.
* **Tantor SE 1C 18.4** (built with `-DTANTOR_SE1C`): the module builds against
  its headers without warnings and the regression suite passes against its
  binaries, on a scratch cluster with the stand's parallel settings, in both
  load orders. Its sources are not available to us, so this is the whole of
  the evidence; run the suite again on every new Tantor build.
* **Tantor adds transaction events** (`XACT_EVENT_PRE_ABORT` and its parallel
  twin) that community PostgreSQL does not have; the transaction callback
  ignores them.
* **Tantor-specific temporary-table features**: `enable_delayed_temp_file` is on
  on the stand and in the test cluster, and works. `enable_temp_memory_catalog`
  is off on the stand and has not been tried.
* **The emptiness test** uses `RelationGetNumberOfBlocks()` on the table, both
  at `ExecutorStart` and before the rebuild. The review remarks about it are
  deliberately left as they are.
* **Index types.** B-tree is tested throughout and GIN once. GiST, BRIN and hash
  indexes go through the same `reindex_index()` and are expected to work, but
  no test covers them. The TOAST table's index is not affected: it belongs to
  the TOAST table, not to the target, and is maintained as usual.
* **Only the heap table access method is tested.**
* **GUCs.** `idxdefer.*` settings made before the library is loaded become
  placeholders and take effect on load; after loading, the prefix is reserved,
  and a misspelt `idxdefer.*` name is an error.

## 8. Checklist before enabling it on the 1C stand

1. Build against the stand's `pg_config` and run `make installcheck
   REGRESS=idxdefer` against a scratch cluster of the same binaries (see
   [§7](#7-versions-forks-and-untested-ground)).
2. Put `enforce_workers` first in `shared_preload_libraries`:
   `'enforce_workers,auto_explain'`. Check the server log at startup: the
   load-order `LOG` line must not be there.
3. Size `idxdefer.maintenance_work_mem` against memory and the number of
   sessions that may rebuild at once; check `temp_file_limit`.
4. For the first run, set `idxdefer.log_level = log`, and look in the log for:
   the deferrals and their estimates; the rebuild times, points and memory;
   any "not deferring … parallel mode" refusal; any `WARNING` about a late
   rebuild.
5. When comparing against a baseline, remember that later queries on the
   deferred tables may be planned with real row counts
   ([§4](#4-behaviour-that-changes)).
