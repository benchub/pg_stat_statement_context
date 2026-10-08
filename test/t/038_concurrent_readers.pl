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
# Needs no TEST-ONLY module, so it also runs against the release build.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';
my $CLIENTS = 6;
my $SECONDS = 10;

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

# The readers loop until $1 seconds have passed and return the first
# violation, or 'ok' and how many rows they checked.
sql(qq{CREATE EXTENSION $P;
CREATE FUNCTION check_activity(secs float8) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE
  deadline timestamptz := clock_timestamp() + secs * interval '1 second';
  r record;
  nrows bigint := 0;
  nlong bigint := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    FOR r IN SELECT pid, state, tags FROM ${P}_activity
             WHERE pid <> pg_backend_pid() LOOP
      nrows := nrows + 1;
      IF r.tags = '{}' OR r.tags = '{"controller": "hot", "action": "hot"}' THEN
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
  RETURN format('ok %s %s', nrows, nlong);
END \$f\$;
CREATE FUNCTION check_stats(secs float8) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE
  deadline timestamptz := clock_timestamp() + secs * interval '1 second';
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
  RETURN format('ok %s %s', nscans, nentries);
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
		[ 'pgbench', '-n', '-c', $CLIENTS, '-j', $CLIENTS, '-T', $SECONDS,
			'-f', $script, 'postgres' ],
		'>', \$bout, '2>', \$berr);
}

# Wait until every writer has a row and the long tag sets are recorded.
$node->poll_query_until('postgres', qq{SELECT (SELECT count(*) FROM ${P}_activity a
                                         JOIN pg_stat_activity s USING (pid)
                                        WHERE s.application_name = 'writer') = $CLIENTS
                                       AND (SELECT count(*) FROM ${P}_totals
                                        WHERE tags->>'controller' <> 'hot') = 50})
  or do { $bench->finish; die "writers did not start: $berr"; };

my $read = 6;
my %out;
my @readers;
for my $f (qw(check_activity check_stats))
{
	$out{$f} = '';
	push @readers, IPC::Run::start(
		[ 'psql', '-X', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
			'-c', "SELECT $f($read)" ],
		'>', \$out{$f}, '2>', \$out{$f});
}
$_->finish for @readers;
$bench->finish or die "pgbench failed: $berr";
like($bout, qr/number of transactions actually processed: [1-9]/, 'pgbench ran');

chomp(my $act = $out{check_activity});
like($act, qr/^ok \d+ \d+$/, "activity rows are each from one publication ($act)");
my ($along) = $act =~ /^ok \d+ (\d+)$/;
ok(($along // 0) >= 100,
	"the activity reader checked writers' rows (" . ($along // 'none') . " long rows)");

chomp(my $s = $out{check_stats});
like($s, qr/^ok \d+ \d+$/, "each entry's sum(calls) equals calls_total ($s)");
my ($scans, $sent) = $s =~ /^ok (\d+) (\d+)$/;
ok(($scans // 0) >= 10 && ($sent // 0) >= 10 * 51,
	"the stats reader scanned the entries repeatedly (" . ($scans // 'none') . " scans)");

# The writers have stopped: the same invariant, and the hot entry saw all
# the hot statements pgbench ran.
my ($ntx) = $bout =~ m{number of transactions actually processed: (\d+)};
is(sql(qq{SELECT count(*) FROM (SELECT 1 FROM $P GROUP BY userid, dbid, queryid, toplevel, tags
          HAVING sum(calls) <> min(calls_total)) t}), 0, 'quiescent: sum(calls) = calls_total');
cmp_ok(sql(qq{SELECT calls_total FROM ${P}_totals WHERE tags->>'controller' = 'hot'}), '>=', $ntx,
	'the hot entry counts every completed transaction');

$node->stop;
done_testing();
