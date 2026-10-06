-- Included by config.sql (and recursively by itself): one poll of the wait
-- for the reloaded configuration, for at most 300 polls of 0.1 s.
SELECT current_setting('pg_stat_statement_context.extractors') = :'extractors'
   AND current_setting('pg_stat_statement_context.tags') = :'tags'
   AND current_setting('pg_stat_statement_context.exclude_tags') = :'exclude_tags'
   AND (SELECT setting FROM pg_settings
         WHERE name = 'pg_stat_statement_context.scan_window') = :'scan_window'
       AS pssc_config_ok,
       :pssc_polls + 1 AS pssc_polls,
       :pssc_polls >= 300 AS pssc_config_timeout \gset
\if :pssc_config_ok
\elif :pssc_config_timeout
\echo 'configuration reload timed out'
\else
SELECT pg_sleep(0.1) \gset
\i test/sql/include/wait_config.sql
\endif
