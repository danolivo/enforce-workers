LOAD 'enforce_workers';

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

CREATE TEMP TABLE sg_tmp_dst (id serial, a int);
CREATE SEQUENCE sg_perm_seq;

--
-- The function is PARALLEL RESTRICTED, so the planner is happy to put a Gather
-- Merge underneath it.  That is what the executor needs for parallel mode to
-- be open while the call runs, which is the state the whole file is about.
--
EXPLAIN (COSTS OFF, VERBOSE)
SELECT seqguard_nextval('sg_tmp_dst_id_seq') FROM sg_src ORDER BY b, a;

SELECT setval('sg_tmp_dst_id_seq', 1, false);

-- One value per row, no duplicates, no gaps: the private path takes a single
-- value per call, as CACHE 1 does.
SELECT count(*), count(DISTINCT n), min(n), max(n)
FROM (SELECT seqguard_nextval('sg_tmp_dst_id_seq') AS n FROM sg_src ORDER BY b, a) s;

--
-- The guard.  A permanent sequence reaching the function inside parallel mode
-- is handed back to the core, which raises exactly the error it would have
-- raised had the function never existed.
--
EXPLAIN (COSTS OFF)
SELECT seqguard_nextval('sg_perm_seq') FROM sg_src ORDER BY b, a LIMIT 1;

SELECT seqguard_nextval('sg_perm_seq') FROM sg_src ORDER BY b, a LIMIT 1;

--
-- Outside parallel mode the function is nextval(), cache and all - including
-- for a permanent sequence, which the guard above only refuses while a Gather
-- is open.
--
SELECT seqguard_nextval('sg_perm_seq');
SELECT currval('sg_perm_seq');

SELECT setval('sg_tmp_dst_id_seq', 1, false);
SELECT seqguard_nextval('sg_tmp_dst_id_seq');
SELECT currval('sg_tmp_dst_id_seq');

--
-- The documented limitation.  The private path cannot reach the backend-local
-- cache in sequence.c, so currval() still reports what the core last cached,
-- not the twenty thousand values the parallel query produced.
--
SELECT count(*)
FROM (SELECT seqguard_nextval('sg_tmp_dst_id_seq') AS n FROM sg_src ORDER BY b, a) s;

SELECT currval('sg_tmp_dst_id_seq');

-- The sequence itself did move, which is what keeps the values unique.
SELECT last_value > 20000 AS advanced FROM sg_tmp_dst_id_seq;

DROP TABLE sg_tmp_dst;
DROP TABLE sg_src;
DROP SEQUENCE sg_perm_seq;
DROP EXTENSION enforce_workers;
