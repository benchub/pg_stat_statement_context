# Execution lifecycle and pg_stat_statements parity (DESIGN.md §3.2, §3.3,
# §6.3-§6.9, §9; backlog 20261005-091225-22).
#
# With untagged = record and both extensions freshly reset, a mixed
# workload is run by two roles in two databases, and every
# (userid, dbid, queryid, toplevel) of pg_stat_statement_context_totals
# (summed over tags) is compared with pg_stat_statements:
#   - calls must be equal;
#   - total_exec_time of plannable statements must be equal (both hooks
#     read the same queryDesc->totaltime, InstrEndLoop is idempotent; the
#     only allowed difference is double rounding of differently associated
#     sums: 1e-9 relative + 1e-9 ms);
#   - total_exec_time of utility statements is measured by each hook around
#     its own chained call. With the documented load order this
#     extension's hook encloses pgss's, so ours >= pgss's, and the excess
#     is what runs between the two clock reads: pgss's pgss_store() of the
#     utility and our frame bookkeeping, a bounded per-call cost. Allowed:
#     1% of pgss's time + $UTIL_SLACK_MS per call (default 2 ms;
#     PSSC_TEST_UTILITY_SLACK_MS overrides it, e.g. under Valgrind).
#     A scheduling stall between the two clock reads lands in the
#     enclosing hook's time only, and on a loaded host (seen under --assert
#     in a laptop Docker VM: 5.6 ms for one RELEASE SAVEPOINT) exceeds any
#     slack tight enough to matter. So when the only problems of a
#     comparison are utility times beyond that bound in the enclosing hook's
#     direction (ours above pgss's; pgss's above ours with the wrong load
#     order) and the configuration's other invariants (coverage, lifecycle,
#     presence, utility_missing_queryid) hold on that first run, its
#     workload is run once more (after a reset) and the second run is
#     final, with the same bound. A stall is rare and random, so it does
#     not hit twice; a systematic error (a per-call offset, double
#     counting) fails again. Widening the bound or tolerating "a few
#     outliers" instead would hide double counting: only the few utilities
#     slower than the slack (EXPLAIN ANALYZE) reveal it.
# The workload: prepared statements over the extended protocol (named
# statements Parsed once and Bound/Executed many times, through the raw
# protocol driver below so that every version is covered; unnamed ones
# re-Parsed; psql \bind on PG16+), with the stale-comment behavior of §6.3;
# overlapping and suspended portals (Execute with a row limit, PL/pgSQL FOR
# loops, SQL cursors), portals and cursors never run, closed early, left
# open until COMMIT (recorded then, at top level, as pgss does) or dropped by
# ROLLBACK; failed portals; SPI errors caught in PL/pgSQL followed by
# successful work; failing top-level statements (§6.9: counted by neither);
# explicit ROLLBACK and savepoints; COMMIT/ROLLBACK inside procedures and
# DO (also in a FOR loop, and a failing CALL after a COMMIT); triggers,
# folded functions, EXPLAIN (ANALYZE), PREPARE/EXECUTE, multi-statement
# strings; parallel queries (cost-forced and
# debug_parallel_query/force_parallel_mode).
#
# Configurations: the same track/track_utility in both extensions
# ({top, all} x {on, off}, exact parity everywhere, including PG14-16 with
# track_utility = off: nesting mirrors pgss, §6.7, item -18); settings that
# differ (the side that tracks less is a subset of the other, with equal
# rows); and the wrong shared_preload_libraries order (pgss zeroes the
# utility queryId first: plannable rows still equal, every utility pgss
# recorded is missing here and counted in utility_missing_queryid instead).
#
# A utility kept in a plan cache (a named extended-protocol statement, a
# PL/pgSQL statement) is counted by pgss only on its first execution per
# backend: pgss_ProcessUtility zeroes pstmt->queryId of the cached
# PlannedStmt, so later executions reach both hooks with queryId 0. This
# extension cannot record them either (parity holds) and counts them in
# utility_missing_queryid, in either load order; $lost below.
#
# Exception: releases before upstream commit 8700851352a8 ("Avoid
# unnecessary plancache revalidation of utility statements", bug #18059;
# first in 14.10, 15.5 and 16.1, so 14.0-14.9, 15.0-15.4 and 16.0) also
# revalidated plain utilities, re-running parse analysis (so computing a
# fresh queryId) when the saved search_path no longer matches. p_tx's first
# CREATE TEMP TABLE creates the backend's temp namespace, which changes the
# active search_path, so its second run is re-analyzed and counted by both.
# $replans_utilities detects this at runtime through the bug's other
# symptom, independently of either extension.
#
# No statement that pgss tracks is deliberately left out by this extension
# when the settings agree (DESIGN.md §6.7: EXECUTE/PREPARE, and DEALLOCATE
# before PG17, are excluded by both).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

# ------------------------------------------------- raw v3 protocol driver
#
# Just enough of the frontend/backend protocol (trust authentication over
# the node's Unix socket) to send Parse/Bind/Execute(row limit)/Close/Sync
# and simple Query messages in any order, which neither psql (before 17)
# nor pgbench can do.
package RawPq;

use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);

sub connect
{
	my ($class, $node, $db, $user) = @_;
	my $path = $node->host . '/.s.PGSQL.' . $node->port;
	my $sock = IO::Socket::UNIX->new(Type => SOCK_STREAM, Peer => $path)
	  or die "cannot connect to $path: $!";
	binmode $sock;
	my $self = bless { sock => $sock, out => '' }, $class;
	my $body = pack('N', 196608) . "user\0$user\0database\0$db\0\0";
	$self->_write(pack('N', length($body) + 4) . $body);
	my @ev = $self->_until_ready;
	die "startup failed: @ev" if grep { /^E:/ } @ev;
	return $self;
}

sub _write
{
	my ($self, $buf) = @_;
	my $off = 0;
	while ($off < length $buf)
	{
		my $n = syswrite($self->{sock}, $buf, length($buf) - $off, $off);
		die "write failed: $!" unless defined $n;
		$off += $n;
	}
}

