# Eviction under pressure (DESIGN.md §5.3, §5.4, §9; backlog
# 20261005-091225-15): when an insert finds the table at max_entries, an
# eviction pass under the exclusive lock first reclaims every dead entry
# (no live slot), then, if that freed fewer than
# max(1, max_entries * 5 / 100) entries, evicts live entries ordered by
# last_bucket (oldest first) then usage (lowest first) until that many are
# free; every surviving entry's usage decays by 0.99. dealloc counts passes,
# evicted_entries the entries removed (dead or live), and the insert that
# triggered the pass succeeds. Driven through the TEST-ONLY module
# test/modules/pssc_store_test, with its shared debug clock pinned so that
# bucket boundaries (and dead entries) are deterministic.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('eviction');
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
# "entries dealloc reclaimed_entries evicted_entries dropped_records"
sub evict_state
{
	my $c = counters();
	return "$c->{entries} $c->{dealloc} $c->{reclaimed_entries} $c->{evicted_entries} "
	  . $c->{dropped_records};
}
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
# Record queryids $from..$to (empty tags) $times times each.
sub fill
{
	my ($from, $to, $times) = @_;
	$times //= 1;
	is(sql(qq{SELECT count(*) FROM generate_series($from, $to) q,
	                 generate_series(1, $times) t
	          WHERE pssc_store_test_record(q) NOT IN ('inserted', 'updated')}), 0,
		"recorded queryids $from..$to x $times");
}
# Sorted queryids present in the table (matching $where).
sub keys_present
{
	my ($where) = @_;
	$where //= 'true';
	return sql(qq{SELECT string_agg(queryid::text, ',' ORDER BY queryid) FROM
	              (SELECT DISTINCT queryid FROM pssc_store_test_entries()
	                WHERE $where) e});
}
sub ids { return join(',', @_); }
sub pgbench
{
	my ($scripts, $clients, $txns) = @_;
	my @args;
	my $i = 0;
	for my $s (@$scripts)
	{
		my ($text, $weight) = @$s;
		my $file = $node->basedir . '/pgbench_evict_' . $i++ . '.sql';
		open my $fh, '>', $file or die "open $file: $!";
		print $fh $text;
		close $fh;
		push @args, '-f', "$file\@$weight";
	}
	my ($out, $err);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	IPC::Run::run([ 'pgbench', '-n', '-c', $clients, '-j', $clients,
			'-t', $txns, @args, 'postgres' ],
		'>', \$out, '2>', \$err)
	  or die "pgbench failed: $err";
	my $n = $clients * $txns;
	like($out, qr{number of transactions actually processed: $n/$n},
		"pgbench ran all $n transactions");
	return $n;
}

configure(max_entries => 100);

# ----------------------------------------------- sequential churn, limits
{
	fresh();
	is(evict_state(), '0 0 0 0 0', 'starts empty, no passes');
	# Every insert must succeed and the entry count may never exceed the limit.
	is(sql(q{DO $$
	DECLARE r text; c record;
	BEGIN
	  FOR q IN 1..1000 LOOP
	    r := pssc_store_test_record(q);
	    IF r <> 'inserted' THEN
	      RAISE EXCEPTION 'record % returned %', q, r;
	    END IF;
	    SELECT * INTO c FROM pssc_store_test_counters();
	    IF c.entries > c.max_entries OR c.entries <> c.hash_entries THEN
	      RAISE EXCEPTION 'after record %: entries %, hash %, max %',
	        q, c.entries, c.hash_entries, c.max_entries;
	    END IF;
	  END LOOP;
	END $$; SELECT 'ok'}), 'ok',
		'1000 distinct keys into max_entries=100: every insert succeeds, never above the limit');
	# target = max(1, 100 * 5 / 100) = 5: each pass frees 5, then 5 inserts
	# refill the table, so inserts 101, 106, ..., 996 each trigger a pass.
	is(evict_state(), '100 180 0 900 0',
		'180 passes evicted 900 entries (5 each); nothing dropped');
	is(rec(q{1000}), 'updated', 'the newest entry survived and is updated in place');
	is(keys_present(), ids(901 .. 1000),
		'pgss-style decay makes churn FIFO: the 100 newest keys remain');
	is(sql('SELECT pssc_store_test_check_invariants()'), 100, 'invariants hold');
}

