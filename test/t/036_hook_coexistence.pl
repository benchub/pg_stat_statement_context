# Coexistence with other hook-using extensions (DESIGN.md §3.2, §9;
# docs/maintaining.md; backlog 20261008-065635-7). This extension chains the
# planner (PG17+), executor and utility hooks; this test preloads it next to
# other libraries that install the same hooks, in both orders relative to
# it, with pg_stat_statements first as documented (when installed):
#   auto_explain     (contrib) log_analyze and log_nested_statements on
#   pgaudit          session audit logging of every class
#   pg_hint_plan     planner hints in a leading /*+ ... */ comment
#   pg_stat_monitor  must follow pg_stat_statements (its documented order)
#
# Each order runs in its own database: tagged top-level SELECTs, a tagged
# call of a PL/pgSQL function whose inner SELECT inherits the tags (nested),
# and a tagged utility (ANALYZE). The recorded (tags, toplevel, calls) must
# be exactly the expected ones, every recorded (queryid, toplevel) must have
# pg_stat_statements' calls, and no utility may lose its queryId
# (_info().utility_missing_queryid). The other library's own output must be
# intact too: auto_explain's analyzed plans for each top-level and nested
# statement, pgaudit's audit lines, pg_hint_plan's "used hint" report and
# changed plan (with sqlcommenter and marginalia tag comments in the same
# statement, which must not turn the hint into tags), and pg_stat_monitor's
# calls.
#
# auto_explain is contrib, installed with every harness server. The others
# are optional: a module that is not installed is skipped with a diagnostic
# on stderr, unless PSSC_REQUIRE_MODULES lists it (docker/run-tests.sh sets
# it to what the harness image installed), which makes it a failure.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('coexist');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
$P.extractors = 'sqlcommenter(position=any), marginalia(position=any)'
$P.track = all
pg_stat_statements.track = all
log_min_messages = warning
include_if_exists = 'coexist.conf'
});
$node->start;

my $have_pgss = defined pgss_suffix($node);
my %have;
for my $m (qw(auto_explain pgaudit pg_hint_plan pg_stat_monitor))
{
	$have{$m} = defined module_suffix($node, $m);
	diag("$m is not installed: skipping its coexistence checks")
	  unless $have{$m};
}
diag('pg_stat_statements is not installed: no pgss parity checks')
  unless $have_pgss;

sub logsize { return -s $node->logfile; }

sub log_since
{
	my ($off) = @_;
	my $log = slurp_file($node->logfile);
	return substr($log, $off);
}

sub count_re
{
	my ($text, $re) = @_;
	my $n = () = $text =~ /$re/g;
	return $n;
}

# Restarts with shared_preload_libraries = pgss (when installed), then
# @$libs, plus the extra settings; creates the scenario's database.
sub start_scenario
{
	my ($db, $libs, $conf) = @_;
	my @spl = (($have_pgss ? ('pg_stat_statements') : ()), @$libs);
	my $file = $node->data_dir . '/coexist.conf';
	open my $fh, '>', $file or die "cannot write $file: $!";
	print $fh "shared_preload_libraries = '" . join(', ', @spl) . "'\n";
	print $fh $conf // '';
	close $fh;
	$node->restart;
	is($node->safe_psql('postgres', 'SHOW shared_preload_libraries'),
		join(', ', @spl), "$db: loaded " . join(', ', @spl));

	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, "CREATE EXTENSION $P");
	$node->safe_psql($db, 'CREATE EXTENSION pg_stat_statements')
	  if $have_pgss;
	$node->safe_psql($db, q{
CREATE TABLE coexist_t(i int PRIMARY KEY, pad text);
INSERT INTO coexist_t SELECT g, repeat('x', 100) FROM generate_series(1, 10000) g;
ANALYZE coexist_t;
CREATE TABLE coexist_u(i int);
CREATE FUNCTION f_nested() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  PERFORM count(*) AS nested_marker FROM coexist_t WHERE i > 0;
END $$;
});
}

sub missing_queryid
{
	my ($db) = @_;
	return $node->safe_psql($db,
		"SELECT utility_missing_queryid FROM ${P}_info()");
}

# The common tagged workload; $t prefixes its tags.
sub workload
{
	my ($db, $t) = @_;
	for (1 .. 3)
	{
		$node->safe_psql($db,
			"SELECT count(*) FROM coexist_t WHERE i > 0 /*controller='${t}_top'*/");
	}
	for (1 .. 2)
	{
		$node->safe_psql($db, "SELECT f_nested() /*controller='${t}_nest'*/");
		# Not coexist_t: the setup's untagged ANALYZE of it has the same
		# queryid from PG16.
		$node->safe_psql($db, "ANALYZE coexist_u /*controller='${t}_util'*/");
	}
}

sub workload_rows
{
	my ($t) = @_;
	return (
		qq({"controller": "${t}_nest"}|f|2),
		qq({"controller": "${t}_nest"}|t|2),
		qq({"controller": "${t}_top"}|t|3),
		qq({"controller": "${t}_util"}|t|2));
}