sub _read_n
{
	my ($self, $n) = @_;
	my $buf = '';
	while (length($buf) < $n)
	{
		my $r = sysread($self->{sock}, $buf, $n - length($buf), length($buf));
		die "read failed: $!" unless defined $r;
		die 'connection closed by the server' if $r == 0;
	}
	return $buf;
}

# Backend messages up to ReadyForQuery, as short event strings:
# 1/2/3 (Parse/Bind/CloseComplete), D (DataRow), s (PortalSuspended),
# n (NoData), C:<tag>, E:<message>, Z.
sub _until_ready
{
	my ($self) = @_;
	my @ev;
	while (1)
	{
		my ($t, $len) = unpack('a N', $self->_read_n(5));
		my $body = $len > 4 ? $self->_read_n($len - 4) : '';
		if ($t eq 'E')
		{
			my %f = map { substr($_, 0, 1) => substr($_, 1) }
			  grep { length } split /\0/, $body;
			push @ev, "E:$f{M}";
		}
		elsif ($t eq 'C')
		{
			$body =~ s/\0$//;
			push @ev, "C:$body";
		}
		elsif ($t eq 'R')
		{
			my $code = unpack('N', $body);
			die "authentication method $code not supported" if $code != 0;
		}
		elsif ($t eq 'Z')
		{
			push @ev, 'Z';
			return @ev;
		}
		elsif ($t =~ /^[123Dsn]$/)
		{
			push @ev, $t;
		}
		# S, K, T, N, I: ignored
	}
}

sub _msg
{
	my ($self, $type, $body) = @_;
	$self->{out} .= $type . pack('N', length($body) + 4) . $body;
	return $self;
}

sub parse { my ($s, $name, $sql) = @_; $s->_msg('P', "$name\0$sql\0" . pack('n', 0)); }

