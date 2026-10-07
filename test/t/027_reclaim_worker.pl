# Optional background worker reclaiming dead entries (DESIGN.md §5.2, §5.3,
# §8; backlog 20261005-091225-34). With pg_stat_statement_context.
# reclaim_worker = on, a worker without a database connection wakes every
# reclaim_worker_interval, raises current_bucket to the clock as readers do,
# and, if the watermark moved since its last pass, removes every dead entry
# (no live slot) under the exclusive lock: reclaimed_entries counts them, but
# it is not an eviction pass (dealloc unchanged), it never evicts a live entry
# and never decays usage. With the default (off) no worker is registered and
# dead entries stay until an insert into a full table reclaims them (§5.3).
# Driven through the TEST-ONLY module test/modules/pssc_store_test with its
# shared debug clock pinned; between making entries dead and checking the
# result the test only reads counters that do not move current_bucket, so
# whatever is reclaimed was reclaimed by the worker.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';
my $W = "$P reclaim worker";

my $node = PostgreSQL::Test::Cluster->new('reclaim_worker');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
$P.max_entries = 100
$P.bucket_count = 2
});
$node->start;
$node->safe_psql('postgres', "CREATE EXTENSION $P");
$node->safe_psql('postgres', 'CREATE EXTENSION pssc_store_test');
$node->safe_psql('postgres', "ALTER SYSTEM SET $P.reclaim_worker = on");
$node->safe_psql('postgres', "ALTER SYSTEM SET $P.reclaim_worker_interval = '100ms'");
$node->restart;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub rec { return sql("SELECT pssc_store_test_record($_[0])"); }
sub counters
{
	my @cols = qw(entries max_entries dealloc evicted_entries invalid_tags
	  dropped_tags regex_compile_failures heuristic_scans
	  utility_missing_queryid dropped_records stats_reset shmem_bytes keysize
	  entrysize bucket_count max_tagset_bytes force_collisions hash_entries
	  reclaimed_entries);
	my @v = split /\|/, sql('SELECT * FROM pssc_store_test_counters()'), -1;
	my %c;
	@c{@cols} = @v;
	return \%c;
}
# "entries dealloc reclaimed_entries evicted_entries dropped_records"; reads
# the header only (pssc_store_get_counters() does not move current_bucket).
sub evict_state
{
	my $c = counters();
	return "$c->{entries} $c->{dealloc} $c->{reclaimed_entries} $c->{evicted_entries} "
	  . $c->{dropped_records};
}
sub watermark { return sql('SELECT current_bucket FROM pssc_store_test_buckets()'); }
# Reset, then pin the debug clock in the middle of a bucket beyond any
# bucket seen so far (current_bucket never decreases); returns that bucket.
sub fresh
{
	sql('SELECT pssc_store_test_reset()');
	my $b = sql('SELECT reader_bucket + 1 FROM pssc_store_test_buckets()');
	pin($b);
	return $b;
}
sub pin
{
	my ($b) = @_;
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($b)
	         + ((interval_us / 2) || ' microseconds')::interval)
	       FROM pssc_store_test_buckets()});
}
sub fill
{
	my ($from, $to) = @_;
	is(sql(qq{SELECT count(*) FROM generate_series($from, $to) q
	          WHERE pssc_store_test_record(q) NOT IN ('inserted', 'updated')}), 0,
		"recorded queryids $from..$to");
}
sub keys_present
{
	return sql(q{SELECT string_agg(queryid::text, ',' ORDER BY queryid) FROM
	             (SELECT DISTINCT queryid FROM pssc_store_test_evict_slots()) e});
}
# "queryid:usage,..." from the eviction array (does not move current_bucket).
sub usages
{
	return sql(q{SELECT string_agg(queryid || ':' || usage, ',' ORDER BY queryid)
	             FROM pssc_store_test_evict_slots()});
}
# Polls evict_state() (no other statement in between) until it equals $want.
sub wait_state
{
	my ($want, $desc) = @_;
	my $got;
	for (1 .. 1200)
	{
		$got = evict_state();
		last if $got eq $want;
		usleep(50_000);
	}
	is($got, $want, $desc);
}
sub info
{
	return sql(q{SELECT entries || ' ' || dealloc || ' ' || reclaimed_entries || ' '
	             || evicted_entries FROM pg_stat_statement_context_info()});
}
# PIDs of this node's postmaster children whose process title names the worker.
sub worker_pids
{
	my $pm = $node->{_pid};
	my @pids;
	if (-d '/proc/self')
	{
		for my $dir (glob '/proc/[0-9]*')
		{
			my ($pid) = $dir =~ m{(\d+)$};
			open my $fh, '<', "$dir/stat" or next;
			my $stat = <$fh>;
			close $fh;
			next unless defined $stat;
			my @f = split ' ', substr($stat, rindex($stat, ')') + 2);
			next unless $f[1] == $pm;
			open $fh, '<', "$dir/cmdline" or next;
			local $/;
			my $cmd = <$fh> // '';
			close $fh;
			push @pids, $pid if $cmd =~ /\Q$W\E/;
		}
	}
	else
	{
		for my $line (split /\n/, `ps -A -o pid= -o ppid= -o command=`)
		{
			my ($pid, $ppid, $cmd) = $line =~ /^\s*(\d+)\s+(\d+)\s+(.*)$/ or next;
			push @pids, $pid if $ppid == $pm && $cmd =~ /\Q$W\E/;
		}
	}
	return scalar @pids;
}

