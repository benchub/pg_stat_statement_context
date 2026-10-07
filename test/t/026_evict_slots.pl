# The compact eviction array (DESIGN.md §5.1, §5.3; backlog
# 20261006-075124-1): one (last_bucket, usage, entry) slot per hash entry,
# kept in the store's shared header, which the eviction pass scans instead
# of the whole entries. It must hold exactly one slot per hash entry, in
# slots 0 .. entries - 1, with that entry's last_bucket, under every path
# that inserts, updates, reclaims, evicts or resets entries: sequential
# churn, dead-entry reclaim, mixed passes, forced hash collisions, a failed
# candidate allocation, reset, and concurrent churn (pgbench) while the
# clock moves. Its size is part of the store's shared memory request
# (max_entries x slot size, in the "pg_stat_statement_context" allocation).
# Driven through the TEST-ONLY module test/modules/pssc_store_test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('evict_slots');
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
sub entries
{
	return sql('SELECT entries FROM pssc_store_test_counters()');
}
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
# Record queryids $from..$to $times times each (the qual names t too, so
# it runs once per joined row, not once per q).
sub fill
{
	my ($from, $to, $times) = @_;
	$times //= 1;
	is(sql(qq{SELECT count(*) FROM generate_series($from, $to) q,
	                 generate_series(1, $times) t
	          WHERE pssc_store_test_record(q + 0 * t) NOT IN ('inserted', 'updated')}), 0,
		"recorded queryids $from..$to x $times");
}

# The array, compared with the hash table in one snapshot each:
#  - as many slots as entries (header and hash table);
#  - slot indexes are exactly 0 .. entries - 1;
#  - the slots name exactly the hash entries (key), each once, and carry
#    that entry's last_bucket and usage.
sub consistent
{
	my ($what) = @_;
	my $n = entries();
	is(sql('SELECT count(*) FROM pssc_store_test_evict_slots()'), $n,
		"$what: one slot per entry ($n)");
	is(sql(q{SELECT count(*) = coalesce(max(idx) + 1, 0) AND count(DISTINCT idx) = count(*)
	           AND coalesce(min(idx), 0) = 0
	         FROM pssc_store_test_evict_slots()}), 't',
		"$what: slots are dense from 0");
	is(sql(q{WITH a AS (SELECT queryid, tags, toplevel, last_bucket, usage
	                      FROM pssc_store_test_evict_slots()),
	              h AS (SELECT DISTINCT queryid, tags, toplevel, last_bucket, usage
	                      FROM pssc_store_test_entries())
	         SELECT (SELECT count(*) FROM (SELECT * FROM a EXCEPT ALL SELECT * FROM h) x)
	              + (SELECT count(*) FROM (SELECT * FROM h EXCEPT ALL SELECT * FROM a) y)}),
		0, "$what: slots match the hash entries (key, last_bucket, usage)");
	is(sql('SELECT pssc_store_test_check_invariants()'), $n,
		"$what: invariants (including the array) hold");
}

configure(max_entries => 100);

# ------------------------------------------------------------ the basics
{
	fresh();
	is(sql('SELECT count(*) FROM pssc_store_test_evict_slots()'), 0,
		'empty store: no slot');
	fill(1, 10);
	consistent('10 inserts');
	is(sql(q{SELECT string_agg(queryid::text, ',' ORDER BY idx)
	         FROM pssc_store_test_evict_slots()}), '1,2,3,4,5,6,7,8,9,10',
		'slots are appended in insertion order');
	rec(3) for 1 .. 4;
	is(sql('SELECT usage FROM pssc_store_test_evict_slots() WHERE queryid = 3'), 6,
		'updates raise the usage in the slot (1 + 5 calls)');
	my $b = sql('SELECT last_bucket FROM pssc_store_test_evict_slots() WHERE queryid = 3');
	pin($b + 1);
	rec(4);
	is(sql('SELECT last_bucket FROM pssc_store_test_evict_slots() WHERE queryid = 4'), $b + 1,
		'a write in a newer bucket moves the slot\'s last_bucket');
	consistent('updates across buckets');
	sql('SELECT pssc_store_test_reset()');
	is(sql('SELECT count(*) FROM pssc_store_test_evict_slots()'), 0,
		'reset empties the array');
	consistent('after reset');
}

