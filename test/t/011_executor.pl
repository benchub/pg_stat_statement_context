# Executor hooks and recording (DESIGN.md §3.2, §3.3, §4.1, §6.4, §6.5,
# §6.8; backlog 20261005-091225-17): src/executor.c records one call and
# the executor time of every recordable statement at ExecutorEnd. Entries
# are read through the TEST-ONLY module test/modules/pssc_store_test
# (pssc_store_test_entries(); the user-facing views are items -20/-21).
#
# Covers: a commented simple-protocol SELECT recorded with its tags, its
# queryid and time equal to pg_stat_statements'; track = top / all, with
# per-(userid, queryid, toplevel) calls equal to pgss's for top-level,
# nested PL/pgSQL and (PG17+: not top level) plan-time folded SQL;
# PL/pgSQL statements inheriting the caller's tags; a parallel query
# counted once (cost-forced and debug_parallel_query/force_parallel_mode);
# a cursor fetched many times counted once; untagged = skip / record;
# enabled = off; backend-local extraction counters flushed into the shared
# header; leading comments of later statements of a multi-statement string
# (also past scan_window, PG18); extended protocol (pgbench -M extended /
# prepared, psql \bind on PG16+) recorded with the tags of the Parse-time
# text. pgss parity checks run where its library is installed (every
# harness image).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('executor');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
$P.extractors = 'sqlcommenter(position=any)'
max_worker_processes = 16
max_parallel_workers = 8
});
$node->start;

my $pkglibdir = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
my $have_pgss = -e "$pkglibdir/pg_stat_statements.so";
note("pg_stat_statements available: " . ($have_pgss ? 'yes' : 'no'));
if ($have_pgss)
{
	# The documented order: pgss first, so this extension's hooks are the
	# outermost (DESIGN.md §3.2 "Load order").
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'pg_stat_statements, $P'\n");
	$node->restart;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');
}

my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
my $pg17 = $vnum >= 170000;
my $pg16 = $vnum >= 160000;
my $force_parallel = $pg16 ? 'debug_parallel_query' : 'force_parallel_mode';

$node->safe_psql('postgres', q{
CREATE EXTENSION pssc_store_test;
-- One table per scenario, so every statement has its own queryid.
CREATE TABLE t_simple(i int);
CREATE TABLE t_untag(i int);
CREATE TABLE t_off(i int);
CREATE TABLE t_nest(i int);
CREATE TABLE t_fold(i int);
CREATE TABLE t_cur(i int);
CREATE TABLE t_stats(i int);
CREATE TABLE t_m1(i int);
CREATE TABLE t_m2(i int);
CREATE TABLE t_m3(i int);
CREATE TABLE t_ext(i int);
CREATE TABLE t_bind(i int);
CREATE TABLE t_par(i int);
CREATE TABLE t_force(i int);
INSERT INTO t_cur SELECT generate_series(1, 10);
INSERT INTO t_par SELECT generate_series(1, 100000);
ANALYZE;

CREATE FUNCTION f_nest() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r bigint;
BEGIN
  SELECT count(*) INTO r FROM t_nest;
  RETURN r;
END $$;

-- Evaluated while planning its caller (constant folding).
CREATE FUNCTION f_fold(int) RETURNS bigint LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE r bigint;
BEGIN
  SELECT count(*) INTO r FROM t_fold;
  RETURN r + $1;
END $$;

-- One executor (one Start, eleven Runs, one End) for the cursor query.
CREATE FUNCTION f_cur() RETURNS int LANGUAGE plpgsql AS $$
DECLARE
  c CURSOR FOR SELECT i FROM t_cur;
  v int;
  n int := 0;
BEGIN
  OPEN c;
  LOOP
    FETCH c INTO v;
    EXIT WHEN NOT FOUND;
    n := n + 1;
  END LOOP;
  CLOSE c;
  RETURN n;
END $$;

-- Recorded entries of this database, summed over buckets.
CREATE VIEW rec AS
  SELECT userid, queryid, toplevel, tags, sum(calls) AS calls,
         sum(total_exec_time) AS total_exec_time
    FROM pssc_store_test_entries()
   WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
   GROUP BY 1, 2, 3, 4;
});

