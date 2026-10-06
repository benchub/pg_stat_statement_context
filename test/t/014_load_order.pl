# shared_preload_libraries load-order check (DESIGN.md §3.2, §6.12; backlog
# 20261005-091225-19): _PG_init parses shared_preload_libraries like the
# postmaster does and logs one WARNING (with the required order in its
# HINT) when pg_stat_statements is listed after this extension, because
# pgss's ProcessUtility hook is then outside ours and zeroes the utility's
# queryId. It takes no other action: utility tracking stays enabled.
#
# Covers: the wrong order warns exactly once and utilities are still
# processed (counted in utility_missing_queryid while pgss zeroes their
# queryId, recorded when pgss's track_utility is off); the documented order
# and no pgss at all do not warn; quoted, $libdir-path, suffixed and
# whitespace-padded entries are matched by basename; a duplicate entry does
# not change the order of the first load; letter case is ignored (checked
# through pssc_guc_test_load_order_wrong(), as mixed-case names do not load
# on a case-sensitive filesystem).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';
my $warn_re =
  qr/WARNING:  pg_stat_statements is loaded after $P in shared_preload_libraries/;
my $hint_re =
  qr/HINT:  Set shared_preload_libraries = 'pg_stat_statements, $P'/;

my $node = PostgreSQL::Test::Cluster->new('load_order');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
$P.extractors = 'sqlcommenter(position=any)'
});
$node->start;

my $pkglibdir = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
if (!-e "$pkglibdir/pg_stat_statements.so")
{
	plan skip_all => 'pg_stat_statements is not installed';
}

sub count_matches
{
	my ($re) = @_;
	my $log = slurp_file($node->logfile);
	my $n = () = $log =~ /$re/g;
	return $n;
}

# Restarts with the given shared_preload_libraries value and returns the
# number of load-order warnings and hints the restart added to the log.
sub restart_with
{
	my ($spl) = @_;
	my $w0 = count_matches($warn_re);
	my $h0 = count_matches($hint_re);
	$node->append_conf('postgresql.conf', "shared_preload_libraries = $spl\n");
	$node->restart;
	# A few backends, so a per-backend re-run of _PG_init would show up.
	$node->safe_psql('postgres', 'SELECT 1') for 1 .. 3;
	return (count_matches($warn_re) - $w0, count_matches($hint_re) - $h0);
}

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counter
{
	return sql("SELECT $_[0] FROM pssc_store_test_counters()");
}

# --------------------------------------------------------- no pgss at all
my ($w, $h) = restart_with("'$P'");
is($w, 0, 'no pgss: no warning');

sql('CREATE EXTENSION pssc_store_test');
sql(q{
CREATE VIEW rec AS
  SELECT toplevel, tags, sum(calls) AS calls
    FROM pssc_store_test_entries()
   WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
   GROUP BY 1, 2;
});

# ------------------------------------------------------- documented order
($w, $h) = restart_with("'pg_stat_statements, $P'");
is($w, 0, 'pgss first: no warning');
sql('CREATE EXTENSION pg_stat_statements');

# ---------------------------------------------------------- wrong order
($w, $h) = restart_with("'$P, pg_stat_statements'");
is($w, 1, 'pgss after this extension: exactly one warning');
is($h, 1, 'wrong order: the hint names the required order');
ok( slurp_file($node->logfile) =~
	  /$warn_re\n[^\n]*DETAIL:  [^\n]*utility_missing_queryid[^\n]*\n[^\n]*$hint_re/,
	'wrong order: warning, detail and hint are logged together');

# Utility tracking is not disabled: pgss zeroes the queryId of a tracked
# utility, which is counted ...
my $m0 = counter('utility_missing_queryid');
sql(q{CREATE TABLE lo_wrong(i int) /*controller='lo_wrong'*/});
is(counter('utility_missing_queryid') - $m0, 1,
	'wrong order: utility still processed (counted as missing queryid)');
