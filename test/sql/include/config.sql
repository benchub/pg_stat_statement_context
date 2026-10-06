\set ECHO none
-- Included by test/sql/extract.sql (psql \i, relative to the directory
-- pg_regress runs in). Applies the sighup configuration held in the psql
-- variables :extractors, :tags, :exclude_tags and :scan_window (bytes), and
-- :normalize when it is set (test/sql/normalize.sql), with ALTER SYSTEM,
-- reloads, and waits until this backend sees all the values
-- (pg_reload_conf() only signals; a backend processes the reload between
-- two commands). Runs with ECHO none so the expected output does not depend
-- on how many polls the reload took; errors are still printed.
ALTER SYSTEM SET pg_stat_statement_context.extractors = :'extractors';
ALTER SYSTEM SET pg_stat_statement_context.tags = :'tags';
ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = :'exclude_tags';
ALTER SYSTEM SET pg_stat_statement_context.scan_window = :scan_window;
\if :{?normalize}
ALTER SYSTEM SET pg_stat_statement_context.normalize = :'normalize';
\endif
SELECT pg_reload_conf() AS pssc_reload \gset
\set pssc_polls 0
\i test/sql/include/wait_config.sql
\set ECHO all