sub sql { return $node->safe_psql('postgres', $_[0]); }

# psql on stdin (statements joined with \; form one simple-protocol query).
sub run
{
	my ($input) = @_;
	my ($out, $err);
	$node->psql('postgres', $input, stdout => \$out, stderr => \$err,
		extra_params => [ '-v', 'ON_ERROR_STOP=1' ]);
	return ($out, $err);
}

# Sets sighup GUCs with ALTER SYSTEM and waits until new sessions see them.
sub set_conf
{
	my (%kv) = @_;
	for my $k (sort keys %kv)
	{
		sql("ALTER SYSTEM SET $P.$k = '$kv{$k}'");
	}
	sql('SELECT pg_reload_conf()');
	for my $k (sort keys %kv)
	{
		$node->poll_query_until('postgres', "SHOW $P.$k", $kv{$k})
		  or die "$P.$k did not become $kv{$k}";
	}
}

# Query identifier of a statement (EXPLAIN VERBOSE; compute_query_id is
# on: _PG_init enables it). Call before reset_all(): EXPLAIN's inner
# statement is itself executed (EXPLAIN-only) and may be recorded.
sub qid
{
	my ($stmt, $opts) = @_;
	$opts = $opts ? ", $opts" : '';
	my $out = sql("EXPLAIN (VERBOSE, COSTS OFF$opts) $stmt");
	$out =~ /Query Identifier: (-?\d+)/
	  or die "no query identifier for $stmt: $out";
	return $1;
}

my $reset_sql = 'SELECT pssc_store_test_reset()'
  . ($have_pgss ? ', pg_stat_statements_reset()' : '');

sub reset_all { sql($reset_sql); }

# Recorded rows of one queryid: "toplevel|tags|calls" lines, sorted.
sub rec_of
{
	my ($qid) = @_;
	return sql(
		"SELECT toplevel, tags, calls FROM rec WHERE queryid = $qid ORDER BY 1, 2::text");
}

sub counter
{
	my ($name) = @_;
	return sql("SELECT $name FROM pssc_store_test_counters()");
}

my %q = (
	simple => qid('SELECT count(*) FROM t_simple'),
	untag => qid('SELECT count(*) FROM t_untag'),
	off => qid('SELECT count(*) FROM t_off'),
	nest => qid('SELECT count(*) FROM t_nest'),
	fold => qid('SELECT count(*) FROM t_fold'),
	cur => qid('SELECT i FROM t_cur'),
	stats => qid('SELECT count(*) FROM t_stats'),
	m1 => qid('SELECT count(*) FROM t_m1'),
	m2 => qid('SELECT count(*) FROM t_m2'),
	m3 => qid('SELECT count(*) FROM t_m3'),
	ext => qid('SELECT count(*) FROM t_ext WHERE i > 0'),
	par => qid('SELECT count(*) FROM t_par'),
	force => qid('SELECT count(*) FROM t_force'),
	caller => qid('SELECT f_nest()'),
);

# ------------------------------------------------- simple protocol, pgss queryid

reset_all();
sql(q{SELECT /*controller='simple'*/ count(*) FROM t_simple});
is(rec_of($q{simple}), 't|{controller,simple}|1',
	'commented simple-protocol SELECT recorded once with its tags');
SKIP:
{
	skip 'pg_stat_statements not installed', 3 unless $have_pgss;
	is( sql(
			q{SELECT queryid FROM pg_stat_statements WHERE query LIKE '%FROM t_simple%'}),
		$q{simple},
		'queryid equals pg_stat_statements\'');
	is( sql(
			qq{SELECT count(*) FROM rec r JOIN pg_stat_statements s USING (userid, queryid, toplevel)
				WHERE r.queryid = $q{simple} AND r.calls = s.calls
				  AND abs(r.total_exec_time - s.total_exec_time) < 1e-9}),
		'1',
		'calls and total_exec_time equal pgss\'s (same queryDesc->totaltime)');
	ok(sql("SELECT total_exec_time > 0 FROM rec WHERE queryid = $q{simple}") eq 't',
		'executor time is measured');
}

# ------------------------------------------------- untagged = skip (default)