# ... and recorded when pgss leaves the queryId alone.
sql(q{ALTER SYSTEM SET pg_stat_statements.track_utility = off});
sql('SELECT pg_reload_conf()');
$node->poll_query_until('postgres', 'SHOW pg_stat_statements.track_utility',
	'off')
  or die 'pg_stat_statements.track_utility did not become off';
sql(q{CREATE TABLE lo_wrong2(i int) /*controller='lo_wrong2'*/});
is(sql(q{SELECT toplevel, calls FROM rec WHERE tags = '{controller,lo_wrong2}'}),
	't|1', 'wrong order: utility still recorded');
sql(q{ALTER SYSTEM RESET pg_stat_statements.track_utility});
sql('SELECT pg_reload_conf()');

# ------------------------------------------------- spelling variants
my @cases = (
	[ qq{'$P, "\$libdir/pg_stat_statements"'}, 1,
		'quoted $libdir path after this extension' ],
	[ qq{'  $P  ,   pg_stat_statements.so  '}, 1,
		'whitespace and .so suffix after this extension' ],
	[ qq{'"\$libdir/$P", pg_stat_statements'}, 1,
		'this extension as a quoted $libdir path, pgss after it' ],
	[ qq{'"\$libdir/$P.so" , "\$libdir/pg_stat_statements.so"'}, 1,
		'both quoted paths with suffix, pgss after' ],
	[ qq{'"\$libdir/pg_stat_statements", "\$libdir/$P.so"'}, 0,
		'quoted paths, pgss first' ],
	[ qq{'pg_stat_statements.so,$P'}, 0,
		'suffix, no spaces, pgss first' ],
	[ qq{'pg_stat_statements, $P, pg_stat_statements'}, 0,
		'duplicate pgss entry after: first load decides the order' ],
	[ qq{'$P, $P, pg_stat_statements'}, 1,
		'duplicate entry of this extension, pgss after' ],
);
for my $c (@cases)
{
	my ($spl, $want, $name) = @$c;
	($w, $h) = restart_with($spl);
	is($w, $want, "$name: " . ($want ? 'one warning' : 'no warning'));
	is($h, $want, "$name: hint count");
}

# ------------------------------------------------- matcher, any spelling
# Drives the matcher directly with values the server could not load here
# (mixed case only loads on a case-insensitive filesystem), as the same
# function _PG_init uses.
sql('CREATE EXTENSION pssc_guc_test');
my @matcher = (
	[ "PG_STAT_STATEMENT_CONTEXT, pg_stat_statements", 't',
		'upper-case extension, pgss after' ],
	[ "$P, Pg_Stat_Statements", 't', 'mixed-case pgss after' ],
	[ "\"\$libdir/Pg_Stat_Statement_Context.SO\", \"\$libdir/PG_STAT_STATEMENTS.Dylib\"",
		't', 'mixed-case quoted paths and suffixes, pgss after' ],
	[ "PG_STAT_STATEMENTS.SO, Pg_Stat_Statement_Context", 'f',
		'mixed case, pgss first' ],
	[ "$P, pg_stat_statements", 't', 'plain wrong order' ],
	[ "pg_stat_statements, $P", 'f', 'plain documented order' ],
	[ "$P", 'f', 'no pgss' ],
	[ "pg_stat_statements", 'f', 'pgss without this extension' ],
	[ "", 'f', 'empty list' ],
	[ "$P, pg_stat_statements_ext", 'f', 'longer name is not pgss' ],
	[ "$P, pg_stat_statements.sox", 'f', 'unknown suffix is not stripped' ],
);
for my $c (@matcher)
{
	my ($spl, $want, $name) = @$c;
	(my $lit = $spl) =~ s/'/''/g;
	is(sql("SELECT pssc_guc_test_load_order_wrong('$lit')"), $want,
		"matcher: $name");
}

$node->stop;
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash');

done_testing();
