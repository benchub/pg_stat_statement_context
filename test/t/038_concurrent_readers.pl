# Readers see consistent rows while writers run (DESIGN.md §5.4, §6.11, §9;
# backlog 20261008-065635-6). pgbench clients run tagged statements while
# two sessions read concurrently and check invariants that hold under
# concurrency:
#   - pg_stat_statement_context_activity: each row's tags come from a single
#     publication. Every writer statement carries one value twice
#     (controller = action, a 9-digit number repeated 40 times), so a row
#     torn between two publications shows different or malformed values
#     (the slot's change-counter retry loop, src/activity.c);
#   - pg_stat_statement_context: with no expiry (bucket_interval = 1 day),
#     each entry's sum(calls) over its live buckets equals calls_total (the
#     entry is copied under its spinlock, src/store.c pssc_store_foreach()).
#     This is not an invariant once buckets have expired.
# The writers run until both readers have finished (pgbench is then
# terminated, and must have been running then and die of that SIGTERM; -T
# only bounds a stuck run), and each reader proves that it
# overlapped them: the hot entry's calls_total advanced between the start
# and the end of its checks, and the activity reader saw writers' rows in
# state 'active'.
# Needs no TEST-ONLY module, so it also runs against the release build.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use POSIX qw(WIFSIGNALED WTERMSIG SIGTERM);

my $P = 'pg_stat_statement_context';
my $CLIENTS = 6;
my $READ_SECONDS = 6;

my $node = PostgreSQL::Test::Cluster->new('readers');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
max_connections = 30
autovacuum = off
$P.save = off
$P.bucket_interval = '1d'
$P.max_tag_value_len = 400
$P.max_tagset_bytes = 1024
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# The readers loop for $1 seconds and return the first violation, or 'ok',
# how many rows they checked, and the hot entry's calls_total before and
# after their checks.
sql(qq{CREATE EXTENSION $P;
CREATE FUNCTION hot_calls() RETURNS bigint LANGUAGE sql AS \$f\$
  SELECT coalesce(sum(calls_total), 0)::bigint FROM ${P}_totals
  WHERE tags->>'controller' = 'hot'
\$f\$;
CREATE FUNCTION check_activity(secs float8) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE
  deadline timestamptz := clock_timestamp() + secs * interval '1 second';
  hot0 bigint := hot_calls();
  r record;
  nrows bigint := 0;
  nlong bigint := 0;
  nactive bigint := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    FOR r IN SELECT pid, state, tags FROM ${P}_activity
             WHERE pid <> pg_backend_pid() LOOP
      nrows := nrows + 1;
      IF r.tags = '{}' THEN
        CONTINUE;
      END IF;
      IF r.state = 'active' THEN
        nactive := nactive + 1;
      END IF;
      IF r.tags = '{"controller": "hot", "action": "hot"}' THEN
        CONTINUE;
      END IF;
      IF r.tags IS NULL OR r.tags->>'controller' IS NULL
         OR r.tags->>'controller' !~ '^([0-9]{9})\\1{39}\$'
         OR r.tags->>'action' IS DISTINCT FROM r.tags->>'controller'
         OR (SELECT count(*) FROM jsonb_object_keys(r.tags)) <> 2 THEN
        RETURN format('torn activity row: pid %s state %s tags %s', r.pid, r.state, r.tags);
      END IF;
      nlong := nlong + 1;
    END LOOP;
  END LOOP;
  RETURN format('ok %s %s %s %s %s', nrows, nlong, nactive, hot0, hot_calls());
END \$f\$;
CREATE FUNCTION check_stats(secs float8) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE
  deadline timestamptz := clock_timestamp() + secs * interval '1 second';
  hot0 bigint := hot_calls();
  r record;
  nscans bigint := 0;
  nentries bigint := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    nscans := nscans + 1;
    FOR r IN SELECT queryid, tags, sum(calls) AS calls, min(calls_total) AS lo,
                    max(calls_total) AS hi, count(*) AS nbuckets
             FROM $P GROUP BY userid, dbid, queryid, toplevel, tags LOOP
      nentries := nentries + 1;
      IF r.calls <> r.lo OR r.lo <> r.hi THEN
        RETURN format('torn entry: queryid %s tags %s: sum(calls) %s, calls_total %s..%s over %s buckets',
                      r.queryid, r.tags, r.calls, r.lo, r.hi, r.nbuckets);
      END IF;
    END LOOP;
  END LOOP;
  RETURN format('ok %s %s %s %s', nscans, nentries, hot0, hot_calls());
END \$f\$;});

