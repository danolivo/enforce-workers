LOAD 'enforce_workers';

-- Keep every plan below serial: the module also removes the relation-size gate
-- on parallelism, and nothing here is about that.
SET max_parallel_workers_per_gather = 0;

-- Report what idxdefer does, and keep the planner's row estimates out of the
-- expected output: terse drops the DETAIL line that carries them, and the
-- CONTEXT line of errors raised during a rebuild.
SET idxdefer.log_level = notice;
SET idxdefer.min_rows = 1000;
\set VERBOSITY terse

-- Rebuilt indexes are checked with amcheck where it is installed, including
-- heapallindexed, which proves that every heap tuple that ought to be in the
-- index is there.  Without amcheck the checks below still compare the index
-- with the heap row by row; the output is the same either way.
DO $$
BEGIN
	IF EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'amcheck') THEN
		SET LOCAL client_min_messages = warning;
		CREATE EXTENSION IF NOT EXISTS amcheck;
	END IF;
END
$$;

CREATE FUNCTION idx_ok(idx regclass) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	IF to_regprocedure('bt_index_check(regclass, boolean)') IS NOT NULL THEN
		EXECUTE 'SELECT bt_index_check($1, true)' USING idx;
	END IF;
	RETURN true;
END
$$;

-- relfilenode changes exactly when an index is rebuilt.
CREATE FUNCTION idx_node(idx regclass) RETURNS oid LANGUAGE sql AS $$
	SELECT relfilenode FROM pg_class WHERE oid = idx
$$;

--
-- The case the feature exists for: a bulk INSERT ... SELECT into an empty
-- temporary table with indexes.  Two of them, and one of each kind the
-- rebuild has to evaluate: a plain column, an expression, a predicate.
--
CREATE TEMP TABLE t1 (a int, b text);
CREATE INDEX t1_a ON t1 (a);
CREATE INDEX t1_expr ON t1 (lower(b));
CREATE INDEX t1_part ON t1 (a) WHERE a % 10 = 0;
SELECT idx_node('t1_a') AS t1_a_before \gset

INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(1, 5000) i;

SELECT idx_node('t1_a') <> :t1_a_before AS rebuilt;
SELECT idx_ok('t1_a'), idx_ok('t1_expr'), idx_ok('t1_part');

-- Every row through each index, and the same answers the heap gives.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t1 WHERE a BETWEEN 1 AND 5000;
SELECT count(*), sum(a) FROM t1 WHERE a BETWEEN 1 AND 5000;
EXPLAIN (COSTS OFF) SELECT a FROM t1 WHERE lower(b) = 'row 4242';
SELECT a FROM t1 WHERE lower(b) = 'row 4242';
EXPLAIN (COSTS OFF) SELECT count(*) FROM t1 WHERE a % 10 = 0 AND a > 0;
SELECT count(*) FROM t1 WHERE a % 10 = 0 AND a > 0;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*), sum(a) FROM t1;

-- The table is no longer empty: the next insert maintains the indexes itself.
SELECT idx_node('t1_a') AS t1_a_before \gset
INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(5001, 10000) i;
SELECT idx_node('t1_a') = :t1_a_before AS untouched, idx_ok('t1_a');
SELECT count(*) FROM t1;

-- TRUNCATE empties it again, and the next insert is deferred again.
TRUNCATE t1;
INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(1, 3000) i;
SELECT idx_ok('t1_a'), idx_ok('t1_expr'), idx_ok('t1_part'), count(*) FROM t1;

-- An insert that brings no rows is deferred, and has nothing to rebuild.
TRUNCATE t1;
SELECT idx_node('t1_a') AS t1_a_before \gset
INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(1, 5000) i WHERE i < 0;
SELECT idx_node('t1_a') = :t1_a_before AS untouched;

-- EXPLAIN without ANALYZE runs nothing, so it defers nothing.
EXPLAIN (COSTS OFF) INSERT INTO t1 SELECT i, 'x' FROM generate_series(1, 5000) i;

-- EXPLAIN ANALYZE does run the insert, and so defers and rebuilds.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(1, 5000) i;
SELECT idx_ok('t1_a'), count(*) FROM t1;

-- A prepared insert with a cached generic plan: the deferral invalidates that
-- plan, and each execution is replanned and judged afresh - deferred while the
-- table is empty, left alone once it is not.
TRUNCATE t1;
SET plan_cache_mode = force_generic_plan;
PREPARE ins(int) AS
	INSERT INTO t1 SELECT i, 'Row ' || i FROM generate_series(1, $1) i;