# ------------------------------------------------------- sequential churn
{
	fresh();
	fill(1, 1000);
	consistent('1000 keys through max_entries=100 (live evictions)');
	is(sql(q{SELECT string_agg(queryid::text, ',' ORDER BY queryid)
	         FROM pssc_store_test_evict_slots()}),
		join(',', 901 .. 1000), 'the slots hold the 100 newest keys');
}

# ------------------------------------- dead first, then live, then mixed
{
	my $b = fresh();
	fill(1, 30, 10);
	pin($b + 12);
	fill(101, 170);
	rec(999);
	is(sql(q{SELECT reclaimed_entries || ' ' || evicted_entries FROM pssc_store_test_counters()}),
		'30 0', 'a pass reclaimed the 30 dead entries');
	consistent('after reclaiming dead entries');

	$b = fresh();
	fill(1, 2, 10);
	pin($b + 12);
	fill(101, 103);
	fill(104, 198, 2);
	rec(999);
	is(sql(q{SELECT reclaimed_entries || ' ' || evicted_entries FROM pssc_store_test_counters()}),
		'2 3', 'a mixed pass: 2 dead, 3 live');
	consistent('after a mixed pass');
	is(sql(q{SELECT string_agg(queryid::text, ',' ORDER BY queryid)
	         FROM pssc_store_test_evict_slots() WHERE queryid < 104}), '',
		'the reclaimed and evicted keys left the array');

	# Dead and live entries interleaved in the array: every slot moved
	# into a hole must be judged as well. Even keys go dead, odd ones live.
	$b = fresh();
	fill(1, 100);
	pin($b + 11);
	is(sql(q{SELECT count(*) FROM generate_series(1, 99, 2) q
	         WHERE pssc_store_test_record(q) <> 'updated'}), 0, 'odd keys written again');
	pin($b + 12);
	rec(999);
	is(sql(q{SELECT reclaimed_entries || ' ' || evicted_entries FROM pssc_store_test_counters()}),
		'50 0', 'the 50 dead (even) keys were all reclaimed, no live one evicted');
	is(sql(q{SELECT string_agg(queryid::text, ',' ORDER BY queryid)
	         FROM pssc_store_test_evict_slots()}),
		join(',', (grep { $_ % 2 } 1 .. 100), 999), 'only the live keys and the new one remain');
	consistent('interleaved dead and live slots');
}

# ------------------------------- forced collisions and a failed allocation
{
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(true)');
	fresh();
	fill(1, 300);
	consistent('one dynahash chain');
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(false)');

	my $b = fresh();
	fill(1, 3);
	pin($b + 12);
	fill(101, 197);
	is(sql(q{SELECT pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(999)}), '|inserted',
		'allocation failure: dead entries are still reclaimed');
	consistent('reclaim without a candidate buffer');
}

# ------------------------------------------- shared memory accounting
{
	my $size = sub {
		return sql(qq{SELECT size FROM pg_shmem_allocations WHERE name = '$P'});
	};
	my $s100 = $size->();
	configure(max_entries => 1100);
	my $s1100 = $size->();
	is($s1100 - $s100, 1000 * 24,
		'the store header allocation grows by 24 bytes per max_entries (the array)');
	is(sql('SELECT shmem_bytes FROM pssc_store_test_counters()'),
		sql('SELECT pssc_store_test_shmem_size_for(1100, 512, 12)'),
		'shmem_bytes still equals the formula');
	configure(max_entries => 100);
}

