# Time buckets and lazy per-entry ring rollover (DESIGN.md §5.2, §5.4, §9;
# backlog 20261005-091225-14): the header epoch/interval/current_bucket,
# floor() bucket ids (also before the epoch), header advance and clamping,
# per-entry ring rollover, readers that hide expired slots without writes,
# stalled writers, clock steps backwards and forwards, and a concurrent
# stress run across bucket boundaries with the ring invariants checked.
# Driven through the TEST-ONLY module test/modules/pssc_store_test; clock
# steps use its shared debug clock (pinned or offset), not sleeps, except in
# the real-time 1 s rollover test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Time::HiRes qw(usleep);
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('buckets');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
max_connections = 40
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pssc_store_test');

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub rec { return sql("SELECT pssc_store_test_record($_[0])"); }
sub configure
{
	my (%g) = @_;
	for my $k (sort keys %g)
	{
		if (defined $g{$k}) { sql("ALTER SYSTEM SET $P.$k = '$g{$k}'"); }
		else { sql("ALTER SYSTEM RESET $P.$k"); }
	}
	$node->restart;
}
sub buckets
{
	my @cols = qw(epoch interval_us bucket_count current_bucket clock_bucket
	  reader_bucket now advances clock_mode clock_value);
	my @v = split /\|/, sql('SELECT * FROM pssc_store_test_buckets()'), -1;
	my %b;
	@b{@cols} = @v;
	return \%b;
}
# Pin the shared debug clock $us microseconds into bucket $b (default: the
# middle of the bucket).
sub pin
{
	my ($b, $us) = @_;
	$us //= 'interval_us / 2';
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($b)
	         + (($us) || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}
# "queryid:bucket:calls" of the slots matching $where (default: live ones).
sub rows
{
	my ($where) = @_;
	$where //= 'live';
	return sql(qq{SELECT string_agg(format('%s:%s:%s', queryid, bucket_id, calls), ' '
	               ORDER BY queryid, bucket_id) FROM pssc_store_test_entries() WHERE $where});
}
sub last_bucket
{
	return sql("SELECT DISTINCT last_bucket FROM pssc_store_test_entries() WHERE queryid = $_[0]");
}
sub wait_for
{
	my ($cond, $what) = @_;
	for (1 .. 400)
	{
		return 1 if $cond->();
		usleep(50_000);
	}
	die "timed out waiting for $what";
}
sub pgbench
{
	my ($script, $clients, $txns, @opts) = @_;
	my $file = $node->basedir . '/pgbench_buckets.sql';
	open my $fh, '>', $file or die "open $file: $!";
	print $fh $script;
	close $fh;
	my ($out, $err);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	IPC::Run::run([ 'pgbench', '-n', '-c', $clients, '-j', $clients,
			'-t', $txns, @opts, '-f', $file, 'postgres' ],
		'>', \$out, '2>', \$err)
	  or die "pgbench failed: $err";
	my $n = $clients * $txns;
	like($out, qr{number of transactions actually processed: $n/$n},
		"pgbench ran all $n transactions");
	return $n;
}

# ------------------------------------------- header: epoch, interval, ids
{
	my $b = buckets();
	is("$b->{interval_us} $b->{bucket_count}", '300000000 12',
		'header holds bucket_interval (us) and bucket_count');
	is("$b->{current_bucket} $b->{advances} $b->{clock_mode} $b->{clock_value}",
		'0 0 real 0', 'current_bucket starts at 0; real clock without offset');
	is(sql(q{SELECT (extract(epoch FROM epoch)
	                 - extract(epoch FROM timestamptz '2000-01-01 00:00:00+00')) % 300 = 0
	                AND epoch <= pg_postmaster_start_time() + interval '5 s'
	                AND pg_postmaster_start_time() < epoch + interval '305 s'
	         FROM pssc_store_test_buckets()}), 't',
		'epoch = postmaster start rounded down to a multiple of the interval since 2000-01-01');
	is(sql(q{SELECT clock_bucket = floor(extract(epoch FROM now - epoch) / 300)
	                AND reader_bucket = greatest(clock_bucket, current_bucket)
	         FROM pssc_store_test_buckets()}), 't',
		'clock_bucket = floor((now - epoch) / interval); reader = max(clock, header)');
	is(sql(q{SELECT pssc_store_test_bucket_start(0) = epoch
	                AND pssc_store_test_bucket_start(3) = epoch + interval '900 s'
	                AND pssc_store_test_bucket_start(-2) = epoch - interval '600 s'
	         FROM pssc_store_test_buckets()}), 't', 'bucket_start = epoch + id * interval');

	# floor() below the epoch (signed ids), through the store's own clock
	my @cases = ([ '0', 0 ], [ '300000000 - 1', 0 ], [ '300000000', 1 ],
		[ '-1', -1 ], [ '-300000000', -1 ], [ '-300000000 - 1', -2 ],
		[ '-1000000000', -4 ]);
	for my $c (@cases)
	{
		sql(qq{SELECT pssc_store_test_pin_clock(epoch + (($c->[0]) || ' us')::interval)
		       FROM pssc_store_test_buckets()});
		is(sql('SELECT pssc_store_test_clock_bucket()'), $c->[1],
			"epoch + ($c->[0]) us is bucket $c->[1]");
	}
	$b = buckets();
	is("$b->{clock_mode} $b->{clock_bucket} $b->{reader_bucket}", 'pinned -4 0',
		'the pinned clock is shared (seen by another session); reader bucket '
		  . 'does not go below current_bucket');

	is(rec(q{1, ARRAY['a', '1']}), 'inserted', 'record with the clock before the epoch');
	is(rows('true'), '1:0:1', 'computed id -4 is clamped up to current_bucket 0');
	$b = buckets();
	is("$b->{current_bucket} $b->{advances}", '0 0', 'an older id does not move current_bucket');

	# offset mode
	sql('SELECT pssc_store_test_set_clock_offset(3600000000)');
	$b = buckets();
	is($b->{clock_mode}, 'real', 'offset mode');
	is(sql(q{SELECT abs(extract(epoch FROM now - clock_timestamp()) - 3600) < 60
	         FROM pssc_store_test_buckets()}), 't', 'an offset of +1 h shifts the clock by 1 h');
	ok($b->{clock_bucket} >= 12, "clock bucket with +1 h ($b->{clock_bucket}) is >= 12");
	my ($ok, $out, $err) =
	  $node->psql('postgres', 'SELECT pssc_store_test_set_clock_offset(200000000000000000)');
	like($err, qr/out of range/, 'an absurd offset is rejected');
	($ok, $out, $err) = $node->psql('postgres', q{SELECT pssc_store_test_pin_clock('infinity')});
	like($err, qr/out of range|finite/, 'pinning to infinity is rejected');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
	is(buckets()->{clock_mode} . ' ' . buckets()->{clock_value}, 'real 0', 'back to the real clock');
}

# ------------------------------------------ real time: 1 s rollover (4 buckets)
configure(bucket_interval => 1, bucket_count => 4);
{
	my $b = buckets();
	is("$b->{interval_us} $b->{bucket_count} $b->{current_bucket}", '1000000 4 0',
		'1 s interval, 4 buckets, fresh header');

	rec(q{100, '{}'});
	my $b1 = last_bucket(100);
	ok($b1 <= sql('SELECT pssc_store_test_clock_bucket()'), "first record in clock bucket $b1");
	wait_for(sub { sql('SELECT pssc_store_test_clock_bucket()') > $b1 }, 'the next bucket');
	rec(q{100, '{}'});
	my $b2 = last_bucket(100);
	ok($b2 > $b1, "next record lands in a newer bucket ($b2 > $b1)");
	is(rows('queryid = 100 AND live'), "100:$b1:1 100:$b2:1", 'two live slots, one call each');

	# Record once the clock is in a bucket that maps to $b1's slot again.
	my $b3;
	for my $try (1 .. 8)
	{
		wait_for(sub {
			my $c = sql('SELECT pssc_store_test_clock_bucket()');
			$c > $b2 && ($c - $b1) % 4 == 0;
		}, "a bucket in the slot of $b1");
		rec(q{100, '{}'});
		$b3 = last_bucket(100);
		last if ($b3 - $b1) % 4 == 0;
	}
	ok($b3 >= $b1 + 4 && ($b3 - $b1) % 4 == 0, "rolled over into the slot of $b1 (bucket $b3)");
	is(rows("queryid = 100 AND bucket_id = $b1"), '', "bucket ${b1}'s counts are gone");
	is(rows("queryid = 100 AND bucket_id = $b3"), "100:$b3:1",
		'the slot was zeroed and relabeled before adding');
	unlike(rows('queryid = 100 AND live'), qr/:$b1:/, "readers no longer see bucket $b1");

	# No writes at all from here: readers still expire the entry.
	wait_for(sub { sql('SELECT pssc_store_test_clock_bucket()') >= $b3 + 4 }, 'the ring to expire');
	is(rows('queryid = 100 AND live'), '', 'with no writes, readers hide every expired slot');
	is(sql('SELECT bool_and(dead) FROM pssc_store_test_entries() WHERE queryid = 100'), 't',
		'the entry is dead');
	ok(buckets()->{current_bucket} >= $b3 + 4,
		'readers advanced current_bucket (the shared monotonic watermark) to the clock');
}

# ------------------------------------------- pinned clock: deterministic
{
	sql('SELECT pssc_store_test_reset()');
	my $base = buckets()->{reader_bucket} + 100;
	my $adv0 = buckets()->{advances};

	pin($base);
	is(rec(q{1, '{}'}), 'inserted', 'record in a newer bucket');
	my $b = buckets();
	is("$b->{current_bucket} " . ($b->{advances} - $adv0), "$base 1",
		'a newer computed id advances current_bucket (once)');
	rec(q{2, '{}'});
	rec(q{1, '{}'});
	is(buckets()->{advances} - $adv0, 1,
		'one header advance per bucket, not per entry or per call');
	is(rows(), "1:$base:2 2:$base:1", 'calls attributed to the clock bucket at record time');

	pin($base + 1);
	rec(q{1, '{}'});
	is(rows(), "1:$base:2 1:@{[$base + 1]}:1 2:$base:1", 'next bucket: next slot');

	# Per-entry ring rollover: base + 4 maps to base's slot.
	pin($base + 4);
	is(rec(q{1, '{}'}), 'updated', 'record in base + 4');
	is(rows('queryid = 1'), "1:@{[$base + 1]}:1 1:@{[$base + 4]}:1",
		'slot of base relabeled to base + 4 and zeroed before adding (2 calls gone)');
	is(last_bucket(1), $base + 4, 'last_bucket = the id written');
	is(rows('queryid = 2 AND live'), '', 'entry 2 (never written again): its expired slot is hidden');
	is(rows('queryid = 2 AND dead'), "2:$base:1", 'entry 2 is dead; its slot is still in memory');

	# Readers expire slots without any writes.
	pin($base + 7);
	is(rows(), "1:@{[$base + 4]}:1", 'clock at base + 7, no writes: only base + 4 is live');
	$b = buckets();
	is("$b->{current_bucket} $b->{reader_bucket}", ($base + 7) . ' ' . ($base + 7),
		'the read advanced current_bucket to the clock bucket, without any write');
	pin($base + 8);
	is(rows(), '', 'clock at base + 8, no writes: nothing is live');
	is(sql('SELECT bool_and(dead) FROM pssc_store_test_entries()'), 't', 'every entry is dead');
	is(sql('SELECT pssc_store_test_check_invariants()'), 2, 'invariants hold (2 entries)');

	# Readers are monotonic: once a read observed base + 8, stepping the
	# clock back (no write in between) does not bring the old counts back.
	pin($base + 4);
	is(rows(), '', 'clock stepped back to base + 4 after a read at base + 8: still nothing live');
	is(sql('SELECT bool_and(dead) FROM pssc_store_test_entries()'), 't', 'every entry stays dead');
	$b = buckets();
	is("$b->{current_bucket} $b->{reader_bucket}", ($base + 8) . ' ' . ($base + 8),
		'current_bucket kept the high-water mark of the read');

	# A stalled writer: its id goes stale while it waits for the lock.
	my $s = $base + 20;
	pin($s);
	rec(q{1, '{}'});
	is(sql(q{SELECT pssc_store_test_stall_next_record(2000000, 9);
		SELECT pssc_store_test_record(1, '{}')}) =~ s/^\s+//r, 'updated',
		'stalled record (computed base + 20; hook is per-backend, same session)');
	is(buckets()->{current_bucket}, $s + 2, 'meanwhile another backend advanced to base + 22');
	is(rows('queryid IN (1, 9) AND live'), "1:$s:1 1:@{[$s + 2]}:1 9:@{[$s + 2]}:1",
		'the stale id was clamped to current_bucket (base + 20 still has 1 call)');

	# Stalled longer than the whole ring: its id would be an expired bucket.
	sql(q{SELECT pssc_store_test_stall_next_record(10000000, 9);
		SELECT pssc_store_test_record(1, '{}')});
	is(rows('queryid = 1 AND live'), "1:@{[$s + 12]}:1",
		'a stale id from an expired bucket is clamped to the live current_bucket');
	is(rows("queryid = 1 AND bucket_id = $s + 2"), "1:@{[$s + 2]}:1",
		'the expired slot was not written');

	# Caller-computed ids (pssc_store_record_at).
	is(rec(qq{1, '{}', $base}), 'updated', 'record_at with an old id');
	is(rows('queryid = 1 AND live'), "1:@{[$s + 12]}:2", 'old id clamped to current_bucket');
	my $adv = buckets()->{advances};
	rec(qq{1, '{}', @{[$s + 13]}});
	$b = buckets();
	is("$b->{current_bucket} " . ($b->{advances} - $adv), ($s + 13) . ' 1',
		'record_at with a newer id advances current_bucket');

	# A stalled writer while only the clock moves (nobody else writes or
	# reads): the clock is re-read under the lock, so the write lands in the
	# clock's bucket, which is live, not in the stale (expired) one.
	is(sql(q{SELECT pssc_store_test_stall_next_record(10000000);
		SELECT pssc_store_test_record(1, '{}')}) =~ s/^\s+//r, 'updated',
		'clock-only stall of 10 buckets (computed base + 32)');
	is(rows('queryid = 1 AND bucket_id = last_bucket'), "1:@{[$s + 22]}:1",
		'written into the clock bucket base + 42 (re-read after the lock)');
	is(rows('queryid = 1 AND bucket_id = last_bucket AND live'), "1:@{[$s + 22]}:1",
		'the destination slot is live');
	is(buckets()->{current_bucket}, $s + 22, 'the writer advanced current_bucket to the clock');

	# The same with a caller-computed stale id and no reads in between.
	pin($s + 32);
	is(rec(qq{1, '{}', @{[$s + 22]}}), 'updated', 'record_at with id base + 42, clock at base + 52');
	is(rows('queryid = 1 AND bucket_id = last_bucket AND live'), "1:@{[$s + 32]}:1",
		'written into the live clock bucket base + 52, not the expired base + 42');
	is(sql('SELECT pssc_store_test_check_invariants()'), 3, 'invariants hold (3 entries)');
	my $cur = $s + 32;

	# The clock steps backwards: current_bucket does not regress.
	pin($base - 1000);
	is(rec(q{1, '{}'}), 'updated', 'record with the clock 1000 buckets back');
	$b = buckets();
	is("$b->{current_bucket} $b->{clock_bucket} $b->{reader_bucket}",
		"$cur " . ($base - 1000) . " $cur",
		'backward step: current_bucket kept; readers use current_bucket');
	is(rows("queryid = 1 AND bucket_id = $cur"), "1:$cur:2", 'written into current_bucket');
	is(rows("queryid = 1 AND live AND bucket_id = $cur"), "1:$cur:2", 'and still live');
	sql('SELECT pssc_store_test_set_clock_offset(-86400000000)');
	rec(q{1, '{}'});
	is(buckets()->{current_bucket}, $cur, 'a -1 day offset does not regress current_bucket');
	is(rows("queryid = 1 AND bucket_id = $cur"), "1:$cur:3", 'written into current_bucket');

	# A forward jump larger than the ring hides everything.
	pin($cur + 100);
	is(rows(), '', 'forward jump of 100 buckets: nothing is live, without any write');
	is(sql('SELECT count(*) FILTER (WHERE NOT dead) FROM pssc_store_test_entries()'), 0,
		'every entry is dead');
	rec(q{1, '{}'});
	is(rows(), "1:@{[$cur + 100]}:1", 'the next record starts a fresh live slot');
	is(buckets()->{current_bucket}, $cur + 100, 'current_bucket jumped too');

	# reset keeps current_bucket (it never decreases) and the debug clock
	sql('SELECT pssc_store_test_reset()');
	$b = buckets();
	is("$b->{current_bucket} $b->{clock_mode}", ($cur + 100) . ' pinned',
		'reset keeps current_bucket and the debug clock');
	is(sql('SELECT pssc_store_test_check_invariants()'), 0, 'invariants hold on an empty table');
}

# ------------------------------- concurrent stress across bucket boundaries
configure(bucket_interval => 1, bucket_count => 64);
{
	my $b0 = buckets()->{reader_bucket} + 10;
	pin($b0, 0);

	# Phase A: the clock crosses ~20 boundaries (250 ms steps by any client,
	# racing with the records) but never wraps the 64-slot ring, so no count
	# may be lost and every call must be in a bucket in [b0, current].
	my $n = pgbench(q{\set q random(1, 20)
\set r random(1, 100)
\if :r <= 2
SELECT pssc_store_test_advance_clock(250000);
\endif
SELECT pssc_store_test_record(:q, ARRAY['k', (:q % 3)::text], NULL, 0.5);
}, 8, 500);
	my $b = buckets();
	is(sql('SELECT sum(calls) || \' \' || sum(total_exec_time) FROM pssc_store_test_entries()'),
		"$n " . ($n / 2), 'no ring wrap: no call lost (calls and time)');
	is(sql("SELECT min(bucket_id) >= $b0 AND max(bucket_id) = $b->{current_bucket}
	        FROM pssc_store_test_entries()"), 't',
		'every slot is in [b0, current_bucket], and current_bucket was written');
	my $nb = sql('SELECT count(DISTINCT bucket_id) FROM pssc_store_test_entries()');
	ok($nb >= 5, "records spread over $nb buckets");
	is(sql('SELECT pssc_store_test_check_invariants()'), 20, 'invariants hold (20 entries)');

	# Phase B: small steps, jumps past the ring, backward steps, and
	# concurrent invariant checks (which also check that current_bucket
	# never goes backwards as seen by each client). The random jumps alone
	# leave the ring unwrapped in ~1% of runs (backward steps cancel them)
	# and need not reuse a slot (70 = 6 mod 64), so client 0 also wraps its
	# entry's ring onto an occupied slot at its transactions 50, 150, ...,
	# 450 (:i counts them; pgbench variables persist across transactions):
	# it records :q, reads that entry's last_bucket b, then pins the clock
	# to b + 64 and records with that caller-computed id in one statement.
	# b's slot is b + 64's slot, so the write rolls it over (relabel, zero)
	# unless current_bucket has already passed b + 64 (then the id is
	# clamped up to it); either way record_at advances current_bucket
	# to >= b + 64 whatever the others do to the clock, so each wrap raises
	# current_bucket by >= 64 and the five together by >= 320,
	# deterministically, while the other clients keep racing.
	sql('SELECT pssc_store_test_reset()');
	$n = pgbench(q{\set q random(1, 20)
\set r random(1, 1000)
\set i :i + 1
\if :client_id = 0 and :i % 100 = 50
SELECT pssc_store_test_record(:q, ARRAY['k', (:q % 3)::text], NULL, 0.5);
SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start(b + 64)),
       pssc_store_test_record(:q, ARRAY['k', (:q % 3)::text], b + 64, 0.5)
  FROM (SELECT max(last_bucket) AS b FROM pssc_store_test_entries() WHERE queryid = :q) s;
\endif
\if :r <= 20
SELECT pssc_store_test_advance_clock(250000);
\elif :r <= 23
SELECT pssc_store_test_advance_clock(70000000);
\elif :r <= 26
SELECT pssc_store_test_advance_clock(-30000000);
\elif :r <= 60
SELECT pssc_store_test_check_invariants();
\endif
SELECT pssc_store_test_record(:q, ARRAY['k', (:q % 3)::text], NULL, 0.5);
}, 8, 500, '-D', 'i=0');
	$n += 10;	# client 0's extra records (two per wrap)
	$b = buckets();
	is(sql('SELECT pssc_store_test_check_invariants()'), 20, 'invariants hold after phase B');
	ok(sql('SELECT sum(calls) FROM pssc_store_test_entries()') <= $n, 'no call counted twice');
	# Client 0's 50 records up to its first wrap (from b) are in buckets
	# <= b, which the wrap to b + 64 took out of every ring's live window.
	ok(sql('SELECT coalesce(sum(calls), 0) FROM pssc_store_test_entries() WHERE live') <= $n - 50,
		'calls recorded before the wrap are no longer live');
	is(sql("SELECT max(bucket_id) FROM pssc_store_test_entries()"), $b->{current_bucket},
		'the newest slot is current_bucket');
	is(sql("SELECT count(*) FROM pssc_store_test_entries() WHERE bucket_id > $b->{current_bucket}"),
		0, 'no slot is newer than current_bucket');
	ok($b->{current_bucket} >= $b0 + 5 * 64,
		"the ring wrapped during phase B (current_bucket = b0 + @{[$b->{current_bucket} - $b0]})");
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|terminated by signal|TRAP/,
	'server log has no crash');

done_testing();