# ---------------------------------------------------------------- enabled
is(sql(qq{SELECT string_agg(name || '=' || setting || ':' || boot_val || ':' || context,
                            ',' ORDER BY name)
          FROM pg_settings WHERE name IN ('$P.reclaim_worker', '$P.reclaim_worker_interval')}),
	"$P.reclaim_worker=on:off:postmaster,$P.reclaim_worker_interval=100:10000:sighup",
	'GUCs: reclaim_worker (postmaster, default off), reclaim_worker_interval (sighup, ms, default 10s)');
is(sql("SELECT unit FROM pg_settings WHERE name = '$P.reclaim_worker_interval'"), 'ms',
	'reclaim_worker_interval is in milliseconds');
{
	my $n = 0;
	for (1 .. 600) { $n = worker_pids(); last if $n; usleep(50_000); }
	is($n, 1, 'exactly one reclaim worker process runs');
	my $started = 0;
	for (1 .. 600)
	{
		$started = slurp_file($node->logfile) =~ /\Q$W\E started/;
		last if $started;
		usleep(50_000);
	}
	ok($started, 'the worker logs its start');
}

{
	my $b = fresh();
	fill(1, 5);
	usleep(600_000);	# several worker wake-ups
	is(evict_state(), '5 0 0 0 0', 'live entries are left alone');

	pin($b + 1);
	fill(4, 6);		# 4..6 last written in $b + 1, 1..3 in $b
	my $before = usages();
	usleep(300_000);
	is(evict_state(), '6 0 0 0 0', 'nothing dead yet: window [b, b+1]');

	# No query traffic from here on: only header reads.
	pin($b + 2);	# window [b+1, b+2]: 1..3 dead
	wait_state('3 0 3 0 0',
		'the worker reclaimed the 3 dead entries without query traffic; no eviction pass (dealloc 0), nothing evicted');
	is(watermark(), $b + 2, 'the worker advanced current_bucket to the clock');
	is(keys_present(), '4,5,6', 'the live entries remain');
	is(usages(), join(',', grep { !/^[123]:/ } split /,/, $before),
		'the survivors\' usage did not decay (reclaim-only pass)');
	is(sql('SELECT pssc_store_test_check_invariants()'), 3, 'invariants hold');
	is(info(), '3 0 3 0', '_info(): entries dropped to 3, reclaimed_entries 3, dealloc 0');

	pin($b + 4);
	wait_state('0 0 6 0 0', 'everything expired: the worker reclaimed the rest');
	is(info(), '0 0 6 0', '_info(): entries 0');

	# A full table with dead entries: the worker makes room before any insert.
	$b = fresh();
	fill(1, 100);
	pin($b + 2);
	wait_state('0 0 100 0 0', 'full table of dead entries emptied by the worker');
	is(rec(101), 'inserted', 'the next insert finds room');
	is(evict_state(), '1 0 100 0 0', 'no eviction pass was needed');
}

# SIGHUP: the interval is reloaded (and a reload wakes the worker).
{
	sql("ALTER SYSTEM SET $P.reclaim_worker_interval = '1h'");
	$node->reload;
	usleep(1_000_000);
	my $b = fresh();
	fill(1, 3);
	pin($b + 2);
	usleep(1_500_000);
	is(evict_state(), '3 0 0 0 0', 'after reload to 1h the worker sleeps: nothing reclaimed');
	sql("ALTER SYSTEM SET $P.reclaim_worker_interval = '100ms'");
	$node->reload;
	wait_state('0 0 3 0 0', 'the next reload wakes it and it reclaims');
	is(sql("SHOW $P.reclaim_worker_interval"), '100ms', 'interval reloaded');
}

# The postmaster GUC needs a restart.
{
	sql("ALTER SYSTEM SET $P.reclaim_worker = off");
	$node->reload;
	is(sql("SELECT pending_restart FROM pg_settings WHERE name = '$P.reclaim_worker'"), 't',
		'reclaim_worker change waits for a restart');
	is(worker_pids(), 1, 'the worker keeps running until then');
}

# ---------------------------------------------------------------- disabled
# The default: no worker; dead entries wait for an insert into a full table.
sql("ALTER SYSTEM RESET $P.reclaim_worker");
sql("ALTER SYSTEM RESET $P.reclaim_worker_interval");
$node->stop;
my $logpos = -s $node->logfile;
$node->start;
is(sql("SELECT setting FROM pg_settings WHERE name = '$P.reclaim_worker'"), 'off',
	'reclaim_worker is off by default');
usleep(1_000_000);
is(worker_pids(), 0, 'off: no worker process');
unlike(substr(slurp_file($node->logfile), $logpos), qr/\Q$W\E/, 'off: the worker never starts');
{
	my $b = fresh();
	fill(1, 100);
	pin($b + 3);
	usleep(1_500_000);
	is(evict_state(), '100 0 0 0 0', 'off: dead entries stay without traffic');
	is(info(), '100 0 0 0', 'off: _info() (a reader) does not reclaim either');
	is(rec(101), 'inserted', 'an insert into the full table succeeds');
	is(evict_state(), '1 1 100 0 0',
		'off: the insert\'s eviction pass reclaimed the dead entries, as before');
	is(sql('SELECT pssc_store_test_check_invariants()'), 1, 'invariants hold');
}

$node->stop;
unlike(slurp_file($node->logfile),
	qr/PANIC|TRAP|terminated by signal|server process .* was terminated|"\Q$W\E" \(PID \d+\) exited with exit code [1-9]/,
	'server log has no crashes and the worker exited cleanly');

done_testing();