# ------------------------- concurrent updates: no lost usage (pgbench)
# The usage lives only in the slot, and record updates it under the entry
# spinlock with the table lock shared. With the clock pinned and fewer keys
# than max_entries there is no pass, so no decay: each key's usage must be
# exactly 1 (insert) + its successful records. The oracle is an ordinary
# table that the same statement fills only when the record succeeded
# (found_late: a racing backend inserted the key first, and this record
# counted as an update).
sql('CREATE TABLE usage_oracle (q bigint)');
for my $collide ('f', 't')
{
	sql('TRUNCATE usage_oracle');
	sql('SELECT pssc_store_test_reset()');
	sql("SELECT pssc_store_test_force_collisions('$collide')");
	fresh();
	my $file = $node->basedir . '/pgbench_usage.sql';
	open my $fh, '>', $file or die "open $file: $!";
	print $fh q{\set q random(1, 20)
INSERT INTO usage_oracle SELECT :q
 WHERE pssc_store_test_record(:q, '{}', NULL, 0.5) IN ('inserted', 'updated', 'found_late');
};
	close $fh;
	my ($out, $err);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	IPC::Run::run([ 'pgbench', '-n', '-c', 8, '-j', 8, '-t', 1000, '-f', $file, 'postgres' ],
		'>', \$out, '2>', \$err)
	  or die "pgbench failed: $err";
	like($out, qr{number of transactions actually processed: 8000/8000},
		"usage, collisions=$collide: pgbench ran every transaction");
	is(sql('SELECT count(*) FROM usage_oracle'), 8000,
		"usage, collisions=$collide: every record succeeded");
	is(sql('SELECT dealloc FROM pssc_store_test_counters()'), 0,
		"usage, collisions=$collide: no eviction pass, so no decay");
	is(sql(q{SELECT count(*) || ' ' || sum(usage) FROM pssc_store_test_evict_slots()}),
		'20 8020', "usage, collisions=$collide: total usage = records + entries");
	is(sql(q{SELECT count(*) FROM
	           (SELECT q, count(*) + 1 AS want FROM usage_oracle GROUP BY q) o
	           FULL JOIN pssc_store_test_evict_slots() s ON s.queryid = o.q
	         WHERE s.usage IS DISTINCT FROM o.want}), 0,
		"usage, collisions=$collide: each key's usage = 1 + its own records");
	is(sql(q{SELECT count(*) FROM
	           (SELECT q, count(*) AS want FROM usage_oracle GROUP BY q) o
	           FULL JOIN (SELECT queryid, sum(calls) AS calls
	                        FROM pssc_store_test_entries() GROUP BY queryid) e
	             ON e.queryid = o.q
	         WHERE e.calls IS DISTINCT FROM o.want}), 0,
		"usage, collisions=$collide: each key's calls = its own records");
	consistent("usage, collisions=$collide");
}
sql('DROP TABLE usage_oracle');
sql('SELECT pssc_store_test_reset()');
sql('SELECT pssc_store_test_force_collisions(false)');

# --------------------------------------- concurrent churn (pgbench)
configure(bucket_count => 2);
for my $collide ('f', 't')
{
	sql('SELECT pssc_store_test_reset()');
	sql("SELECT pssc_store_test_force_collisions('$collide')");
	fresh();
	my @args;
	my $i = 0;
	for my $s (
		[ q{\set q random(1, 2000)
SELECT pssc_store_test_record(:q, '{}', NULL, 0.5);
}, 90 ],
		[ q{SELECT pssc_store_test_check_invariants();
}, 5 ],
		[ q{SELECT pssc_store_test_advance_clock(interval_us) FROM pssc_store_test_buckets();
}, 5 ])
	{
		my $file = $node->basedir . '/pgbench_slots_' . $i++ . '.sql';
		open my $fh, '>', $file or die "open $file: $!";
		print $fh $s->[0];
		close $fh;
		push @args, '-f', "$file\@$s->[1]";
	}
	my ($out, $err);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	IPC::Run::run([ 'pgbench', '-n', '-c', 8, '-j', 8, '-t', 1000, @args, 'postgres' ],
		'>', \$out, '2>', \$err)
	  or die "pgbench failed: $err";
	like($out, qr{number of transactions actually processed: 8000/8000},
		"collisions=$collide: pgbench ran every transaction");
	my ($dealloc, $reclaimed, $evicted) = split /\|/,
	  sql('SELECT dealloc, reclaimed_entries, evicted_entries FROM pssc_store_test_counters()');
	ok($dealloc > 0 && $reclaimed > 0,
		"collisions=$collide: passes reclaimed dead entries under load "
		  . "($dealloc passes, $reclaimed reclaimed, $evicted evicted)");
	consistent("collisions=$collide: after concurrent churn");
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(false)');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|terminated by signal|TRAP/,
	'server log has no crash');

done_testing();
