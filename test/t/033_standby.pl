# A primary and a streaming standby (DESIGN.md §5, §5.5; backlog
# 20261008-065635-4). The store lives in each instance's shared memory and is
# not WAL-logged:
#   - read-only tagged statements on the standby are recorded in the
#     standby's own store;
#   - the primary's entries never appear on the standby, and vice versa;
#   - with save = on, a clean (fast) standby restart saves and reloads its
#     statistics: pg_control then says DB_SHUTDOWNED_IN_RECOVERY, which
#     cluster_shut_down_cleanly() (src/store.c) must accept;
#   - an immediate standby shutdown saves nothing, and since the previous
#     file was consumed at load the standby starts empty;
#   - after promotion the new primary keeps its in-memory history and keeps
#     recording (reads and writes), and a clean restart of it saves and
#     reloads as usual.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
});
$primary->start;
$primary->safe_psql('postgres', "CREATE EXTENSION $P; CREATE TABLE t_rep(i int)");
$primary->safe_psql('postgres', 'INSERT INTO t_rep VALUES (1)');

# postgresql.conf (and so shared_preload_libraries) comes with the backup.
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 't', 'the standby is in recovery');
is($standby->safe_psql('postgres', "SHOW $P.save"), 'on', 'save is on on the standby');

my $DUMP = $standby->data_dir . "/pg_stat/$P.stat";

# The controller tag values recorded by $node, with their call totals.
sub controllers
{
	my ($node) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT coalesce(string_agg(c || ':' || n, ',' ORDER BY c), '')
		   FROM (SELECT tags->>'controller' AS c, sum(calls_total) AS n
		         FROM ${P}_totals WHERE tags ? 'controller' GROUP BY 1) s});
}

# Stops $node in $mode, then starts it again; returns the log of the cycle.
sub restart
{
	my ($node, $mode, $between) = @_;
	my $pos = -s $node->logfile;
	$node->stop($mode);
	$between->() if $between;
	$node->start;
	return substr(slurp_file($node->logfile), $pos);
}

# ------------------------------------------ recording on the standby
$primary->safe_psql('postgres', q{SELECT count(*) FROM t_rep /*controller='on_primary'*/}) for 1 .. 2;
$standby->safe_psql('postgres', q{SELECT count(*) FROM t_rep /*controller='on_standby'*/}) for 1 .. 3;
$primary->wait_for_catchup($standby);

is(controllers($standby), 'on_standby:3',
	'a read-only tagged statement on the standby is recorded there, and only the standby\'s');
is(controllers($primary), 'on_primary:2',
	'the primary records only its own statements (the store is not replicated)');

# -------------------------------------- clean standby restart: reloaded
{
	my $log = restart($standby, 'fast', sub {
		ok(-f $DUMP, 'a fast standby shutdown writes the dump file');
	});
	unlike($log, qr/not saving statistics/, 'LOG: the standby shutdown counts as clean');
	like($log, qr/loaded (\d+) of \1 saved entries/, 'LOG: the standby reloads its entries');
	ok(!-e $DUMP, 'the dump file is unlinked after the load');
	is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 't', 'still a standby');
	is(controllers($standby), 'on_standby:3', 'the standby\'s statistics survive its clean restart');

	$standby->safe_psql('postgres', q{SELECT count(*) FROM t_rep /*controller='on_standby'*/});
	is(controllers($standby), 'on_standby:4', 'recording finds the reloaded entry');
	is($standby->safe_psql('postgres',
			"SELECT count(*) FROM ${P}_totals WHERE tags->>'controller' = 'on_standby'"),
		1, 'no duplicate entry');
}

# ------------------------------- immediate standby shutdown: discarded
{
	my $log = restart($standby, 'immediate', sub {
		ok(!-e $DUMP, 'an immediate standby shutdown writes nothing');
	});
	like($log, qr/not saving statistics: the server did not shut down cleanly/,
		'LOG: an immediate standby shutdown is not a clean one');
	is($standby->safe_psql('postgres', "SELECT entries FROM ${P}_info()"), 0,
		'the standby starts empty after an immediate shutdown');
	is(controllers($standby), '', 'no saved statistics replayed');
	$standby->poll_query_until('postgres', 'SELECT pg_is_in_recovery()', 't')
	  or die 'standby';
	$primary->wait_for_catchup($standby);
}

# ------------------------------------------------------------ promotion
{
	$standby->safe_psql('postgres', q{SELECT count(*) FROM t_rep /*controller='before_promote'*/})
	  for 1 .. 2;
	is(controllers($standby), 'before_promote:2', 'before promotion');

	$primary->stop;
	$standby->promote;
	$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die 'promotion';

	is(controllers($standby), 'before_promote:2',
		'the in-memory history survives promotion');
	$standby->safe_psql('postgres', q{SELECT count(*) FROM t_rep /*controller='before_promote'*/});
	$standby->safe_psql('postgres', q{INSERT INTO t_rep VALUES (2) /*controller='after_promote'*/});
	is(controllers($standby), 'after_promote:1,before_promote:3',
		'the new primary keeps recording, reads and writes');

	my $log = restart($standby, 'fast');
	like($log, qr/loaded (\d+) of \1 saved entries/, 'a clean restart of the new primary reloads it');
	is(controllers($standby), 'after_promote:1,before_promote:3', 'with the same totals');
}

$standby->stop;

done_testing();