EXECUTE ins(3000);
SELECT idx_ok('t1_a'), idx_ok('t1_expr'), idx_ok('t1_part'), count(*) FROM t1;
TRUNCATE t1;
EXECUTE ins(3000);
SELECT idx_ok('t1_a'), count(*) FROM t1;
SELECT idx_node('t1_a') AS t1_a_before \gset
EXECUTE ins(3000);
SELECT idx_node('t1_a') = :t1_a_before AS untouched, idx_ok('t1_a'), count(*) FROM t1;
DEALLOCATE ins;
RESET plan_cache_mode;

--
-- What is left alone.
--

-- Below idxdefer.min_rows.
CREATE TEMP TABLE small (a int);
CREATE INDEX small_a ON small (a);
SELECT idx_node('small_a') AS small_a_before \gset
INSERT INTO small SELECT generate_series(1, 10);
SELECT idx_node('small_a') = :small_a_before AS untouched, idx_ok('small_a');

-- A permanent table.
CREATE TABLE perm (a int);
CREATE INDEX perm_a ON perm (a);
INSERT INTO perm SELECT generate_series(1, 5000);
SELECT idx_ok('perm_a');
DROP TABLE perm;

-- A unique index is checked on every row, so it keeps being maintained, and
-- the duplicate is still caught where it happens.
CREATE TEMP TABLE uniq (a int);
CREATE UNIQUE INDEX uniq_a ON uniq (a);
INSERT INTO uniq SELECT i % 3000 FROM generate_series(1, 5000) i;
INSERT INTO uniq SELECT generate_series(1, 5000);
SELECT idx_ok('uniq_a'), count(*) FROM uniq;

-- A primary key is a unique index as well.
CREATE TEMP TABLE pk (a int PRIMARY KEY, b int);
CREATE INDEX pk_b ON pk (b);
INSERT INTO pk SELECT i, i FROM generate_series(1, 5000) i;
SELECT idx_ok('pk_pkey'), idx_ok('pk_b');

-- ON CONFLICT, even with nothing to conflict on.
CREATE TEMP TABLE onconf (a int);
CREATE INDEX onconf_a ON onconf (a);
INSERT INTO onconf SELECT generate_series(1, 5000) ON CONFLICT DO NOTHING;
SELECT idx_ok('onconf_a');

-- RETURNING, and a data-modifying CTE.
CREATE TEMP TABLE ret (a int);
CREATE INDEX ret_a ON ret (a);
\o /dev/null
INSERT INTO ret SELECT generate_series(1, 5000) RETURNING a;
\o
WITH ins AS (INSERT INTO ret SELECT generate_series(1, 5000) RETURNING a)
SELECT count(*) FROM ins;
TRUNCATE ret;
WITH src AS (SELECT generate_series(1, 5000) AS a)
INSERT INTO ret SELECT a FROM src;
SELECT idx_ok('ret_a'), count(*) FROM ret;

-- A trigger, which could look at the table through an index.
CREATE FUNCTION trg_noop() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	RETURN NEW;
END
$$;
CREATE TEMP TABLE trg (a int);
CREATE INDEX trg_a ON trg (a);
CREATE TRIGGER trg_t BEFORE INSERT ON trg FOR EACH ROW EXECUTE FUNCTION trg_noop();
INSERT INTO trg SELECT generate_series(1, 5000);
SELECT idx_ok('trg_a');

-- An index that is already open, here by a cursor in the same transaction:
-- it could not be rebuilt underneath the cursor.
BEGIN;
CREATE TEMP TABLE curs (a int);
CREATE INDEX curs_a ON curs (a);
SET LOCAL enable_seqscan = off;
DECLARE c CURSOR FOR SELECT a FROM curs WHERE a > 0;
SELECT idx_node('curs_a') AS curs_a_before \gset
INSERT INTO curs SELECT generate_series(1, 5000);
FETCH ALL FROM c;
CLOSE c;
SELECT idx_node('curs_a') = :curs_a_before AS untouched, idx_ok('curs_a');
SELECT count(*) FROM curs WHERE a > 0;
COMMIT;

