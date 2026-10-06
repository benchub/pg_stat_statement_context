-- Monitoring role for the docs/integrations recipes.
-- Run as a superuser, in the database the exporter connects to. The
-- extension must be created there (CREATE EXTENSION pg_stat_statement_context);
-- its views are cluster-wide, so one database is enough.
--
-- Set the password separately, e.g. in psql:  \password pssc_monitor
CREATE ROLE pssc_monitor LOGIN;

-- pg_read_all_stats is what pg_stat_statement_context needs: without it,
-- rows of other roles have queryid and tags = NULL. pg_monitor includes it
-- (plus pg_read_all_settings and pg_stat_scan_tables), and is what
-- postgres_exporter's built-in collectors need. Grant only
-- pg_read_all_stats if the role is used for these recipes alone.
GRANT pg_monitor TO pssc_monitor;

-- Optional: keep a stuck view from piling up scrapes.
ALTER ROLE pssc_monitor SET statement_timeout = '10s';