# ------------------------------------------------- dead entries first
{
	my $b = fresh();
	fill(1, 30, 10);	# heavily used, but only in bucket $b
	pin($b + 12);		# 12 buckets later: live window [$b + 1, $b + 12]
	fill(101, 170);
	is(sql('SELECT count(DISTINCT queryid) FROM pssc_store_test_entries() WHERE dead'), 30,
		'30 dead entries (usage 11) and 70 live ones (usage 2)');
	is(rec(q{999}), 'inserted', 'insert into a full table succeeds');
	is(evict_state(), '71 1 30 0 0',
		'one pass reclaimed all 30 dead entries (more than the target of 5) and no live one');
	is(keys_present(), ids(101 .. 170, 999), 'every live entry survived');
	is(sql(q{SELECT string_agg(DISTINCT usage::text, ',') FROM pssc_store_test_entries()
	         WHERE queryid <> 999}),
		sql('SELECT (2.0::float8 * 0.99::float8)::text'),
		'surviving entries decayed by 0.99 (2.0 -> 1.98)');
	is(sql('SELECT DISTINCT usage FROM pssc_store_test_entries() WHERE queryid = 999'), 2,
		'the new entry starts undecayed (usage 2 after its first call)');

	# Fewer dead entries than the target: the rest comes from live ones.
	$b = fresh();
	fill(1, 2, 10);
	pin($b + 12);
	fill(101, 103);			# usage 2
	fill(104, 198, 2);		# usage 3
	is(counters()->{entries}, 100, 'full: 2 dead + 98 live');
	is(rec(q{999}), 'inserted', 'insert into a full table succeeds');
	is(evict_state(), '96 1 2 3 0', 'one pass freed 5: the 2 dead ones (reclaimed) and 3 live ones (evicted)');
	is(keys_present(), ids(104 .. 198, 999),
		'the dead entries and the 3 lowest-usage live entries were evicted');
	is(sql('SELECT pssc_store_test_check_invariants()'), 96, 'invariants hold');
}

# ------------------------------------------------- order among live entries
{
	# Least recently written first, whatever the usage.
	my $b = fresh();
	fill(1, 5, 20);			# usage 21, last written in bucket $b
	pin($b + 1);
	fill(6, 100);			# usage 2, last written in bucket $b + 1
	is(sql('SELECT count(*) FROM pssc_store_test_entries() WHERE dead'), 0,
		'all 100 entries are live');
	is(rec(q{999}), 'inserted', 'insert into a full table succeeds');
	is(evict_state(), '96 1 0 5 0', 'one pass evicted 5 live entries');
	is(keys_present('queryid <= 5'), '',
		'the 5 least recently written entries were evicted despite the highest usage');

	# Same last_bucket: lowest usage first.
	fresh();
	my @low = (7, 23, 50, 81, 99);
	my %low = map { $_ => 1 } @low;
	for my $q (1 .. 100)
	{
		rec($q);
		rec($q) unless $low{$q};	# usage 3, the five low ones usage 2
	}
	is(rec(q{999}), 'inserted', 'insert into a full table succeeds');
	is(evict_state(), '96 1 0 5 0', 'one pass evicted 5 live entries');
	is(keys_present('queryid IN (' . ids(@low) . ')'), '',
		'within one bucket the 5 lowest-usage entries were evicted');
	is(sql(q{SELECT string_agg(DISTINCT usage::text, ',') FROM pssc_store_test_entries()
	         WHERE queryid <= 100}),
		sql('SELECT (3.0::float8 * 0.99::float8)::text'),
		'survivors decayed once (3.0 -> 2.97)');

	# Next pass: the older entries decay to 2.9403 and outrank the five
	# entries inserted since the first pass (999..1003: 2.0 -> 1.98).
	fill(1000, 1003);
	is(counters()->{entries}, 100, 'full again');
	is(rec(q{1004}), 'inserted', 'second triggering insert succeeds');
	is(evict_state(), '96 2 0 10 0', 'second pass: dealloc 2, evicted_entries 10');
	is(keys_present('queryid >= 999'), '1004',
		'the second pass evicted the low-usage newcomers 999..1003');
}