# Each transaction publishes one of 50 long tag sets (controller = action,
# 360 bytes each) and then the one hot entry, which all clients update.
my $v = ':v' x 40;
my $script = $node->basedir . '/writers.sql';
open my $fh, '>', $script or die "open $script: $!";
print $fh qq{\\set v random(100000000, 100000049)
SELECT 1 /*controller='$v',action='$v'*/;
SELECT 2 /*controller='hot',action='hot'*/;
};
close $fh;

my ($bout, $berr) = ('', '');
my $bench;
{
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	local $ENV{PGAPPNAME} = 'writer';
	$bench = IPC::Run::start(
		[ 'pgbench', '-n', '-c', $CLIENTS, '-j', $CLIENTS, '-T', 600,
			'-f', $script, 'postgres' ],
		'>', \$bout, '2>', \$berr);
}
# Whether pgbench is still running. IPC::Run's pumpable() is no test: it is
# true while the output pipes are open, even after the child has exited.
# reap_nb() waitpid()s it without blocking, recording its status.
sub writers_running
{
	$bench->reap_nb;
	return scalar $bench->_running_kids;
}
# Stops pgbench if it still runs and returns its wait status, and any
# error from finish().
sub stop_writers
{
	return unless $bench;
	$bench->signal('TERM') if writers_running();
	eval { $bench->finish; };
	my $err = $@;
	my $status = eval { $bench->full_result };
	$bench = undef;
	return ($status, $err);
}
# A test that dies midway leaves no pgbench behind; keep the exit status.
END { local $?; stop_writers(); }

# Startup barrier: the writers run until stopped, so this wait does not
# shorten the readers' window. Every writer has a row and the 50 long tag
# sets are recorded.
$node->poll_query_until('postgres', qq{SELECT (SELECT count(*) FROM ${P}_activity a
                                         JOIN pg_stat_activity s USING (pid)
                                        WHERE s.application_name = 'writer') = $CLIENTS
                                       AND (SELECT count(*) FROM ${P}_totals
                                        WHERE tags->>'controller' <> 'hot') = 50})
  or die "writers did not start: $berr";

my %out;
my @readers;
for my $f (qw(check_activity check_stats))
{
	$out{$f} = '';
	push @readers, IPC::Run::start(
		[ 'psql', '-X', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
			'-c', "SELECT $f($READ_SECONDS)" ],
		'>', \$out{$f}, '2>', \$out{$f},
		IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));
}
$_->finish for @readers;
ok(writers_running(), 'the writers ran until both readers finished')
  or diag "pgbench had exited: $bout $berr";
my ($bstatus, $berror) = stop_writers();
ok(defined $bstatus && $bstatus =~ /^\d+$/ && WIFSIGNALED($bstatus)
	  && WTERMSIG($bstatus) == SIGTERM && $berror eq '',
	'pgbench ended only through our SIGTERM')
  or diag 'pgbench wait status ' . ($bstatus // 'unknown') . ", finish error '$berror': $bout $berr";
$node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_activity WHERE application_name = 'writer'})
  or die 'writers did not exit';

chomp(my $act = $out{check_activity});
like($act, qr/^ok( \d+){5}$/, "activity rows are each from one publication ($act)");
my ($arows, $along, $aactive, $ahot0, $ahot1) = $act =~ /^ok (\d+) (\d+) (\d+) (\d+) (\d+)$/;
ok(($along // 0) >= 100 && ($aactive // 0) >= 10,
	"the activity reader saw writers' rows (" . ($along // 'none') . ' long, '
	  . ($aactive // 'none') . ' active)');
ok(($ahot1 // 0) > ($ahot0 // 0) + 1000,
	'writes ran during the activity checks (hot calls_total '
	  . ($ahot0 // 'none') . ' -> ' . ($ahot1 // 'none') . ')');

chomp(my $s = $out{check_stats});
like($s, qr/^ok( \d+){4}$/, "each entry's sum(calls) equals calls_total ($s)");
my ($scans, $sent, $shot0, $shot1) = $s =~ /^ok (\d+) (\d+) (\d+) (\d+)$/;
ok(($scans // 0) >= 10 && ($sent // 0) >= 10 * 51,
	"the stats reader scanned the entries repeatedly (" . ($scans // 'none') . " scans)");
ok(($shot1 // 0) > ($shot0 // 0) + 1000,
	'writes ran during the stats checks (hot calls_total '
	  . ($shot0 // 'none') . ' -> ' . ($shot1 // 'none') . ')');

# The writers have stopped: the same invariant.
is(sql(qq{SELECT count(*) FROM (SELECT 1 FROM $P GROUP BY userid, dbid, queryid, toplevel, tags
          HAVING sum(calls) <> min(calls_total)) t}), 0, 'quiescent: sum(calls) = calls_total');

$node->stop;
done_testing();
