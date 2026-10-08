# Loading the library with and without shared_preload_libraries.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $qid_sql = q{SELECT coalesce(query_id, 0) <> 0 FROM pg_stat_activity WHERE pid = pg_backend_pid();};

# Preloaded: the server starts cleanly and the extension can be created.
my $pre = PostgreSQL::Test::Cluster->new('preload');
$pre->init;
$pre->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_stat_statement_context'\ncompute_query_id = auto\n");
$pre->start;

is($pre->safe_psql('postgres', 'SELECT 1'), '1', 'preloaded server accepts queries');
$pre->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statement_context');
is( $pre->safe_psql('postgres',
		q{SELECT extversion FROM pg_extension WHERE extname = 'pg_stat_statement_context'}),
	'1.0', 'CREATE EXTENSION works when preloaded');
is($pre->safe_psql('postgres', $qid_sql), 't',
	'EnableQueryId(): compute_query_id = auto computes query IDs when preloaded');

# PG18+: PG_MODULE_MAGIC_EXT names the library and its version (the
# control file's default_version, which the Makefile passes as
# PSSC_EXT_VERSION), so pg_get_loaded_modules() identifies it.
SKIP:
{
	skip 'pg_get_loaded_modules() is PostgreSQL 18+', 1 if $pre->pg_version < 18;
	is( $pre->safe_psql('postgres',
			q{SELECT string_agg(coalesce(module_name, '?') || '|' || coalesce(version, '?'), ',')
			  FROM pg_get_loaded_modules()
			  WHERE file_name ~ '^pg_stat_statement_context\.(so|dylib|dll)$'}),
		'pg_stat_statement_context|1.0',
		'pg_get_loaded_modules() shows the module name and version');
}
$pre->stop;
my $log = slurp_file($pre->logfile);
unlike($log, qr/PANIC|FATAL|terminated by signal/, 'preloaded server log is clean');

# Not preloaded: LOAD must not crash and must not enable query IDs.
my $nopre = PostgreSQL::Test::Cluster->new('nopreload');
$nopre->init;
$nopre->append_conf('postgresql.conf', "compute_query_id = auto\n");
$nopre->start;

my $out = $nopre->safe_psql('postgres',
	"LOAD 'pg_stat_statement_context';\n$qid_sql\nSELECT 42;");
is($out, "f\n42", 'LOAD without preload is a no-op and the session survives');
is($nopre->safe_psql('postgres', 'SELECT 1'), '1', 'server still up after LOAD');
$nopre->stop;
$log = slurp_file($nopre->logfile);
unlike($log, qr/PANIC|terminated by signal/, 'no-preload server log is clean');

done_testing();