# --------------------------------------------- forced hash collisions
{
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(true)');
	fresh();
	fill(1, 300);
	is(evict_state(), '100 40 0 200 0', 'one dynahash chain: churn evicts correctly');
	is(keys_present(), ids(201 .. 300), 'the 100 newest keys remain');
	is(rec(q{250}), 'updated', 'surviving colliding keys are still found');
	is(sql('SELECT pssc_store_test_check_invariants()'), 100, 'invariants hold');
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(false)');
}

# --------------------------- allocation failure: fall back, never corrupt
{
	# The candidate buffer cannot be allocated: with no dead entry to
	# reclaim, the record is dropped and counted (the statement does not fail).
	fresh();
	fill(1, 100);
	is(sql(q{SELECT pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(999)}), '|full',
		'allocation failure with no dead entries: the record is dropped');
	is(evict_state(), '100 1 0 0 1', 'counted in dropped_records; nothing evicted');
	is(keys_present(), ids(1 .. 100), 'the table is unchanged');
	is(sql('SELECT pssc_store_test_check_invariants()'), 100, 'invariants hold');
	is(rec(q{999}), 'inserted', 'the next insert evicts normally');
	is(evict_state(), '96 2 0 5 1', 'second pass evicted 5');

	# Dead entries are reclaimed without allocating, so the insert succeeds.
	my $b = fresh();
	fill(1, 3);
	pin($b + 12);
	fill(101, 197);
	is(sql(q{SELECT pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(999)}), '|inserted',
		'allocation failure with 3 dead entries: the insert still succeeds');
	is(evict_state(), '98 1 3 0 0', 'the dead entries were reclaimed; no live one');
	is(keys_present(), ids(101 .. 197, 999), 'every live entry survived');
	is(sql('SELECT pssc_store_test_check_invariants()'), 98, 'invariants hold');
}

# ----------------------------------- the candidate buffer across passes
{
	# A backend keeps its candidate buffer after a pass (max_entries 100:
	# 5 candidates); the testing aid must still fail the next pass of the
	# same backend, which already holds a buffer. One statement, so one
	# backend: the first record runs a pass, four more refill the table.
	fresh();
	fill(1, 100);
	is(sql(q{SELECT pssc_store_test_record(999), pssc_store_test_record(1000),
	                pssc_store_test_record(1001), pssc_store_test_record(1002),
	                pssc_store_test_record(1003),
	                pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(1004), pssc_store_test_record(1005)}),
		'inserted|inserted|inserted|inserted|inserted||full|inserted',
		'one backend: pass, refill, failed pass (buffer held), normal pass');
	is(evict_state(), '96 3 0 10 1', 'three passes; only the failed one dropped');
	is(sql('SELECT pssc_store_test_check_invariants()'), 96, 'invariants hold');
}

