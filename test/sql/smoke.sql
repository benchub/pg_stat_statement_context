-- Smoke test: requires the library in shared_preload_libraries.
CREATE EXTENSION pg_stat_statement_context;

SELECT extname, extversion
  FROM pg_extension
 WHERE extname = 'pg_stat_statement_context';

SELECT 'pg_stat_statement_context' = ANY (
         string_to_array(replace(current_setting('shared_preload_libraries'), ' ', ''), ',')
       ) AS preloaded;

-- _PG_init calls EnableQueryId(), so compute_query_id = auto behaves as on
-- and the current statement gets a query ID.
SET compute_query_id = auto;
SELECT query_id IS NOT NULL AND query_id <> 0 AS has_query_id
  FROM pg_stat_activity
 WHERE pid = pg_backend_pid();
RESET compute_query_id;

DROP EXTENSION pg_stat_statement_context;