--
-- log reports and changes nothing; off does not even report.
--
CREATE TEMP TABLE modes (a int);
CREATE INDEX modes_a ON modes (a);
SELECT idx_node('modes_a') AS modes_a_before \gset
SET idxdefer.mode = log;
INSERT INTO modes SELECT generate_series(1, 5000);
SELECT idx_node('modes_a') = :modes_a_before AS untouched, idx_ok('modes_a');
TRUNCATE modes;
SELECT idx_node('modes_a') AS modes_a_before \gset
SET idxdefer.mode = off;
INSERT INTO modes SELECT generate_series(1, 5000);
SELECT idx_node('modes_a') = :modes_a_before AS untouched, idx_ok('modes_a');
RESET idxdefer.mode;

--
-- Errors.  Nothing is ever left to repair.
--

-- The insert fails halfway: the rows it wrote are dead, the index never heard
-- of them, and nothing is rebuilt.
CREATE TEMP TABLE err_ins (a int);
CREATE INDEX err_ins_a ON err_ins (a);
INSERT INTO err_ins SELECT 10000 / (i - 4000) FROM generate_series(1, 5000) i;
SELECT idx_ok('err_ins_a'), count(*) FROM err_ins;
-- The heap now has pages, so the next insert is not deferred - and is right.
INSERT INTO err_ins SELECT generate_series(1, 5000);
SET enable_seqscan = off;
SELECT count(*) FROM err_ins WHERE a > 0;
RESET enable_seqscan;

-- The rebuild fails: an index expression that the rows make divide by zero.
-- Without the module the insert itself would fail on the same row.  The new
-- relfilenode goes away with the transaction, and the old index is right for
-- the heap that is left.
CREATE TEMP TABLE err_build (a int);
CREATE INDEX err_build_e ON err_build ((100 / (a - 4000)));
INSERT INTO err_build SELECT generate_series(1, 5000);
SELECT idx_ok('err_build_e'), count(*) FROM err_build;
INSERT INTO err_build SELECT i FROM generate_series(1, 3000) i;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM err_build WHERE 100 / (a - 4000) = 0;
SELECT count(*) FROM err_build WHERE 100 / (a - 4000) = 0;
RESET enable_seqscan;

-- Rolled back to a savepoint after the rebuild.
BEGIN;
CREATE TEMP TABLE sp (a int);
CREATE INDEX sp_a ON sp (a);
SAVEPOINT s;
INSERT INTO sp SELECT generate_series(1, 5000);
ROLLBACK TO SAVEPOINT s;
SELECT idx_ok('sp_a'), count(*) FROM sp;
INSERT INTO sp SELECT generate_series(1, 5000);
SELECT idx_ok('sp_a'), count(*) FROM sp;
COMMIT;

-- Failed inside a PL/pgSQL exception block, which is a subtransaction that
-- never reaches ExecutorEnd; the transaction then commits normally.
CREATE TEMP TABLE blk (a int);
CREATE INDEX blk_a ON blk (a);
DO $$
BEGIN
	BEGIN
		INSERT INTO blk SELECT 10000 / (i - 4000) FROM generate_series(1, 5000) i;
	EXCEPTION WHEN division_by_zero THEN
		RAISE NOTICE 'caught';
	END;
END
$$;
SELECT idx_ok('blk_a'), count(*) FROM blk;

--
-- A VOLATILE function that reads the target while the insert is running sees
-- the rows inserted so far.  It must go on seeing them, although they are not
-- in the index yet: its query is planned without the target's indexes.  The
-- function's plan is cached first, as a generic plan through the index, to
-- show that the cached plan is not used either; and it is first called late
-- in the statement, when many rows are already in.
--
CREATE TEMP TABLE vol (a int, seen bigint);
CREATE INDEX vol_a ON vol (a);
CREATE FUNCTION vol_seen(k int) RETURNS bigint LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	n bigint;
BEGIN
	SELECT count(*) INTO n FROM vol WHERE a = k - 1;
	RETURN n;
END
$$;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET plan_cache_mode = force_generic_plan;
SELECT vol_seen(1);
INSERT INTO vol
	SELECT i, CASE WHEN i > 2500 THEN vol_seen(i) END
	FROM generate_series(1, 5000) i;
SELECT seen, count(*) FROM vol GROUP BY seen ORDER BY seen;
-- Afterwards the same function goes through the rebuilt index again.
EXPLAIN (COSTS OFF) SELECT count(*) FROM vol WHERE a = 42;
SELECT vol_seen(43);
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT idx_ok('vol_a');