# A candidate buffer above 64 kB (max_entries 50000: 2500 candidates) is
# allocated for each pass and freed after it, not kept.
configure(max_entries => 50000);
{
	fresh();
	fill(1, 50000);
	fill(2501, 50000);		# 1..2500 now have the lowest usage
	is(sql(q{SELECT count(*) FROM generate_series(50001, 52501) q
	          WHERE pssc_store_test_record(q) <> 'inserted'}), 0,
		'max_entries 50000: 2501 inserts in one backend succeed');
	is(evict_state(), '47501 2 0 5000 0', 'two passes of 2500 each');
	is(sql(q{SELECT count(*) FROM pssc_store_test_entries() WHERE queryid <= 2500}),
		0, 'the first pass evicted the 2500 lowest-usage entries');
	is(sql(q{SELECT count(*) FROM pssc_store_test_entries()
	          WHERE queryid BETWEEN 2501 AND 50000}), 47500,
		'the 47500 entries recorded twice survive both passes');
	is(keys_present('queryid > 50000'), '52501',
		'the second pass evicted the 2500 newcomers (usage 1.98 < 2.94)');
	is(sql('SELECT pssc_store_test_check_invariants()'), 47501, 'invariants hold');

	# The testing aid fails that per-pass allocation itself: nothing to
	# free afterwards, the record is dropped, and the next pass (same
	# backend) allocates normally.
	fresh();
	fill(1, 50000);
	is(sql(q{SELECT pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(60000), pssc_store_test_record(60001)}),
		'|full|inserted',
		'max_entries 50000: failed per-pass allocation, then a normal pass');
	is(evict_state(), '47501 2 0 2500 1', 'the failed pass dropped; the next evicted 2500');
	is(sql('SELECT pssc_store_test_check_invariants()'), 47501, 'invariants hold');
}
configure(max_entries => 100);

# ------------------------------------------- concurrent churn (pgbench)
# Two buckets: a clock step of two intervals kills every entry not written
# since, so passes under load reclaim dead entries as well as live ones.
configure(bucket_count => 2);
for my $collide ('f', 't')
{
	sql('DROP TABLE IF EXISTS results');
	sql('CREATE UNLOGGED TABLE results (r text)');
	sql('SELECT pssc_store_test_reset()');
	sql("SELECT pssc_store_test_force_collisions('$collide')");
	fresh();
	my $n = pgbench([
		[ q{\set q random(1, 2000)
INSERT INTO results SELECT pssc_store_test_record(:q, '{}', NULL, 0.5);
}, 90 ],
		[ q{SELECT pssc_store_test_check_invariants();
}, 5 ],
		# Step the pinned clock a whole bucket: entries go dead under load.
		[ q{SELECT pssc_store_test_advance_clock(interval_us) FROM pssc_store_test_buckets();
}, 5 ],
	], 8, 1000);
	my $c = counters();
	ok($c->{entries} > 0 && $c->{entries} <= 100 && $c->{entries} == $c->{hash_entries},
		"collisions=$collide: entries ($c->{entries}) within max_entries, matching the hash table");
	is($c->{dropped_records}, 0, "collisions=$collide: no record was dropped");
	my $freed = $c->{reclaimed_entries} + $c->{evicted_entries};
	ok($c->{dealloc} > 0 && $freed >= 5 * $c->{dealloc},
		"collisions=$collide: passes ran ($c->{dealloc}), each freeing at least the "
		  . "target of 5 ($c->{reclaimed_entries} reclaimed, $c->{evicted_entries} evicted)");
	ok($freed > 5 * $c->{dealloc} && $c->{reclaimed_entries} > 0,
		"collisions=$collide: some passes reclaimed more dead entries than the target");
	my ($records, $inserted, $full) = split /\|/, sql(q{SELECT count(*),
	    count(*) FILTER (WHERE r = 'inserted'), count(*) FILTER (WHERE r = 'full')
	    FROM results});
	is($full, 0, "collisions=$collide: every record succeeded");
	is($inserted - $freed, $c->{entries},
		"collisions=$collide: inserted ($inserted) - evicted = entries");
	my $calls = sql('SELECT coalesce(sum(calls), 0) FROM pssc_store_test_entries()');
	ok($calls >= $c->{entries} && $calls <= $records,
		"collisions=$collide: stored calls ($calls) <= records ($records)");
	is(sql('SELECT pssc_store_test_check_invariants()'), $c->{entries},
		"collisions=$collide: invariants hold after the run");
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(false)');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|terminated by signal|TRAP/,
	'server log has no crash');

done_testing();
