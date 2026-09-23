/* contrib/enforce-workers/enforce_workers--1.0.sql */

-- complain if the script is sourced in psql rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION enforce_workers" to load this file. \quit

--
-- The one SQL object in the module: a stand-in for nextval(regclass) that the
-- planner may place above a Gather.
--
-- PARALLEL RESTRICTED is the entire point.  nextval() itself is UNSAFE, which
-- costs the whole query its parallelism; RESTRICTED says only that the call
-- must stay in the leader, which is where it was always going to run.  See the
-- header comment of seqguard.c for why that is sound for a temporary sequence
-- and why the C code refuses to do it for any other kind.
--
-- VOLATILE and STRICT match nextval().  The function is deliberately not
-- documented for direct use: seqguard.c substitutes it into query trees, and
-- calling it by hand simply gets you nextval() with a longer name.
--
-- The default EXECUTE grant to PUBLIC is kept on purpose: the planner
-- substitutes this call into queries written by users who have never heard of
-- the extension, so anyone who could have run nextval() must be able to run
-- this.  That grants nothing extra - both paths check USAGE/UPDATE on the
-- sequence itself, exactly as nextval() does.
--
CREATE FUNCTION seqguard_nextval(regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'seqguard_nextval'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;
