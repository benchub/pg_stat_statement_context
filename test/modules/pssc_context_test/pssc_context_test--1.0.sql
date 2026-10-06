/* pssc_context_test--1.0.sql -- TEST-ONLY, see test/t/010_context.pl */

\echo Use "CREATE EXTENSION pssc_context_test" to load this file. \quit

-- The active frame as seen by the statement calling this function (its own
-- executor frame, unless it has none). tags are "key=value"; all columns
-- are NULL if no frame is active. nesting_level is the current level.
CREATE FUNCTION pssc_context_test_active(OUT tags text[],
                                         OUT utility bool,
                                         OUT nested bool,
                                         OUT toplevel bool,
                                         OUT recordable bool,
                                         OUT frame_nesting_level int,
                                         OUT queryid bigint,
                                         OUT nesting_level int)
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Just the active frame's tags (NULL if none).
CREATE FUNCTION pssc_context_test_tags() RETURNS text[]
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Registered executor frames in this backend (the calling statement's own
-- frame included) and the transaction-end checks (pssc_frame_xact_stats).
CREATE FUNCTION pssc_context_test_registry(OUT live int,
                                           OUT xact_checks bigint,
                                           OUT xact_leaks bigint)
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Frames seen at ExecutorEnd by the test hook since the last call (then
-- cleared), oldest first: the frame found by lookup, its statement text
-- (truncated to 200 bytes), and its level and user as refreshed at End
-- (start_* as when the frame was made). At most 1000 are kept (more is an
-- error).
CREATE FUNCTION pssc_context_test_ended(OUT query text,
                                        OUT tags text[],
                                        OUT toplevel bool,
                                        OUT start_toplevel bool,
                                        OUT recordable bool,
                                        OUT userid oid,
                                        OUT start_userid oid)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- ExecutorEnd calls whose QueryDesc had no registered frame although
-- queryId != 0 (should stay 0 while enabled).
CREATE FUNCTION pssc_context_test_end_misses() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Negative control for the transaction-end check (non-assert builds
-- only): make = true registers a fake executor frame whose context
-- outlives the transaction; make = false destroys that context.
CREATE FUNCTION pssc_context_test_leak(make bool) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;