reset_all();
sql(q{SELECT count(*) FROM t_untag});
sql(q{SELECT /*controller='tagged'*/ count(*) FROM t_simple});
is(rec_of($q{untag}), '', 'untagged = skip: untagged statement not recorded');
is(rec_of($q{simple}), 't|{controller,tagged}|1', 'untagged = skip: tagged one is');

# ------------------------------------------------- extraction counters flushed

reset_all();
{
	my $before = counter('invalid_tags');
	sql(q{SELECT /*controller='ok',action='%FF'*/ count(*) FROM t_stats});
	# Read from another session: the counter is in the shared header.
	is(counter('invalid_tags') - $before, 1,
		'backend-local extraction counters flushed into the header at ExecutorEnd');
	is(rec_of($q{stats}), 't|{controller,ok}|1', 'and the valid tag is kept');
}

# ------------------------------------------------- untagged = record

set_conf(untagged => 'record');
reset_all();
sql(q{SELECT count(*) FROM t_untag});
is(rec_of($q{untag}), 't|{}|1', 'untagged = record: recorded with an empty tag set');

# ------------------------------------------------- enabled = off

reset_all();
sql(qq{SET $P.enabled = off; SELECT /*controller='off'*/ count(*) FROM t_off;
	SELECT count(*) FROM t_untag});
is(sql("SELECT count(*) FROM rec WHERE queryid IN ($q{off}, $q{untag})"), '0',
	'enabled = off records nothing');
sql(qq{SELECT /*controller='on'*/ count(*) FROM t_off});
is(rec_of($q{off}), 't|{controller,on}|1', 'enabled = on (control) records');

# ------------------------------------------------- track = top / all, pgss parity

my $workload = q{
SELECT /*controller='caller'*/ f_nest();
SELECT /*controller='caller'*/ f_nest();
SELECT /*controller='folder'*/ f_fold(1);
SELECT /*controller='cur'*/ f_cur();
SELECT /*controller='simple'*/ count(*) FROM t_simple;
};

# Rows recorded here but not in pgss, or the other way round (calls
# summed per userid, queryid, toplevel); empty if they agree.
my $parity_sql = q{
SELECT coalesce(r.queryid, s.queryid), coalesce(r.toplevel, s.toplevel),
       r.calls, s.calls
  FROM (SELECT userid, queryid, toplevel, sum(calls) AS calls FROM rec
         GROUP BY 1, 2, 3) r
  FULL JOIN (SELECT userid, queryid, toplevel, calls FROM pg_stat_statements
              WHERE dbid = (SELECT oid FROM pg_database
                             WHERE datname = current_database())) s
       USING (userid, queryid, toplevel)
 WHERE r.calls IS DISTINCT FROM s.calls
 ORDER BY 1, 2
};

for my $track ('top', 'all')
{
	my $pgss_set =
	  $have_pgss ? "SET pg_stat_statements.track = '$track';" : '';
	my ($out, $err) = run(
		"SET $P.track = '$track'; $pgss_set\n$reset_sql;\n$workload\n"
		  . ($have_pgss
			? "SELECT 'parity', count(*) FROM ($parity_sql) d;\n$parity_sql;\n"
			: ''));
	is($err, '', "track = $track: workload ran");

	is(rec_of($q{caller}), "t|{controller,caller}|2",
		"track = $track: top-level caller recorded");
	if ($track eq 'top')
	{
		is(rec_of($q{nest}), '',
			'track = top: nested PL/pgSQL statement not recorded');
		is(rec_of($q{fold}), ($pg17 ? '' : 't|{}|1'),
			'track = top: folded plan-time SQL is top level only before PG17');
		is(rec_of($q{cur}), '', 'track = top: nested cursor not recorded');
	}
	else
	{
		is(rec_of($q{nest}), "f|{controller,caller}|2",
			'track = all: nested PL/pgSQL statement recorded, inheriting the caller\'s tags');
		is(rec_of($q{fold}), ($pg17 ? 'f|{}|1' : 't|{}|1'),
			'track = all: folded plan-time SQL gets its own tags; not top level on PG17+');
		is(rec_of($q{cur}), "f|{controller,cur}|1",
			'track = all: cursor fetched many times counted as one call');
	}
  SKIP:
	{
		skip 'pg_stat_statements not installed', 1 unless $have_pgss;
		my @l = grep { /\S/ } split /\n/, $out;
		my ($parity) = grep { /^parity\|/ } @l;
		is($parity, 'parity|0',
			"track = $track: calls per (userid, queryid, toplevel) equal pgss's")
		  or diag($out);
	}
}

