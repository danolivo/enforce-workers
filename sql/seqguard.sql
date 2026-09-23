LOAD 'enforce_workers';

-- The planner half of seqguard needs a function in pg_proc to point calls at.
CREATE EXTENSION enforce_workers;

-- enforce_workers already offers partial paths for every relation; the rest of
-- these make a parallel plan the cheapest one for a small test table.
SET max_parallel_workers_per_gather = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;

CREATE TABLE sg_src (a int, b int);
INSERT INTO sg_src SELECT i, i % 100 FROM generate_series(1, 20000) i;
VACUUM ANALYZE sg_src;

-- The module is active from the moment the library is loaded.
SHOW seqguard.mode;

--
-- Part 1: the planner.
--
-- What the module acts on is decided in two steps - a gate on the statement,
-- then a test per call - and the cases below cover the four combinations that
-- matter.  The reports are what says which step rejected a statement, so run
-- this part with them visible.
--
CREATE TEMP TABLE sg_tmp_dst (id serial, a int);
CREATE TABLE sg_perm_dst (id serial, a int);
CREATE SEQUENCE sg_perm_seq;
CREATE TEMP TABLE sg_mixed_dst (id bigint DEFAULT nextval('sg_perm_seq'), a int);

SET seqguard.log_level = notice;
\set VERBOSITY terse

-- The case the module exists for: temporary target, temporary sequence.
SET seqguard.mode = off;

EXPLAIN (COSTS OFF)
INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src;

SET seqguard.mode = on;

EXPLAIN (COSTS OFF)
INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src;

-- The substitution is in the target list of the source query.  On community
-- PostgreSQL the plan around it does not change, because standard_planner()
-- requires commandType == CMD_SELECT before it will consider a Gather at all;
-- on a build that lifts that, this is what keeps the scan below parallel.
EXPLAIN (COSTS OFF, VERBOSE)
INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src WHERE a < 3;

-- A permanent target is rejected by the gate, before any tree is walked.
EXPLAIN (COSTS OFF)
INSERT INTO sg_perm_dst (a) SELECT a FROM sg_src;

-- A temporary target passes the gate, and then the permanent sequence in its
-- column default is rejected by the per-call test.
EXPLAIN (COSTS OFF)
INSERT INTO sg_mixed_dst (a) SELECT a FROM sg_src;

-- A plain SELECT is rejected by the gate too.  This is the deliberate
-- narrowing: such a query keeps losing its parallelism, exactly as it does
-- without the module, and nobody pays a tree walk to find out.
EXPLAIN (COSTS OFF)
SELECT nextval('sg_tmp_dst_id_seq'), a FROM sg_src ORDER BY b, a;

-- log mode reports and changes nothing.
SET seqguard.mode = log;

EXPLAIN (COSTS OFF)
INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src;

SET seqguard.mode = on;

\set VERBOSITY default
RESET seqguard.log_level;

--
-- Part 2: run time.
--
-- The substituted function has to be exercised with a Gather actually open,
-- which on community PostgreSQL an INSERT will not give us.  Calling it
-- directly gets there: it is PARALLEL RESTRICTED, so the planner is happy to
-- put a Gather Merge underneath it and the executor enters parallel mode.
--
EXPLAIN (COSTS OFF, VERBOSE)
SELECT seqguard_nextval('sg_tmp_dst_id_seq') FROM sg_src ORDER BY b, a;

SELECT setval('sg_tmp_dst_id_seq', 1, false);

-- One value per row, no duplicates, no gaps: the private path takes a single
-- value per call, as CACHE 1 does.
SELECT count(*), count(DISTINCT n), min(n), max(n)
FROM (SELECT seqguard_nextval('sg_tmp_dst_id_seq') AS n FROM sg_src ORDER BY b, a) s;

-- The run-time guard.  A permanent sequence reaching the function inside
-- parallel mode is handed back to the core, which raises exactly the error it
-- would have raised had the module never existed.
EXPLAIN (COSTS OFF)
SELECT seqguard_nextval('sg_perm_seq') FROM sg_src ORDER BY b, a LIMIT 1;

SELECT seqguard_nextval('sg_perm_seq') FROM sg_src ORDER BY b, a LIMIT 1;

-- Outside parallel mode the function is nextval(), cache and all - including
-- for a permanent sequence, which the guard above only refuses while a Gather
-- is open.
SELECT seqguard_nextval('sg_perm_seq');
SELECT currval('sg_perm_seq');

-- The parameters the private path caches follow ALTER SEQUENCE.  Every option
-- that affects future values forces a rewrite, so the relfilenumber test in
-- seqguard_nextval_local() sees it; the pg_sequence syscache callback would
-- catch it too.
ALTER SEQUENCE sg_tmp_dst_id_seq INCREMENT BY 10;
SELECT setval('sg_tmp_dst_id_seq', 1, false);

SELECT min(n), max(n)
FROM (SELECT seqguard_nextval('sg_tmp_dst_id_seq') AS n FROM sg_src ORDER BY b, a) s;

ALTER SEQUENCE sg_tmp_dst_id_seq INCREMENT BY 1;

--
-- Part 3: the whole thing end to end, and the documented limitation.
--
-- A serial column on a temporary table behaves exactly as it always did.
--
SELECT setval('sg_tmp_dst_id_seq', 1, false);

INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src WHERE a <= 5;

SELECT id, a FROM sg_tmp_dst ORDER BY id;

SELECT currval('sg_tmp_dst_id_seq');

-- The private path cannot reach the backend-local cache in sequence.c, so
-- currval() still reports what the core last cached, not the twenty thousand
-- values the parallel query produced.
SELECT count(*)
FROM (SELECT seqguard_nextval('sg_tmp_dst_id_seq') AS n FROM sg_src ORDER BY b, a) s;

SELECT currval('sg_tmp_dst_id_seq');

-- The sequence itself did move, which is what keeps the values unique.
SELECT last_value > 20000 AS advanced FROM sg_tmp_dst_id_seq;

--
-- Dropping the SQL half leaves the feature inert rather than broken.  The
-- cached function OID goes away with the pg_proc entry that invalidated it,
-- the next lookup fails, and the statement plans exactly as the core would
-- have planned it.
--
DROP TABLE sg_mixed_dst;

DROP EXTENSION enforce_workers;

SET seqguard.log_level = notice;
\set VERBOSITY terse

EXPLAIN (COSTS OFF)
INSERT INTO sg_tmp_dst (a) SELECT a FROM sg_src;

\set VERBOSITY default
RESET seqguard.log_level;

DROP TABLE sg_tmp_dst;
DROP TABLE sg_perm_dst;
DROP TABLE sg_src;
DROP SEQUENCE sg_perm_seq;