sub bind
{
	my ($s, $portal, $stmt) = @_;
	$s->_msg('B', "$portal\0$stmt\0" . pack('n n n', 0, 0, 0));
}
sub execute { my ($s, $portal, $max) = @_; $s->_msg('E', "$portal\0" . pack('N', $max // 0)); }
sub close_portal { my ($s, $portal) = @_; $s->_msg('C', "P$portal\0"); }

# Send what is queued plus Sync; returns the events up to ReadyForQuery.
sub sync
{
	my ($s) = @_;
	$s->_msg('S', '');
	$s->_write($s->{out});
	$s->{out} = '';
	return join(' ', $s->_until_ready);
}

sub query
{
	my ($s, $sql) = @_;
	$s->_msg('Q', "$sql\0");
	$s->_write($s->{out});
	$s->{out} = '';
	return join(' ', $s->_until_ready);
}

sub finish
{
	my ($s) = @_;
	$s->_write('X' . pack('N', 4));
	close $s->{sock};
}

package main;

my $P = 'pg_stat_statement_context';
my $UTIL_SLACK_MS = $ENV{PSSC_TEST_UTILITY_SLACK_MS} // 2;

my $node = PostgreSQL::Test::Cluster->new('lifecycle');
$node->init;
$node->start;
if (!defined pgss_suffix($node))
{
	plan skip_all => 'pg_stat_statements not installed';
}
# The documented order: pgss first, so this extension's hooks are the
# outermost (DESIGN.md §3.2 "Load order").
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_stat_statements, $P'
$P.extractors = 'sqlcommenter(position=any)'
$P.untagged = 'record'
max_worker_processes = 16
max_parallel_workers = 8
});
$node->restart;

my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
my $pg16 = $vnum >= 160000;
my $force_parallel = $pg16 ? 'debug_parallel_query' : 'force_parallel_mode';
my $super = $node->safe_psql('postgres', 'SELECT current_user');

sub sql { return $node->safe_psql('postgres', $_[0]); }

$node->safe_psql('postgres', qq{
CREATE EXTENSION pg_stat_statements;
CREATE EXTENSION $P;
CREATE ROLE r_app LOGIN;
CREATE DATABASE db2;
});

# Objects of the workload, in both databases. Every statement of the
# functions has its own shape, so that it has its own queryid.
my $schema = q{
CREATE TABLE lt(i int);
INSERT INTO lt SELECT generate_series(1, 100);
CREATE TABLE lpar(i int);
INSERT INTO lpar SELECT generate_series(1, 100000);
CREATE TABLE llog(i int);
CREATE TABLE ltrig(i int);
ANALYZE;

CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO llog VALUES (NEW.i * 1000);
  RETURN NEW;
END $$;
CREATE TRIGGER ltrig_after AFTER INSERT ON ltrig
  FOR EACH ROW EXECUTE FUNCTION trg();

-- SPI errors caught, followed by successful work.
CREATE FUNCTION f_caught() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  BEGIN
    SELECT count(*) / 0 INTO n FROM lt;            -- fails in the executor
  EXCEPTION WHEN division_by_zero THEN
    NULL;
  END;
  BEGIN
    INSERT INTO llog SELECT max(i) FROM lt;        -- ends, then rolled back
    PERFORM 10 / (i - i) FROM lt;                  -- fails
  EXCEPTION WHEN division_by_zero THEN
    NULL;
  END;
  SELECT count(*) INTO n FROM lt WHERE i <> 0;     -- succeeds
  RETURN n;
END $$;

-- PL/pgSQL cursors: never fetched, closed early, left open (closed at
-- transaction end), overlapping suspended FOR loops, a failed cursor.
CREATE FUNCTION f_cursors() RETURNS int LANGUAGE plpgsql AS $$
DECLARE
  c_never CURSOR FOR SELECT i FROM lt WHERE i < 10;
  c_early CURSOR FOR SELECT i FROM lt WHERE i <= 20;
  c_open CURSOR FOR SELECT i FROM lt WHERE i >= 30;
  c_fail refcursor;
  v int;
  n int := 0;
  r record;
  s record;
BEGIN
  OPEN c_never;
  CLOSE c_never;
  OPEN c_early;
  FETCH c_early INTO v;
  CLOSE c_early;
  OPEN c_open;
  FETCH c_open INTO v;
  FOR r IN SELECT i FROM lt WHERE i > 60 LOOP
    FOR s IN SELECT -i AS j FROM lt WHERE i < 3 LOOP
      n := n + 1;
    END LOOP;
  END LOOP;
  BEGIN
    OPEN c_fail FOR SELECT 10 / (i - 5) FROM lt;
    LOOP
      FETCH c_fail INTO v;
      EXIT WHEN NOT FOUND;
    END LOOP;
  EXCEPTION WHEN division_by_zero THEN
    n := n + 1000;
  END;
  RETURN n;
END $$;

CREATE FUNCTION f_par() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM lpar WHERE i % 3 = 0;
  RETURN n;
END $$;

-- Evaluated while planning its caller (constant folding).
CREATE FUNCTION f_fold(int) RETURNS bigint LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM lt WHERE i > 90 AND i < 1000;
  RETURN n + $1;
END $$;

CREATE PROCEDURE p_tx() LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  INSERT INTO llog VALUES (1);
  ROLLBACK;
  INSERT INTO llog VALUES (2);
  COMMIT;
  CREATE TEMP TABLE ltx_tmp(i int);
  DROP TABLE ltx_tmp;
  FOR r IN SELECT g FROM generate_series(1, 3) g LOOP
    INSERT INTO llog VALUES (100 + r.g);
    COMMIT;
  END LOOP;
  ROLLBACK;
END $$;

CREATE PROCEDURE p_outer() LANGUAGE plpgsql AS $$
BEGIN
  CALL p_tx();
  COMMIT;
END $$;

-- Fails after a COMMIT: the CALL is not counted, the committed INSERT is.
CREATE PROCEDURE p_err() LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO llog SELECT min(i) FROM lt;
  COMMIT;
  PERFORM 10 / (i - i) FROM lt LIMIT 1;
END $$;

CREATE PROCEDURE p_small() LANGUAGE plpgsql AS $$
BEGIN
  UPDATE llog SET i = i WHERE i = -1;
END $$;

GRANT ALL ON lt, lpar, llog, ltrig TO r_app;
};
$node->safe_psql($_, $schema) for ('postgres', 'db2');

# Does the plan cache re-analyze a cached plain utility once the temp
# namespace appears in the search_path (see the header)? Where it does, the
# cached SET TRANSACTION is revalidated under a snapshot after the COMMIT
# and fails as in bug #18059.
my $replans_utilities = do {
	$node->safe_psql('postgres', q{
CREATE PROCEDURE p_probe() LANGUAGE plpgsql AS $$
BEGIN
  COMMIT;
  SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
END $$;});
	my ($ret, $out, $err) = $node->psql('postgres',
		'CALL p_probe(); CREATE TEMP TABLE probe_tmp(); CALL p_probe();');
	$node->safe_psql('postgres', 'DROP PROCEDURE p_probe()');
	die "plan cache probe failed unexpectedly: $err"
	  if $ret != 0
	  && $err !~ /SET TRANSACTION ISOLATION LEVEL must be called before any query/;
	$ret != 0 ? 1 : 0;
};
note("plan cache re-analyzes cached utilities after a search_path change: "
	  . ($replans_utilities ? 'yes' : 'no'));

my %dbid = map {
	$_ => sql("SELECT oid FROM pg_database WHERE datname = '$_'")
} ('postgres', 'db2');
my %uid = map { $_ => sql("SELECT oid FROM pg_roles WHERE rolname = '$_'") }
  ($super, 'r_app');

# Query identifier of a statement in database postgres (EXPLAIN VERBOSE;
# compute_query_id is on: _PG_init enables it). Call before a reset.
sub qid
{
	my ($stmt) = @_;
	my $out = sql("EXPLAIN (VERBOSE, COSTS OFF) $stmt");
	$out =~ /Query Identifier: (-?\d+)/
	  or die "no query identifier for $stmt: $out";
	return $1;
}

my %q = (
	caught_fail => qid('SELECT count(*) / 0 FROM lt'),
	caught_insert => qid('INSERT INTO llog SELECT max(i) FROM lt'),
	caught_ok => qid('SELECT count(*) FROM lt WHERE i <> 0'),
	never => qid('SELECT i FROM lt WHERE i < 10'),
	early => qid('SELECT i FROM lt WHERE i <= 20'),
	open => qid('SELECT i FROM lt WHERE i >= 30'),
	for_outer => qid('SELECT i FROM lt WHERE i > 60'),
	for_inner => qid('SELECT -i AS j FROM lt WHERE i < 3'),
	cur_fail => qid('SELECT 10 / (i - 5) FROM lt'),
	top_fail => qid('SELECT 10 % (i - 50) FROM lt'),
	err_insert => qid('INSERT INTO llog SELECT min(i) FROM lt'),
	par => qid('SELECT count(*) FROM lpar'),
	force => qid('SELECT sum(i) FROM lpar WHERE i % 7 = 0'),
	par_fn => qid('SELECT count(*) FROM lpar WHERE i % 3 = 0'),
	x_stale => qid('SELECT count(*) FROM lt WHERE i > 1'),
	x_susp => qid('SELECT i FROM lt WHERE i < 50 ORDER BY i'),
	x_early => qid('SELECT i, i FROM lt'),
	x_never => qid('SELECT i, i, i FROM lt'),
	x_sync => qid('SELECT i::text FROM lt'),
	x_fail => qid('SELECT i, 10 / (i - 30) FROM lt'),
	x_abort => qid('SELECT i::bigint FROM lt'),
	x_rb => qid('SELECT i::numeric FROM lt'),
);

# Sets sighup GUCs (full names) with ALTER SYSTEM and waits until new
# sessions see them.
sub set_conf
{
	my (%kv) = @_;
	for my $k (sort keys %kv)
	{
		sql("ALTER SYSTEM SET $k = '$kv{$k}'");
	}
	sql('SELECT pg_reload_conf()');
	for my $k (sort keys %kv)
	{
		$node->poll_query_until('postgres', "SHOW $k", $kv{$k})
		  or die "$k did not become $kv{$k}";
	}
}

sub reset_all
{
	sql("SELECT pg_stat_statements_reset(), ${P}_reset()");
}

# ------------------------------------------------- the workload

# psql (simple protocol, \; joins statements into one query string); runs
# on after errors. Expected errors are counted by run_workload().
my $psql_workload = qq{
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
SELECT /*controller='par'*/ count(*) FROM lpar;
SELECT /*controller='parfn'*/ f_par();
SET $force_parallel = on;
SELECT /*controller='force'*/ sum(i) FROM lpar WHERE i % 7 = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT max(i) FROM lpar;
RESET $force_parallel;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
RESET parallel_leader_participation;
} . q{
SELECT /*controller='caught'*/ f_caught();
SELECT /*controller='cursors'*/ f_cursors();
SELECT /*controller='topfail'*/ 10 % (i - 50) FROM lt;
BEGIN;
INSERT INTO llog VALUES (10);
SELECT /*controller='rolledback'*/ f_cursors();
SAVEPOINT s1;
INSERT INTO llog VALUES (11);
SELECT 10 % (i - 50) FROM lt;
SELECT 1;
ROLLBACK TO SAVEPOINT s1;
UPDATE llog SET i = i + 1 WHERE i = 10;
ROLLBACK;
BEGIN;
SAVEPOINT s2;
SELECT /*controller='released'*/ f_cursors();
RELEASE SAVEPOINT s2;
COMMIT;
BEGIN;
SAVEPOINT s3;
SELECT /*controller='subrolledback'*/ f_cursors();
ROLLBACK TO SAVEPOINT s3;
COMMIT;
BEGIN;
DECLARE ca CURSOR FOR SELECT i FROM lt ORDER BY i;
DECLARE cb CURSOR FOR SELECT i + i FROM lt;
DECLARE cn CURSOR FOR SELECT i - i FROM lt;
FETCH 2 FROM ca;
FETCH 1 FROM cb;
FETCH 3 FROM ca;
CLOSE cb;
MOVE 5 IN ca;
COMMIT;
BEGIN;
DECLARE ch CURSOR WITH HOLD FOR SELECT i FROM lt WHERE i BETWEEN 1 AND 4;
FETCH 1 FROM ch;
COMMIT;
FETCH ALL FROM ch;
CLOSE ch;
BEGIN;
DECLARE cf CURSOR FOR SELECT 10 / (i - 3) FROM lt;
FETCH 1 FROM cf;
FETCH 5 FROM cf;
ROLLBACK;
CALL p_tx() /*controller='ptx'*/;
CALL p_outer();
CALL p_err();
BEGIN;
CALL p_tx();
ROLLBACK;
DO $$ BEGIN INSERT INTO llog VALUES (20); COMMIT; INSERT INTO llog VALUES (21); ROLLBACK; END $$;
INSERT INTO ltrig VALUES (1), (2);
SELECT f_fold(1);
EXPLAIN (COSTS OFF) SELECT count(*) FROM lt WHERE i >= 5;
PREPARE pq AS SELECT /*controller='prep'*/ count(*) FROM lt WHERE i < 3;
EXECUTE pq /*controller='exec'*/;
EXECUTE pq;
DEALLOCATE pq;
SELECT 1 \; SELECT count(*) FROM llog \; CALL p_small();
CREATE TEMP TABLE ltmp AS SELECT i FROM lt WHERE i <> 4;
DROP TABLE ltmp;
SET work_mem = '8MB';
RESET work_mem;
};
$psql_workload .= q{
SELECT /*controller='bind'*/ count(*) FROM lt WHERE i > $1 \bind 3 \g
} if $pg16;

# Errors the psql workload raises on purpose, per run.
my $psql_errors = 6;

# Extended protocol (one session); returns a description of what the
# server answered, compared with $x_expected.
sub x_workload
{
	my ($c) = @_;
	my @r;

	# A named statement Parsed once with one comment and Bound/Executed many
	# times: every execution gets the Parse-time tags (§6.3), even after
	# statements with other comments ran. Re-Parsing gets fresh tags.
	$c->parse('st', "SELECT /*controller='first'*/ count(*) FROM lt WHERE i > 1");
	push @r, $c->sync;
	for (1 .. 3)
	{
		$c->bind('', 'st')->execute('', 0);
		push @r, $c->sync;
	}
	push @r, $c->query("SELECT /*controller='second'*/ 1");
	$c->bind('', 'st')->execute('', 0);
	push @r, $c->sync;
	$c->parse('', "SELECT /*controller='fresh'*/ count(*) FROM lt WHERE i > 1")
	  ->bind('', '')->execute('', 0);
	push @r, $c->sync;

	# Overlapping suspended portals in a transaction block: one run in
	# chunks, one closed early, one never executed (ended at COMMIT).
	push @r, $c->query('BEGIN');
	$c->parse('s_susp', "SELECT /*controller='susp'*/ i FROM lt WHERE i < 50 ORDER BY i");
	$c->parse('s_early', "SELECT /*controller='early'*/ i, i FROM lt");
	$c->parse('s_never', "SELECT /*controller='never'*/ i, i, i FROM lt");
	$c->bind('pa', 's_susp')->bind('pb', 's_early')->bind('pn', 's_never');
	$c->execute('pa', 5)->execute('pb', 3)->execute('pa', 5)
	  ->execute('pb', 3)->close_portal('pb')->execute('pa', 0);
	push @r, $c->sync;
	push @r, $c->query('COMMIT');

	# A portal suspended when the implicit transaction ends at Sync.
	$c->parse('', "SELECT /*controller='sync'*/ i::text FROM lt")
	  ->bind('', '')->execute('', 2);
	push @r, $c->sync;

	# A failed portal: suspended once, then fails while fetching.
	$c->parse('s_fail', "SELECT /*controller='xfail'*/ i, 10 / (i - 30) FROM lt");
	$c->bind('pf', 's_fail')->execute('pf', 5)->execute('pf', 0);
	push @r, $c->sync;

	# A suspended portal dropped by the abort of a failing transaction,
	# and one dropped by an explicit ROLLBACK.
	push @r, $c->query('BEGIN');
	$c->parse('s_abort', "SELECT /*controller='abort'*/ i::bigint FROM lt");
	$c->bind('pz', 's_abort')->execute('pz', 1);
	$c->bind('pf2', 's_fail')->execute('pf2', 0);
	push @r, $c->sync;
	push @r, $c->query('ROLLBACK');
	push @r, $c->query('BEGIN');
	$c->parse('s_rb', "SELECT /*controller='rb'*/ i::numeric FROM lt");
	$c->bind('pr', 's_rb')->execute('pr', 2);
	push @r, $c->sync;
	push @r, $c->query('ROLLBACK');

	# A utility over the extended protocol.
	$c->parse('', 'CREATE TEMP TABLE xtmp(i int)')->bind('', '')->execute('', 0)
	  ->parse('', 'DROP TABLE xtmp')->bind('', '')->execute('', 0);
	push @r, $c->sync;

	# A named utility statement executed three times: pgss counts only the
	# first execution (see the header), and so does this extension.
	$c->parse('s_util', "SET statement_timeout = '1min'");
	$c->bind('', 's_util')->execute('', 0) for (1 .. 3);
	push @r, $c->sync;
	return join("\n", @r);
}

my $x_expected = join("\n",
	'1 Z',
	('2 D C:SELECT 1 Z') x 3,
	'D C:SELECT 1 Z',
	'2 D C:SELECT 1 Z',
	'1 2 D C:SELECT 1 Z',
	'C:BEGIN Z',
	'1 1 1 2 2 2 ' . 'D ' x 5 . 's ' . 'D ' x 3 . 's ' . 'D ' x 5 . 's '
	  . 'D ' x 3 . 's 3 ' . 'D ' x 39 . 'C:SELECT 39 Z',
	'C:COMMIT Z',
	'1 2 D D s Z',
	'1 2 ' . 'D ' x 5 . 's ' . 'D ' x 24 . 'E:division by zero Z',
	'C:BEGIN Z',
	'1 2 D s 2 ' . 'D ' x 29 . 'E:division by zero Z',
	'C:ROLLBACK Z',
	'C:BEGIN Z',
	'1 2 D D s Z',
	'C:ROLLBACK Z',
	'1 2 C:CREATE TABLE 1 2 C:DROP TABLE Z',
	'1 2 C:SET 2 C:SET 2 C:SET Z');

# Executions of cached utilities that reach the hooks with queryId 0 (see
# the header), for both load orders and the same track_utility in both
# extensions: with track_utility on, the named SET of x_workload loses 2 of
# its 3 executions in each of the 2 extended sessions, and with track = all
# CREATE/DROP TABLE ltx_tmp of p_tx, run twice per psql session (CALL p_tx
# and CALL p_outer; the CALL inside BEGIN fails first), lose the second run
# in each of the 3 psql sessions; where $replans_utilities, the second
# CREATE is re-analyzed and counted instead, so only the DROP loses it.
sub lost
{
	my ($track, $tu) = @_;
	return 0 if $tu ne 'on';
	return 2 * 2 + ($track eq 'all' ? (2 - $replans_utilities) * 3 : 0);
}

# Runs the whole workload: psql as both roles in postgres and as the
# superuser in db2, the extended-protocol session as the superuser in
# postgres and as r_app in db2.
sub run_workload
{
	my ($label) = @_;
	for my $s ([ 'postgres', $super ], [ 'postgres', 'r_app' ], [ 'db2', $super ])
	{
		my ($db, $user) = @$s;
		my ($out, $err);
		$node->psql($db, $psql_workload,
			stdout => \$out, stderr => \$err, on_error_stop => 0,
			extra_params => [ '-U', $user ]);
		my @errs = $err =~ /^.*ERROR:.*$/mg;
		is(scalar(@errs), $psql_errors,
			"$label: psql workload as $user in $db raised only the expected errors")
		  or diag($err);
	}
	for my $s ([ 'postgres', $super ], [ 'db2', 'r_app' ])
	{
		my $c = RawPq->connect($node, @$s);
		is(x_workload($c), $x_expected,
			"$label: extended-protocol workload as $s->[1] in $s->[0]");
		$c->finish;
	}
}

# ------------------------------------------------- comparison with pgss

# Plannable statements, by pgss's text (leading comments skipped). Others
# are utilities, whose time each extension measures itself. A plannable
# statement whose text pgss keeps as its parent's (EXPLAIN's inner
# statement) is checked as a utility, a weaker check that still holds.
# A statement run by EXECUTE is kept with its PREPARE text.
my $plannable_re =
  q{'^\s*(/\*([^*]|\*+[^*/])*\*+/\s*)*(prepare\s+\w+(\s*\([^)]*\))?\s+as\s+(/\*([^*]|\*+[^*/])*\*+/\s*)*)?(select|insert|update|delete|with|values|merge)\M'};

my $cmp_sql = qq{
WITH o AS (SELECT userid, dbid, queryid, toplevel, sum(calls) AS calls,
                  sum(total_exec_time) AS t
             FROM ${P}_totals GROUP BY 1, 2, 3, 4),
     s AS (SELECT userid, dbid, queryid, toplevel, calls, total_exec_time AS t,
                  query ~* $plannable_re AS plannable,
                  regexp_replace(left(query, 70), '\\s+', ' ', 'g') AS query
             FROM pg_stat_statements)
SELECT userid, dbid, queryid, toplevel, coalesce(o.calls, -1),
       coalesce(s.calls, -1), coalesce(o.t, -1), coalesce(s.t, -1),
       coalesce(s.plannable, false), coalesce(s.query, '')
  FROM o FULL JOIN s USING (userid, dbid, queryid, toplevel)
 ORDER BY 1, 2, 3, 4
};

sub fetch_rows
{
	my @rows;
	for my $line (split /\n/, sql($cmp_sql))
	{
		my @f = split /\|/, $line, 10;
		push @rows,
		  {
			key => "$f[0]/$f[1]/$f[2]/$f[3]",
			userid => $f[0],
			dbid => $f[1],
			ours => $f[4],
			theirs => $f[5],
			ot => $f[6],
			st => $f[7],
			plannable => $f[8] eq 't',
			query => $f[9]
		  };
	}
	return @rows;
}

# Time check for a row present in both (see the header). Returns the
# problem, if any, and whether it is a utility time difference a stall can
# cause (see compare()): the enclosing hook's time above the other's, ours
# with the documented load order, pgss's with the wrong one.
sub time_problem
{
	my ($r, $enclosing) = @_;
	my ($o, $s) = ($r->{ot}, $r->{st});
	my $eps = 1e-9 * ($s > 1 ? $s : 1);
	if ($r->{plannable})
	{
		return abs($o - $s) <= $eps ? () : ("plannable time $o != $s", 0);
	}
	return ("utility time $o < pgss's $s", 0)
	  if $enclosing && $o < $s - $eps;
	my $max = 0.01 * $s + $UTIL_SLACK_MS * $r->{ours};
	return abs($o - $s) <= $max ? ()
	  : ("utility time $o vs pgss's $s (allowed difference $max)",
		$enclosing ? $o > $s : $s > $o);
}

# Compares this extension's rows with pgss's.
#   mode equal:  same keys, same calls, times as above.
#   mode subset: ours (if $sub eq 'ours') or pgss's keys are a subset of
#                the other's, with equal calls and times on shared keys.
#   mode wrong:  wrong load order; ours a subset, every missing row a
#                utility; $c->{missing_calls} has the calls of the
#                missing rows.
# $checks->($c) returns the configuration's other invariants, evaluated on
# the current workload's state, as [got, expected, name, diag] each.
# If the only problems are utility times beyond the bound in the direction
# a stall can cause (see the header and time_problem()), and the coverage
# checks (rows shared, both roles, both databases) and every $checks
# invariant hold, the workload is reset and run once more, and the second
# run is final. Otherwise the first run's results are reported: a rerun
# never replaces a first-run failure other than a stall.
sub compare
{
	my ($label, $mode, $sub, $checks) = @_;
	my $c = compare_rows($mode, $sub);
	my @k = $checks ? $checks->($c) : ();
	my $covered = sub {
		my ($x) = @_;
		return $x->{shared} >= 40
		  && join(',', sort keys %{ $x->{users} }) eq join(',', sort values %uid)
		  && join(',', sort keys %{ $x->{dbs} }) eq join(',', sort values %dbid);
	};
	if (@{ $c->{bad} } && $c->{stall_only} && $covered->($c)
		&& !grep { $_->[0] ne $_->[1] } @k)
	{
		diag("$label: utility time above the bound only, running the workload again:\n"
			  . join("\n", @{ $c->{bad} }));
		reset_all();
		run_workload("$label (rerun)");
		$c = compare_rows($mode, $sub);
		@k = $checks ? $checks->($c) : ();
	}
	ok($c->{shared} >= 40,
		"$label: workload produced rows recorded by both ($c->{shared})");
	is_deeply([ sort keys %{ $c->{users} } ], [ sort values %uid ],
		"$label: rows of both roles compared");
	is_deeply([ sort keys %{ $c->{dbs} } ], [ sort values %dbid ],
		"$label: rows of both databases compared");
	is(scalar(@{ $c->{bad} }), 0,
		"$label: per-(userid, dbid, queryid, toplevel) calls and total_exec_time equal pgss's"
	) or diag(join("\n", @{ $c->{bad} }));
	for my $k (@k)
	{
		is($k->[0], $k->[1], $k->[2]) or (defined $k->[3] && diag($k->[3]));
	}
}

sub compare_rows
{
	my ($mode, $sub) = @_;
	my @rows = fetch_rows();
	my (@bad, $shared, $missing_calls);
	$shared = $missing_calls = 0;
	my $stall_only = 1;
	my (%users, %dbs);
	for my $r (@rows)
	{
		my $desc = "$r->{key} ours=$r->{ours} pgss=$r->{theirs} "
		  . "t=$r->{ot}/$r->{st} '$r->{query}'";
		if ($r->{ours} >= 0 && $r->{theirs} >= 0)
		{
			$shared++;
			$users{ $r->{userid} } = 1;
			$dbs{ $r->{dbid} } = 1;
			if ($r->{ours} != $r->{theirs})
			{
				push @bad, "calls differ: $desc";
				$stall_only = 0;
				next;
			}
			my ($p, $excess) = time_problem($r, $mode ne 'wrong');
			if ($p)
			{
				push @bad, "$p: $desc";
				$stall_only &&= $excess;
			}
		}
		elsif ($r->{ours} >= 0)
		{
			unless ($mode eq 'subset' && $sub eq 'pgss')
			{
				push @bad, "only here: $desc";
				$stall_only = 0;
			}
		}
		else
		{
			if ($mode eq 'wrong')
			{
				if ($r->{plannable})
				{
					push @bad, "plannable only in pgss: $desc";
					$stall_only = 0;
				}
				else
				{
					$missing_calls += $r->{theirs};
				}
			}
			elsif (!($mode eq 'subset' && $sub eq 'ours'))
			{
				push @bad, "only in pgss: $desc";
				$stall_only = 0;
			}
		}
	}
	return {
		bad => \@bad,
		stall_only => $stall_only,
		shared => $shared,
		users => \%users,
		dbs => \%dbs,
		missing_calls => $missing_calls
	};
}

# "toplevel:ours/pgss" of one queryid in database postgres, both users.
sub calls_of
{
	my ($qid) = @_;
	return sql(qq{
SELECT coalesce(string_agg(tl || ':' || coalesce(o, 0) || '/' || coalesce(s, 0), ',' ORDER BY tl), '')
  FROM (SELECT CASE WHEN toplevel THEN 't' ELSE 'f' END AS tl, sum(calls) AS o
          FROM ${P}_totals WHERE queryid = $qid AND dbid = $dbid{postgres}
         GROUP BY 1) a
  FULL JOIN
       (SELECT CASE WHEN toplevel THEN 't' ELSE 'f' END AS tl, sum(calls) AS s
          FROM pg_stat_statements WHERE queryid = $qid AND dbid = $dbid{postgres}
         GROUP BY 1) b USING (tl)});
}

# Rows of one queryid in database postgres by tag value: "value:calls".
sub tags_of
{
	my ($qid) = @_;
	return sql(qq{
SELECT string_agg(coalesce(tags->>'controller', '-') || ':' || calls, ','
                  ORDER BY tags->>'controller')
  FROM (SELECT tags, sum(calls) AS calls FROM ${P}_totals
         WHERE queryid = $qid AND dbid = $dbid{postgres} GROUP BY 1) x});
}

# Lifecycle facts (database postgres: 2 psql runs, 1 extended run), as
# compare() checks.
sub lifecycle_checks
{
	my ($label, $track, $tu, $wrong) = @_;
	my @c;
	my $all = $track eq 'all';
	my $nested = sub { $all ? "f:$_[0]/$_[0]" : '' };
	# A CALL child is nested only when pgss counts the CALL as a level:
	# always on PG17+, on PG14-16 only when it tracks utilities (§6.7).
	my $in_call = sub {
		($vnum >= 170000 || $tu eq 'on') ? $nested->($_[0]) : "t:$_[0]/$_[0]";
	};

	push @c, [calls_of($q{caught_fail}), '', "$label: failing SPI query counted by neither"];
	push @c, [calls_of($q{caught_insert}), $nested->(2),
		"$label: SPI query that ended before a caught error counted (rolled back)"];
	push @c, [calls_of($q{caught_ok}), $nested->(2),
		"$label: successful SPI work after caught errors counted"];
	# f_cursors runs 4 times per psql run: in autocommit, in a rolled-back
	# transaction, in a released savepoint, in a rolled-back savepoint.
	push @c, [calls_of($q{never}), $nested->(8), "$label: cursor never fetched counted"];
	push @c, [calls_of($q{early}), $nested->(8), "$label: cursor closed early counted"];
	push @c, [calls_of($q{open}), ($all ? 't:4/4' : ''),
		"$label: cursor left open counted at COMMIT as top level, as pgss, and not when rolled back"
	];
	push @c, [calls_of($q{for_outer}), $nested->(8),
		"$label: suspended FOR-loop portal counted once per loop"];
	push @c, [calls_of($q{for_inner}), $nested->(8 * 40),
		"$label: overlapping inner FOR-loop portals counted"];
	push @c, [calls_of($q{cur_fail}), '', "$label: failed cursor counted by neither"];
	push @c, [calls_of($q{top_fail}), '', "$label: failing top-level statement counted by neither"];
	push @c, [calls_of($q{err_insert}), $in_call->(2),
		"$label: work committed by a procedure that then fails counted"];
	push @c, [calls_of($q{par}), 't:2/2', "$label: parallel query counted once"];
	push @c, [calls_of($q{force}), 't:2/2', "$label: $force_parallel query counted once"];
	push @c, [calls_of($q{par_fn}), $nested->(2), "$label: parallel SPI query counted once"];

	push @c, [tags_of($q{x_stale}), 'first:4,fresh:1',
		"$label: named statement executions keep the Parse-time tags (§6.3)"];
	push @c, [calls_of($q{x_stale}), 't:5/5', "$label: and pgss counts them the same"];
	push @c, [calls_of($q{x_susp}), 't:1/1', "$label: portal run in chunks counted once"];
	push @c, [calls_of($q{x_early}), 't:1/1', "$label: portal closed early counted"];
	push @c, [calls_of($q{x_never}), 't:1/1', "$label: portal never executed counted at COMMIT"];
	push @c, [calls_of($q{x_sync}), 't:1/1', "$label: portal suspended at Sync counted"];
	push @c, [calls_of($q{x_fail}), '', "$label: failed portals counted by neither"];
	push @c, [calls_of($q{x_abort}), '', "$label: portal dropped by an abort counted by neither"];
	push @c, [calls_of($q{x_rb}), '', "$label: portal dropped by ROLLBACK counted by neither"];

	# Cached utilities: only the first execution per backend (see $lost).
	my $util_qid = sub {
		sql(qq{SELECT coalesce(max(queryid), 0) FROM pg_stat_statements
                WHERE dbid = $dbid{postgres} AND query LIKE '$_[0]%'});
	};
	my ($x_util, $ltx, $ltx_drop) = ($util_qid->('SET statement\_timeout'),
		$util_qid->('CREATE TEMP TABLE ltx\_tmp'),
		$util_qid->('DROP TABLE ltx\_tmp'));
	if ($tu eq 'on')
	{
		my $o = $wrong ? 0 : 1;
		# Runs counted per session: the first, plus the re-analyzed second
		# where $replans_utilities (see the header).
		my $n = 2 * (1 + $replans_utilities);
		push @c, [calls_of($x_util), "t:$o/1",
			"$label: named utility executed 3 times counted once by both"];
		push @c, [calls_of($ltx), ($all ? "f:" . $o * $n . "/$n" : ''),
			"$label: PL/pgSQL utility run twice per session counted once per session by both, "
			  . "twice where the plan cache re-analyzes it"];
		push @c, [calls_of($ltx_drop), ($all ? "f:" . 2 * $o . "/2" : ''),
			"$label: PL/pgSQL utility cached after the search_path change counted once per session by both"
		];
	}
	else
	{
		push @c, ["$x_util/$ltx/$ltx_drop", '0/0/0', "$label: pgss tracked no utility either"];
	}
	return @c;
}

sub missing_queryid
{
	return sql("SELECT utility_missing_queryid FROM ${P}_info()");
}

# Workers really run (cost-forced settings).
like(
	sql(q{SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT count(*) FROM lpar}),
	qr/Workers Launched: [12]/,
	'parallel plan launches workers');

# ------------------------------------------------- reference keys
#
# Taken in the equal all/on configuration, where both sides record
# everything: every top-level utility key, and every key of statements
# nested in functions called from top-level SELECTs (their nesting does not
# depend on utility settings, unlike CALL children on PG14-16). With
# differing settings each side must record exactly those its own GUCs
# allow, so a side that followed the other's settings fails.
my (%ref_util, %ref_nested, @present_rows);
my @nested_qids = map { $q{$_} } qw(never early for_outer for_inner caught_ok par_fn);

# Keys recorded with track_utility off are executor work under a utility's
# text (on PG18 a DECLARE's cursor query runs under the DECLARE's queryId)
# and are dropped from %ref_util; equal configs run top/on, top/off, all/on,
# all/off.
my %not_util;

sub snapshot_reference_keys
{
	my ($tu) = @_;
	my %nq = map { $_ => 1 } @nested_qids;
	for my $r (fetch_rows())
	{
		my (undef, undef, $qid, $tl) = split m{/}, $r->{key};
		if ($tu eq 'off')
		{
			$not_util{ $r->{key} } = 1;
			next;
		}
		$ref_util{ $r->{key} } = 1 if $tl eq 't' && !$r->{plannable};
		$ref_nested{ $r->{key} } = 1 if $tl eq 'f' && $nq{$qid};
	}
	delete @ref_util{ keys %not_util };
}

# Number of keys of %$ref present on each side: "ours/pgss".
sub present
{
	my ($ref) = @_;
	my ($o, $s) = (0, 0);
	@present_rows = ();
	for my $r (fetch_rows())
	{
		next unless $ref->{ $r->{key} };
		$o++ if $r->{ours} > 0;
		$s++ if $r->{theirs} > 0;
		push @present_rows, "$r->{key} ours=$r->{ours} pgss=$r->{theirs} '$r->{query}'";
	}
	return "$o/$s";
}

# ------------------------------------------------- same settings: exact parity

for my $track ('top', 'all')
{
	for my $tu ('on', 'off')
	{
		my $label = "track = $track, track_utility = $tu (both)";
		set_conf(
			"$P.track" => $track,
			"$P.track_utility" => $tu,
			'pg_stat_statements.track' => $track,
			'pg_stat_statements.track_utility' => $tu);
		reset_all();
		run_workload($label);
		compare(
			$label, 'equal', undef,
			sub {
				return (lifecycle_checks($label, $track, $tu),
					[ missing_queryid(), lost($track, $tu),
						"$label: only re-executed cached utilities arrived without a queryid" ]);
			});
		snapshot_reference_keys($tu);
	}
}

# ------------------------------------------------- differing settings

ok(keys(%ref_util) >= 20 && keys(%ref_nested) >= 12,
	'reference keys: ' . scalar(keys %ref_util) . ' top-level utilities, '
	  . scalar(keys %ref_nested) . ' nested statements');

for my $c (
	[ 'all', 'on', 'top', 'off', 'pgss' ],
	[ 'top', 'off', 'all', 'on', 'ours' ],
	[ 'all', 'off', 'all', 'on', 'ours' ],
	[ 'top', 'on', 'top', 'off', 'pgss' ])
{
	my ($track, $tu, $ptrack, $ptu, $sub) = @$c;
	my $label = "ours $track/$tu, pgss $ptrack/$ptu";
	set_conf(
		"$P.track" => $track,
		"$P.track_utility" => $tu,
		'pg_stat_statements.track' => $ptrack,
		'pg_stat_statements.track_utility' => $ptu);
	reset_all();
	run_workload($label);
	# Each side records exactly what its own settings allow.
	my $nu = scalar(keys %ref_util);
	my $nn = scalar(keys %ref_nested);
	compare(
		$label, 'subset', $sub,
		sub {
			my $pu = present(\%ref_util);
			my $du = join("\n", @present_rows);
			my $pn = present(\%ref_nested);
			my $dn = join("\n", @present_rows);
			return (
				[ $pu, ($tu eq 'on' ? $nu : 0) . '/' . ($ptu eq 'on' ? $nu : 0),
					"$label: top-level utilities recorded by the side(s) with track_utility on only",
					$du ],
				[ $pn, ($track eq 'all' ? $nn : 0) . '/' . ($ptrack eq 'all' ? $nn : 0),
					"$label: nested statements recorded by the side(s) with track = all only",
					$dn ]);
		});
}

# ------------------------------------------------- wrong load order

$node->append_conf('postgresql.conf',
	"shared_preload_libraries = '$P, pg_stat_statements'\n");
$node->restart;
for my $track ('top', 'all')
{
	my $label = "wrong load order, track = $track, track_utility = on (both)";
	set_conf(
		"$P.track" => $track,
		"$P.track_utility" => 'on',
		'pg_stat_statements.track' => $track,
		'pg_stat_statements.track_utility' => 'on');
	reset_all();
	run_workload($label);
	# (reset_all() also reset the counter.)
	compare(
		$label, 'wrong', undef,
		sub {
			my ($c) = @_;
			my $missing = $c->{missing_calls};
			return (
				[ $missing > 0 ? 1 : 0, 1,
					"$label: pgss recorded utilities this extension could not" ],
				[ missing_queryid(), $missing + lost($track, 'on'),
					"$label: every utility missing here, and every re-executed cached utility, was counted in utility_missing_queryid" ],
				lifecycle_checks($label, $track, 'on', 1));
		});
}

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