# ------------------------------------------------- parallel query

{
	my $par_settings = q{
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
};
	my $plan = sql("$par_settings EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT count(*) FROM t_par");
	like($plan, qr/Workers Launched: [12]/, 'parallel plan launches workers');

	for my $track ('top', 'all')
	{
		my $pgss_set =
		  $have_pgss ? "SET pg_stat_statements.track = '$track';" : '';
		reset_all();
		my ($out, $err) = run(
			"SET $P.track = '$track'; $pgss_set $par_settings\n"
			  . "SELECT /*controller='par'*/ count(*) FROM t_par;\n"
			  . "SET $force_parallel = on;\n"
			  . "SELECT /*controller='force'*/ count(*) FROM t_force;\n");
		is($err, '', "track = $track: parallel queries ran");
		is(rec_of($q{par}), 't|{controller,par}|1',
			"track = $track: parallel query counted once (workers skipped)");
		is(rec_of($q{force}), 't|{controller,force}|1',
			"track = $track: $force_parallel query counted once");
	  SKIP:
		{
			skip 'pg_stat_statements not installed', 1 unless $have_pgss;
			is( sql(
					"SELECT calls FROM pg_stat_statements WHERE queryid = $q{par}"),
				'1',
				"track = $track: pgss also counts it once");
		}
	}
}

# ------------------------------------------------- multi-statement strings

reset_all();
{
	my $pad = 'x' x 3000;
	my ($out, $err) = run(
		"SELECT length('$pad') \\; /*controller='second'*/ SELECT count(*) FROM t_m2;\n"
		  . "SELECT /*controller='first'*/ count(*) FROM t_m1 \\; SELECT count(*) FROM t_m3;\n"
	);
	is($err, '', 'multi-statement strings ran');
	is(rec_of($q{m2}), 't|{controller,second}|1',
		'leading comment of a later statement kept (past scan_window; PG18 locations)');
	is(rec_of($q{m1}), 't|{controller,first}|1', 'first statement keeps its own comment');
	is(rec_of($q{m3}), 't|{}|1', 'and it is not attributed to the next statement');
}

# ------------------------------------------------- extended protocol

{
	my $script = $node->basedir . '/pgbench_ext.sql';
	open my $fh, '>', $script or die $!;
	print $fh "SELECT /*controller='pgb'*/ count(*) FROM t_ext WHERE i > 0;\n";
	close $fh;
	for my $mode ('extended', 'prepared')
	{
		reset_all();
		my ($stdout, $stderr);
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		IPC::Run::run(
			[ 'pgbench', '-n', '-M', $mode, '-t', '5', '-f', $script, 'postgres' ],
			'>', \$stdout, '2>', \$stderr)
		  or die "pgbench failed: $stderr";
		like($stdout, qr{number of transactions actually processed: 5/5},
			"pgbench -M $mode ran");
		is(rec_of($q{ext}), 't|{controller,pgb}|5',
			"pgbench -M $mode: recorded with the tags of the Parse-time text");
	  SKIP:
		{
			skip 'pg_stat_statements not installed', 1 unless $have_pgss;
			is( sql(
					"SELECT calls FROM pg_stat_statements WHERE queryid = $q{ext}"),
				'5',
				"pgbench -M $mode: queryid and calls equal pgss's");
		}
	}
}

SKIP:
{
	skip 'psql \bind needs PG16+', 2 unless $pg16;
	# A parameter jumbles differently from a constant.
	my $qbind = qid('SELECT count(*) FROM t_bind WHERE i > $1', 'GENERIC_PLAN');
	reset_all();
	my ($out, $err) = run(
		"SELECT /*controller='bind'*/ count(*) FROM t_bind WHERE i > \$1 \\bind 0 \\g\n"
	);
	is($err, '', 'psql \bind ran');
	is(rec_of($qbind), 't|{controller,bind}|1',
		'psql \bind (unnamed extended-protocol statement) recorded with its tags');
}

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
