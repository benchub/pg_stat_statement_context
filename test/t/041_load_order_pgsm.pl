# shared_preload_libraries load-order WARNING for pg_stat_monitor (DESIGN.md
# §3.2, §6.12; docs/maintaining.md §4; backlog 20261008-092913-1). Like
# pg_stat_statements, pg_stat_monitor clears the queryId of the utilities it
# tracks before it chains, so it must be listed before this extension, and
# _PG_init warns once per startup when it is listed after. The HINT gives the
# working order of the libraries in the list: pg_stat_statements (when
# listed), pg_stat_monitor, this extension.
#
# Covers: pg_stat_monitor after this extension warns exactly once, with the
# DETAIL and HINT, also as a quoted $libdir path with the platform suffix;
# pg_stat_monitor first does not warn; with pg_stat_statements too, the
# documented order does not warn, pg_stat_monitor alone after this extension
# warns about it only, and both after warn about each.
#
# pg_stat_monitor has no PGDG Debian package: the test is skipped unless it
# is installed, and fails if PSSC_REQUIRE_MODULES lists it but it is not.
# It uses no TEST-ONLY module, so it runs against the release build too.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('load_order_pgsm');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
});
$node->start;

my $SO = module_suffix($node, 'pg_stat_monitor');
if (!defined $SO)
{
	$node->stop;
	plan skip_all => 'pg_stat_monitor is not installed';
}
my $have_pgss = defined pgss_suffix($node);

sub warn_re
{
	my ($lib) = @_;
	return qr/WARNING:  $lib is loaded after $P in shared_preload_libraries/;
}
my $pgsm_re = warn_re('pg_stat_monitor');
my $pgss_re = warn_re('pg_stat_statements');

sub count_matches
{
	my ($re) = @_;
	my $log = slurp_file($node->logfile);
	my $n = () = $log =~ /$re/g;
	return $n;
}

# Restarts with the given shared_preload_libraries value and returns the
# number of pg_stat_monitor and pg_stat_statements load-order warnings the
# restart added to the log, and the log it added.
sub restart_with
{
	my ($spl) = @_;
	my $off = -s $node->logfile;
	my ($m0, $s0) = (count_matches($pgsm_re), count_matches($pgss_re));
	$node->append_conf('postgresql.conf', "shared_preload_libraries = $spl\n");
	$node->restart;
	# A few backends, so a per-backend re-run of _PG_init would show up.
	$node->safe_psql('postgres', 'SELECT 1') for 1 .. 3;
	my $added = substr(slurp_file($node->logfile), $off);
	return (count_matches($pgsm_re) - $m0, count_matches($pgss_re) - $s0,
		$added);
}

# Whether the log has the WARNING about $lib followed by its DETAIL and the
# HINT with the order $order.
sub warned_with_hint
{
	my ($log, $lib, $order) = @_;
	my $w = warn_re($lib);
	return $log =~
	  /$w\n[^\n]*DETAIL:  [^\n]*ProcessUtility hook of $lib [^\n]*utility_missing_queryid[^\n]*\n[^\n]*HINT:  Set shared_preload_libraries = '\Q$order\E' and restart the server\./;
}

# --------------------------------------------------- pg_stat_monitor alone
my ($m, $s, $log) = restart_with("'pg_stat_monitor, $P'");
is($m, 0, 'pg_stat_monitor first: no warning');

($m, $s, $log) = restart_with("'$P, pg_stat_monitor'");
is($m, 1, 'pg_stat_monitor after this extension: exactly one warning');
ok(warned_with_hint($log, 'pg_stat_monitor', "pg_stat_monitor, $P"),
	'pg_stat_monitor after: warning, detail and hint are logged together');

($m, $s, $log) =
  restart_with(qq{' "\$libdir/$P" , "\$libdir/pg_stat_monitor$SO" '});
is($m, 1, 'quoted $libdir paths with suffix, pg_stat_monitor after: one warning');

($m, $s, $log) =
  restart_with(qq{'"\$libdir/pg_stat_monitor$SO",$P'});
is($m, 0, 'quoted $libdir path with suffix, pg_stat_monitor first: no warning');

# ------------------------------------------------ with pg_stat_statements
SKIP:
{
	skip 'pg_stat_statements is not installed', 9 unless $have_pgss;

	($m, $s, $log) =
	  restart_with("'pg_stat_statements, pg_stat_monitor, $P'");
	is($m + $s, 0, 'documented order of all three: no warning');

	($m, $s, $log) =
	  restart_with("'pg_stat_statements, $P, pg_stat_monitor'");
	is($m, 1, 'pgss first, pg_stat_monitor after: one pg_stat_monitor warning');
	is($s, 0, 'pgss first, pg_stat_monitor after: no pgss warning');
	ok( warned_with_hint(
			$log, 'pg_stat_monitor',
			"pg_stat_statements, pg_stat_monitor, $P"),
		'pgss listed: the hint gives the order of all three');

	($m, $s, $log) =
	  restart_with("'$P, pg_stat_monitor, pg_stat_statements'");
	is($m, 1, 'both after: one pg_stat_monitor warning');
	is($s, 1, 'both after: one pgss warning');
	ok( warned_with_hint(
			$log, 'pg_stat_statements',
			"pg_stat_statements, pg_stat_monitor, $P"),
		'both after: the pgss warning has the order of all three');

	($m, $s, $log) =
	  restart_with("'pg_stat_monitor, $P, pg_stat_statements'");
	is($m, 0, 'pg_stat_monitor first, pgss after: no pg_stat_monitor warning');
	is($s, 1, 'pg_stat_monitor first, pgss after: one pgss warning');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash');

done_testing();