# Checks what this extension recorded in $db against @want (rows of
# tags|toplevel|calls), and against pg_stat_statements.
sub check_recorded
{
	my ($db, $name, @want) = @_;
	my $got = $node->safe_psql($db, qq{
SELECT tags::text, toplevel, sum(calls)
  FROM ${P}_totals
 WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
 GROUP BY 1, 2 ORDER BY 1, 2});
	is($got, join("\n", sort @want), "$name: recorded tags, toplevel and calls");
	check_parity($db, $name, 'pg_stat_statements', '') if $have_pgss;
}

# The recorded rows whose calls differ from those of the same
# (userid, dbid, queryid, toplevel) in $other (a relation with these
# columns), as "controller:other's calls" (none when it has no such row).
sub check_parity
{
	my ($db, $name, $other, $want, $label) = @_;
	$label //= $other;
	is( $node->safe_psql($db, qq{
SELECT string_agg(c.tag || ':' || coalesce(o.calls::text, 'none'), ',' ORDER BY c.tag)
  FROM (SELECT tags->>'controller' AS tag, userid, dbid, queryid, toplevel,
               sum(calls) AS calls
          FROM ${P}_totals
         WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
         GROUP BY 1, 2, 3, 4, 5) c
  LEFT JOIN $other o USING (userid, dbid, queryid, toplevel)
 WHERE o.calls IS DISTINCT FROM c.calls}),
		$want,
		"$name: calls equal those of $label"
		  . ($want eq '' ? '' : " except $want"));
}