-- A deferred insert nested inside a function called by another query.
CREATE TEMP TABLE nest (a int);
CREATE INDEX nest_a ON nest (a);
CREATE FUNCTION fill_nest() RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO nest SELECT generate_series(1, 5000);
	RETURN (SELECT count(*) FROM nest);
END
$$;
SELECT fill_nest();
SELECT idx_ok('nest_a');

-- A deferred insert nested inside a deferred insert into the same table.  The
-- inner one runs while the outer one computes its first row, when the table is
-- still empty, so both are deferred; the inner rebuild comes first, and the
-- outer one then rebuilds over both.
CREATE TEMP TABLE nest2 (a int);
CREATE INDEX nest2_a ON nest2 (a);
CREATE FUNCTION fill_nest2(k int) RETURNS int LANGUAGE plpgsql VOLATILE AS $$
BEGIN
	IF k = 1 THEN
		INSERT INTO nest2 SELECT -g FROM generate_series(1, 3000) g;
	END IF;
	RETURN k;
END
$$;
INSERT INTO nest2 SELECT fill_nest2(i) FROM generate_series(1, 3000) i;
SELECT idx_ok('nest2_a'), count(*) FROM nest2;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM nest2 WHERE a < 0;
SELECT count(*) FROM nest2 WHERE a > 0;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A VOLATILE function that updates and deletes rows of the target while the
-- insert runs.  Its UPDATE opens the indexes for itself and maintains them,
-- stale as they are; the rebuild replaces them anyway.  Updates of the
-- indexed column b are not HOT, those of c are.
CREATE TEMP TABLE upd (a int, b int, c int);
CREATE INDEX upd_a ON upd (a);
CREATE INDEX upd_b ON upd (b);
CREATE FUNCTION upd_touch(k int) RETURNS int LANGUAGE plpgsql VOLATILE AS $$
BEGIN
	IF k % 3 = 0 THEN
		UPDATE upd SET b = -b WHERE a = k - 1;
	ELSIF k % 3 = 1 THEN
		UPDATE upd SET c = k WHERE a = k - 1;
	END IF;
	IF k % 7 = 0 THEN
		DELETE FROM upd WHERE a = k - 2;
	END IF;
	RETURN k;
END
$$;
INSERT INTO upd SELECT upd_touch(i), i, 0 FROM generate_series(1, 2000) i;
SELECT idx_ok('upd_a'), idx_ok('upd_b');
SELECT count(*), count(*) FILTER (WHERE b < 0) AS negated,
	   count(*) FILTER (WHERE c > 0) AS hot FROM upd;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM upd WHERE b < 0;
SELECT count(*) FROM upd WHERE b < 0;
SELECT count(*) FROM upd WHERE a > 0;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- The same VOLATILE reader as above, under REPEATABLE READ and SERIALIZABLE:
-- the transaction snapshot is fixed, but the function still sees the rows
-- this statement has inserted so far, and must go on seeing all of them.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET plan_cache_mode = force_generic_plan;
BEGIN ISOLATION LEVEL REPEATABLE READ;
TRUNCATE vol;
INSERT INTO vol
	SELECT i, CASE WHEN i > 2500 THEN vol_seen(i) END
	FROM generate_series(1, 5000) i;
SELECT seen, count(*) FROM vol GROUP BY seen ORDER BY seen;
SELECT idx_ok('vol_a');
COMMIT;
BEGIN ISOLATION LEVEL SERIALIZABLE;
TRUNCATE vol;
INSERT INTO vol
	SELECT i, CASE WHEN i > 2500 THEN vol_seen(i) END
	FROM generate_series(1, 5000) i;
SELECT seen, count(*) FROM vol GROUP BY seen ORDER BY seen;
SELECT idx_ok('vol_a');
COMMIT;
RESET plan_cache_mode;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A GIN index goes through the same rebuild.
CREATE TEMP TABLE gin_t (a int[]);
CREATE INDEX gin_t_a ON gin_t USING gin (a);
INSERT INTO gin_t SELECT ARRAY[i, i + 1] FROM generate_series(1, 5000) i;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM gin_t WHERE a @> ARRAY[4242];
SELECT count(*) FROM gin_t WHERE a @> ARRAY[4242];
RESET enable_seqscan;

DROP FUNCTION fill_nest();
DROP FUNCTION fill_nest2(int);
DROP FUNCTION upd_touch(int);
DROP FUNCTION vol_seen(int);
DROP FUNCTION trg_noop() CASCADE;
DROP FUNCTION idx_node(regclass);
DROP FUNCTION idx_ok(regclass);