# ------------------------------------------------------------ auto_explain
my $ae_conf = q{
auto_explain.log_min_duration = 0
auto_explain.log_analyze = on
auto_explain.log_nested_statements = on
};
SKIP:
{
	skip 'auto_explain is not installed', 1 unless $have{auto_explain};
	for my $c ([ 'ae_first', [ 'auto_explain', $P ] ],
		[ 'ae_last', [ $P, 'auto_explain' ] ])
	{
		my ($db, $libs) = @$c;
		start_scenario($db, $libs, $ae_conf);
		my $off = logsize();
		my $m0 = missing_queryid($db);
		workload($db, $db);
		check_recorded($db, $db, workload_rows($db));
		is(missing_queryid($db) - $m0, 0, "$db: no utility lost its queryId");

		my $log = log_since($off);
		# Analyzed plans: "(actual time=..." or, with timing off, "(actual rows=...".
		is( count_re($log,
				qr/Query Text: SELECT count\(\*\) FROM coexist_t WHERE i > 0 \/\*controller='${db}_top'\*\/\n[^\n]*Aggregate[^\n]*\(actual /
			),
			3,
			"$db: auto_explain logged an analyzed plan of each top-level call");
		is( count_re($log,
				qr/Query Text: SELECT f_nested\(\) \/\*controller='${db}_nest'\*\/\n[^\n]*Result[^\n]*\(actual /
			),
			2,
			"$db: auto_explain logged an analyzed plan of each function call");
		is( count_re($log,
				qr/Query Text: SELECT count\(\*\) AS nested_marker FROM coexist_t WHERE i > 0\n[^\n]*Aggregate[^\n]*\(actual /
			),
			2,
			"$db: auto_explain logged an analyzed plan of each nested statement");
	}
}

# ------------------------------------------------------------------ pgaudit
SKIP:
{
	skip 'pgaudit is not installed', 1 unless $have{pgaudit};
	for my $c ([ 'pa_first', [ 'pgaudit', $P ] ],
		[ 'pa_last', [ $P, 'pgaudit' ] ])
	{
		my ($db, $libs) = @$c;
		start_scenario($db, $libs, "pgaudit.log = 'all'\n");
		my $off = logsize();
		my $m0 = missing_queryid($db);
		workload($db, $db);
		check_recorded($db, $db, workload_rows($db));
		is(missing_queryid($db) - $m0, 0, "$db: no utility lost its queryId");

		my $log = log_since($off);
		is( count_re($log,
				qr/AUDIT: SESSION,\d+,1,READ,SELECT,,,SELECT count\(\*\) FROM coexist_t WHERE i > 0 \/\*controller='${db}_top'\*\//
			),
			3,
			"$db: pgaudit logged each top-level SELECT");
		is( count_re($log,
				qr/AUDIT: SESSION,\d+,\d+,READ,SELECT,,,SELECT count\(\*\) AS nested_marker FROM coexist_t WHERE i > 0,/
			),
			2,
			"$db: pgaudit logged each nested SELECT");
		is( count_re($log,
				qr/AUDIT: SESSION,\d+,1,\w+,ANALYZE,,,ANALYZE coexist_u \/\*controller='${db}_util'\*\//
			),
			2,
			"$db: pgaudit logged each utility");
	}
}

# ------------------------------------------------------------- pg_hint_plan
SKIP:
{
	skip 'pg_hint_plan is not installed', 1 unless $have{pg_hint_plan};
	my $hint = '/*+ SeqScan(coexist_t) */';
	for my $c ([ 'hp_first', [ 'pg_hint_plan', $P ] ],
		[ 'hp_last', [ $P, 'pg_hint_plan' ] ])
	{
		my ($db, $libs) = @$c;
		start_scenario($db, $libs, q{
pg_hint_plan.debug_print = on
pg_hint_plan.message_level = log
});
		# The hint takes effect: an index scan without it, a seq scan with it.
		like(
			$node->safe_psql($db,
				'EXPLAIN (COSTS OFF) SELECT pad FROM coexist_t WHERE i = 5'),
			qr/Index Scan/,
			"$db: index scan without the hint");
		like(
			$node->safe_psql($db,
				"$hint EXPLAIN (COSTS OFF) SELECT pad FROM coexist_t WHERE i = 5"),
			qr/Seq Scan/,
			"$db: seq scan with the hint");

		my $off = logsize();
		my $m0 = missing_queryid($db);
		workload($db, $db);
		# A hint before the statement and a tag comment after it
		# (sqlcommenter, marginalia), a hint followed by a tag comment before
		# the statement, and a hint alone (untagged: not recorded, and the
		# hint is not parsed as tags).
		my @hinted = (
			[ "$hint SELECT pad FROM coexist_t WHERE i = 5 /*controller='${db}_sc'*/", 2 ],
			[ "$hint SELECT i FROM coexist_t WHERE i = 6 /*controller:${db}_mg*/", 1 ],
			[ "$hint /*controller='${db}_pre'*/ SELECT i, pad FROM coexist_t WHERE i = 7", 1 ],
			[ "$hint SELECT pad, i FROM coexist_t WHERE i = 8", 1 ]);
		for my $h (@hinted)
		{
			$node->safe_psql($db, $h->[0]) for 1 .. $h->[1];
		}
		check_recorded(
			$db, $db, workload_rows($db),
			qq({"controller": "${db}_sc"}|t|2),
			qq({"controller": "${db}_mg"}|t|1),
			qq({"controller": "${db}_pre"}|t|1));
		is(missing_queryid($db) - $m0, 0, "$db: no utility lost its queryId");
		# debug_print reports the hints it used, with the statement.
		my $log = log_since($off);
		for my $h (@hinted)
		{
			is( count_re($log,
					qr/pg_hint_plan:\n\tused hint:\n\tSeqScan\(coexist_t\)\n(?:\t[^\n]*\n)*[^\n]*STATEMENT:  \Q$h->[0]\E\n/
				),
				$h->[1],
				"$db: pg_hint_plan used the hint in: $h->[0]");
		}
	}
}

# ---------------------------------------------------------- pg_stat_monitor
# pg_stat_monitor clears the queryId of the utilities it tracks before it
# chains, exactly as pg_stat_statements does (pgsm_ProcessUtility, checked
# with 2.4.0), so it too must be loaded before this extension. Its
# documented order puts pg_stat_statements before it, so pgss runs inside it
# and never sees a utility's queryId: pgss's missing utility rows below are
# pg_stat_monitor's doing, not this extension's. In the other order this
# extension loses the utilities (counted in utility_missing_queryid) and
# records the plannable statements as usual.
my $pgsm_rel = q{(SELECT userid, dbid, queryid, toplevel, sum(calls) AS calls
                    FROM pg_stat_monitor GROUP BY 1, 2, 3, 4)};
SKIP:
{
	skip 'pg_stat_monitor is not installed', 1 unless $have{pg_stat_monitor};

	my $db = 'pm_first';
	start_scenario($db, [ 'pg_stat_monitor', $P ],
		"pg_stat_monitor.pgsm_track = 'all'\n");
	$node->safe_psql($db, 'CREATE EXTENSION pg_stat_monitor');
	my $m0 = missing_queryid($db);
	workload($db, $db);
	my $got = $node->safe_psql($db, qq{
SELECT tags::text, toplevel, sum(calls)
  FROM ${P}_totals
 WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
 GROUP BY 1, 2 ORDER BY 1, 2});
	is($got, join("\n", sort(workload_rows($db))),
		"$db: recorded tags, toplevel and calls");
	is(missing_queryid($db) - $m0, 0, "$db: no utility lost its queryId");
	check_parity($db, $db, $pgsm_rel, '', 'pg_stat_monitor');
	check_parity($db, $db, 'pg_stat_statements', "${db}_util:none")
	  if $have_pgss;

	$db = 'pm_last';
	start_scenario($db, [ $P, 'pg_stat_monitor' ],
		"pg_stat_monitor.pgsm_track = 'all'\n");
	$node->safe_psql($db, 'CREATE EXTENSION pg_stat_monitor');
	$m0 = missing_queryid($db);
	workload($db, $db);
	$got = $node->safe_psql($db, qq{
SELECT tags::text, toplevel, sum(calls)
  FROM ${P}_totals
 WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
 GROUP BY 1, 2 ORDER BY 1, 2});
	is($got, join("\n", sort(grep { !/_util"/ } workload_rows($db))),
		"$db: plannable statements recorded, utilities lost");
	is(missing_queryid($db) - $m0, 2,
		"$db: the utilities are counted in utility_missing_queryid");
	check_parity($db, $db, $pgsm_rel, '', 'pg_stat_monitor');
	check_parity($db, $db, 'pg_stat_statements', '') if $have_pgss;
}

$node->stop;
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash');

done_testing();
