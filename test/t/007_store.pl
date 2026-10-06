# Shared store core (DESIGN.md §3.1 item 4, §5.1, §5.4, §9; backlog
# 20261005-091225-13): shared memory sizing and request, the hash table with
# custom hash/compare, the per-entry bucket ring, locking under concurrent
# recording (pgbench), max_entries enforcement (eviction itself is
# test/t/009_eviction.pl), forced hash collisions,
# header counters and reset. The store is driven through the TEST-ONLY
# module test/modules/pssc_store_test (make install-test-modules).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('store');
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
sub counters
{
	my @cols = qw(entries max_entries dealloc evicted_entries invalid_tags
	  dropped_tags regex_compile_failures heuristic_scans
	  utility_missing_queryid dropped_records stats_reset shmem_bytes keysize
	  entrysize bucket_count max_tagset_bytes force_collisions hash_entries);
	my @v = split /\|/, sql('SELECT * FROM pssc_store_test_counters()'), -1;
	my %c;
	@c{@cols} = @v;
	return \%c;
}
# ALTER SYSTEM the postmaster settings (undef: RESET) and restart.
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
# Slots of the entries, "queryid:tags:slot:bucket:calls:time" per row.
sub slots
{
	my ($where) = @_;
	$where //= 'true';
	return sql(qq{SELECT string_agg(format('%s:%s:%s:%s:%s:%s', queryid,
	                    array_to_string(tags, ','), slot, bucket_id, calls,
	                    total_exec_time), ' ' ORDER BY queryid, tags, slot)
	               FROM pssc_store_test_entries() WHERE $where});
}
sub nentries
{
	return sql(q{SELECT count(*) FROM (SELECT DISTINCT dbid, userid, queryid,
	             toplevel, tags FROM pssc_store_test_entries()) e});
}
# Shared memory accounting from pg_stat_statement_context's point of view:
# the whole segment (named + anonymous + free) and the free part.
sub shmem_view
{
	my ($total, $free) = split /\|/, sql(q{SELECT sum(allocated_size),
	    sum(allocated_size) FILTER (WHERE name IS NULL) FROM pg_shmem_allocations});
	return ($total, $free);
}
sub pgbench
{
	my ($script, $clients, $txns) = @_;
	my $file = $node->basedir . '/pgbench_store.sql';
	open my $fh, '>', $file or die "open $file: $!";
	print $fh $script;
	close $fh;
	my ($out, $err);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	IPC::Run::run([ 'pgbench', '-n', '-c', $clients, '-j', $clients,
			'-t', $txns, '-f', $file, 'postgres' ],
		'>', \$out, '2>', \$err)
	  or die "pgbench failed: $err";
	my $n = $clients * $txns;
	like($out, qr{number of transactions actually processed: $n/$n},
		"pgbench ran all $n transactions");
	return $n;
}

# ---------------------------------------------------------------- defaults
{
	my $c = counters();
	is("$c->{max_entries} $c->{bucket_count} $c->{max_tagset_bytes}",
		'10000 12 512', 'defaults: max_entries, bucket_count, max_tagset_bytes');
	is("$c->{entries} $c->{hash_entries} $c->{dropped_records}", '0 0 0',
		'defaults: the table starts empty');
	# offsetof(PsscKey, tags) = 24 on every supported (64-bit) platform
	is($c->{keysize}, 536, 'keysize = MAXALIGN(offsetof(PsscKey, tags) + max_tagset_bytes)');
	is($c->{keysize}, sql('SELECT pssc_store_test_keysize_for(512)'),
		'keysize matches pssc_store_keysize_for()');
	is($c->{entrysize}, sql("SELECT pssc_store_test_entrysize_for($c->{keysize}, 12)"),
		'entrysize matches pssc_store_entrysize_for()');
	my $ring = 12 * 24;
	ok($c->{entrysize} >= $c->{keysize} + $ring
		  && $c->{entrysize} <= $c->{keysize} + $ring + 64,
		"entrysize ($c->{entrysize}) = keysize + entry header + 12 slots of 24 bytes");
	is($c->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size()'),
		'reported shmem_bytes equals the size requested for the current settings');
	is($c->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(10000, 512, 12)'),
		'shmem_bytes equals the formula for the default settings');
	ok($c->{shmem_bytes} > 10000 * $c->{entrysize},
		'shmem_bytes covers max_entries preallocated entries');
	isnt($c->{stats_reset}, '', 'stats_reset is set at startup');
	is($c->{force_collisions}, 'f', 'forced collisions are off by default');
}

# ------------------------------------- requested size is what is allocated
{
	configure(max_entries => 100);
	my $c1 = counters();
	my ($t1, $f1) = shmem_view();
	configure(max_entries => 5000);
	my $c2 = counters();
	my ($t2, $f2) = shmem_view();
	is($c2->{max_entries}, 5000, 'max_entries = 5000 in effect');
	my $req = $c2->{shmem_bytes} - $c1->{shmem_bytes};
	ok($req > 4900 * $c2->{entrysize}, "requested size grows with max_entries ($req bytes)");
	# The segment is rounded up to 8 kB, so the totals may differ by less.
	ok(abs(($t2 - $t1) - $req) < 8192,
		'the shared memory segment grows by exactly the requested difference '
		  . "(segment +" . ($t2 - $t1) . ", requested +$req)");
	# Using more than requested would eat into the core's free slack.
	ok($f2 - $f1 > -8192,
		'the store allocates no more than it requested (free space: '
		  . ($f2 - $f1) . ')');
	is(sql(qq{SELECT string_agg(name, ',' ORDER BY name COLLATE "C")
	            FROM pg_shmem_allocations WHERE name LIKE '$P%'}),
		"$P,$P activity,$P hash",
		'named allocations: the store header and hash table, and the activity slots');
}

# --------------------------------------------------------- boundary values
{
	configure(max_entries => 100, max_tagset_bytes => 128, bucket_count => 1);
	my $c = counters();
	is("$c->{max_entries} $c->{max_tagset_bytes} $c->{bucket_count}", '100 128 1',
		'minimum settings in effect');
	is($c->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(100, 128, 1)'),
		'minimum settings: shmem_bytes equals the formula');
	is($c->{keysize}, 152, 'minimum settings: keysize');
	# "a\0" + 125 x "v" + "\0" = 128 bytes
	is(rec(q{1, ARRAY['a', repeat('v', 125)], 0}), 'inserted', 'a 128-byte tag set fits');
	is(rec(q{1, ARRAY['a', repeat('v', 126)]}), 'rejected', 'a 129-byte tag set is rejected');
	is(rec(q{1, ARRAY['a', repeat('v', 125)], 1}), 'updated', 'bucket 1, one-slot ring');
	is(slots(), "1:a,@{['v' x 125]}:0:1:1:1", 'one slot: bucket 1 relabeled the slot of bucket 0');

	configure(max_tagset_bytes => 8192, bucket_count => 10000);
	$c = counters();
	is("$c->{max_entries} $c->{max_tagset_bytes} $c->{bucket_count}", '100 8192 10000',
		'maximum max_tagset_bytes and bucket_count in effect');
	is($c->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(100, 8192, 10000)'),
		'maximum settings: shmem_bytes equals the formula');
	is($c->{keysize}, 8216, 'maximum settings: keysize');
	is(rec(q{1, ARRAY['a', repeat('v', 8189)]}), 'inserted', 'an 8192-byte tag set fits');
	is(rec(q{1, ARRAY['a', repeat('v', 8190)]}), 'rejected', 'an 8193-byte tag set is rejected');
	is(rec(q{2, '{}', 0}), 'inserted', 'empty tag set, bucket 0');
	is(rec(q{2, '{}', 9999}), 'updated', 'bucket 9999');
	is(slots('queryid = 2'), '2::0:0:1:1 2::9999:9999:1:1', 'buckets 0 and 9999 in slots 0 and 9999');
	is(rec(q{2, '{}', 10000}), 'updated', 'bucket 10000');
	is(slots('queryid = 2'), '2::0:10000:1:1 2::9999:9999:1:1',
		'bucket 10000 relabels slot 0 (older bucket 0), slot 9999 kept');

	# The largest settings: the size is computed without overflow.
	my $max = sql(qq{SELECT pssc_store_test_shmem_size_for(1073741823, 8192, 10000)
	                 >= 1073741823::numeric * pssc_store_test_entrysize_for(8216, 10000)});
	is($max, 't', 'shmem size for the maximum settings is computed without overflow');

	configure(max_entries => 20000, max_tagset_bytes => undef, bucket_count => undef);
	$c = counters();
	is("$c->{max_entries} $c->{max_tagset_bytes} $c->{bucket_count}", '20000 512 12',
		'a larger max_entries in effect');
	is($c->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(20000, 512, 12)'),
		'larger max_entries: shmem_bytes equals the formula');
	is(sql(q{SELECT count(*) FROM generate_series(1, 20001) q
	         WHERE pssc_store_test_record(q) = 'inserted'}), 20001,
		'20001 entries inserted');
	$c = counters();
	# the 20001st insert ran one eviction pass (§5.3): 5% of 20000 freed
	is("$c->{entries} $c->{hash_entries} $c->{dealloc} $c->{evicted_entries} "
		  . "$c->{dropped_records}", '19001 19001 1 1000 0',
		'the 20001st insert evicts 1000 entries (5%) and succeeds');
}

# ------------------------------------------------- key, entry and the ring
configure(max_entries => undef);
{
	is(rec(q{1, ARRAY['a', '1'], 3, 1.5}), 'inserted', 'first record inserts');
	is(rec(q{1, ARRAY['a', '1'], 3, 2.5}), 'updated', 'second record updates (fast path)');
	is(rec(q{1, ARRAY['a', '1'], 4, 4}), 'updated', 'another bucket updates the same entry');
	is(nentries(), 1, 'same key in two buckets: one entry');
	is(slots(), '1:a,1:3:3:2:4 1:a,1:4:4:1:4', 'separate slots per bucket');
	is(sql('SELECT DISTINCT last_bucket || \' \' || usage FROM pssc_store_test_entries()'),
		'4 4', 'last_bucket = newest bucket; usage = 1 + 1 per call');

	is(rec(q{1, ARRAY['a', '1'], 15, 8}), 'updated', 'bucket 15 (slot 3)');
	is(slots(), '1:a,1:3:15:1:8 1:a,1:4:4:1:4',
		'an older slot is zeroed and relabeled before adding; others kept');
	is(rec(q{1, ARRAY['a', '1'], 10, 1}), 'updated', 'an id older than current_bucket');
	is(slots(), '1:a,1:3:15:2:9 1:a,1:4:4:1:4',
		'an id older than current_bucket is clamped to current_bucket (15)');

	my $oid2 = sql(q{SELECT oid FROM pg_database WHERE datname = 'template1'});
	is(rec(q{1, ARRAY['a', '1'], 3, 1, false}), 'inserted', 'toplevel = false: new entry');
	is(rec(q{2, ARRAY['a', '1'], 3}), 'inserted', 'other queryid: new entry');
	is(rec(q{1, ARRAY['a', '2'], 3}), 'inserted', 'other tag value: new entry');
	is(rec(q{1, ARRAY['a', '1', 'b', '2'], 3}), 'inserted', 'more tags: new entry');
	is(rec(q{1, '{}', 3}), 'inserted', 'empty tag set: new entry');
	is(rec(qq{1, ARRAY['a', '1'], 3, 1, true, NULL, $oid2}), 'inserted', 'other dbid: new entry');
	is(rec(q{1, ARRAY['a', '1'], 3, 1, true, NULL, NULL, 424242}), 'inserted', 'other userid: new entry');
	is(nentries(), 8, 'eight distinct keys, eight entries');
	is(counters()->{entries}, 8, 'header entries = 8');

	sql(q{CREATE DATABASE l1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0});
	$node->safe_psql('l1', 'CREATE EXTENSION pssc_store_test');
	is($node->safe_psql('l1', q{SELECT pssc_store_test_record(9, ARRAY['k', 'v'])}),
		'inserted', 'record from a LATIN1 database');
	is(sql(q{SELECT string_agg(DISTINCT pg_encoding_to_char(encoding), ',')
	         FROM pssc_store_test_entries() WHERE queryid = 9}),
		'LATIN1', 'entry stores the encoding of its database');
	is(sql(q{SELECT string_agg(DISTINCT pg_encoding_to_char(encoding), ',')
	         FROM pssc_store_test_entries() WHERE queryid = 2}),
		sql('SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = current_database()'),
		'entry stores the encoding of its database (postgres)');
}

# ------------------------------------------------------- counters and reset
{
	my $before = counters();
	sql('SELECT pssc_store_test_add_stats(1, 2, 3, 4)');
	sql('SELECT pssc_store_test_add_stats(10, 20, 30, 40)');
	sql('SELECT pssc_store_test_utility_missing_queryid()') for 1 .. 3;
	my $c = counters();
	is("$c->{invalid_tags} $c->{dropped_tags} $c->{heuristic_scans} "
		  . "$c->{regex_compile_failures} $c->{utility_missing_queryid}",
		'11 22 33 44 3', 'header counters accumulate across sessions');
	is("$c->{dealloc} $c->{evicted_entries}", '0 0', 'no eviction yet');

	sql('SELECT pssc_store_test_reset()');
	$c = counters();
	is("$c->{entries} $c->{hash_entries} $c->{invalid_tags} $c->{dropped_tags} "
		  . "$c->{heuristic_scans} $c->{regex_compile_failures} "
		  . "$c->{utility_missing_queryid} $c->{dropped_records} $c->{dealloc} "
		  . "$c->{evicted_entries}",
		'0 0 0 0 0 0 0 0 0 0', 'reset zeroes entries and counters');
	is(sql('SELECT count(*) FROM pssc_store_test_entries()'), 0, 'reset removes all entries');
	is(sql(qq{SELECT '$c->{stats_reset}'::timestamptz > '$before->{stats_reset}'::timestamptz}),
		't', 'reset advances stats_reset');
	is(rec(q{1, ARRAY['a', '1'], 3}), 'inserted', 'records again after reset');
	sql('SELECT pssc_store_test_reset()');
}

# ----------------------------------------------------- forced collisions
{
	is(sql(q{SELECT count(DISTINCT pssc_store_test_key_hash(q, ARRAY['a', q::text], q % 2 = 0))
	         FROM generate_series(1, 20) q}), 20,
		'without forced collisions, distinct keys hash differently');
	is(sql(q{SELECT pssc_store_test_key_hash(1, ARRAY['a', '1'], true, 42)
	              = pssc_store_test_key_hash(1, ARRAY['a', '2'], true, 42)}), 't',
		'the hash combines the fixed fields with tags_hash (not the tag bytes)');
	rec(q{1});
	my ($ok, $out, $err) = $node->psql('postgres', 'SELECT pssc_store_test_force_collisions(true)');
	like($err, qr/not empty/, 'forced collisions cannot be switched on a non-empty table');
	sql('SELECT pssc_store_test_reset()');
	sql('SELECT pssc_store_test_force_collisions(true)');
	is(counters()->{force_collisions}, 't', 'forced collisions on (shared header)');
	is(sql(q{SELECT count(DISTINCT pssc_store_test_key_hash(q, ARRAY['a', q::text], q % 2 = 0, q))
	         FROM generate_series(1, 20) q}), 1, 'with forced collisions every key hashes alike');

	# Distinct tag sets with the same tags_hash (42) and the same hash value.
	my @sets = (q{'{}'}, q{ARRAY['a', '1']}, q{ARRAY['a', '2']}, q{ARRAY['a', 'b']},
		q{ARRAY['ab', '']}, q{ARRAY['a', '1', 'b', '2']}, q{ARRAY['a', '1', 'b', '3']},
		q{ARRAY['b', '1']});
	for my $i (0 .. $#sets)
	{
		my $n = $i + 1;
		for (1 .. $n)
		{
			sql("SELECT pssc_store_test_record(7, $sets[$i], 0, 1, true, 42)");
		}
	}
	is(nentries(), scalar(@sets), 'colliding distinct tag sets stay separate entries');
	is(sql(q{SELECT string_agg(array_to_string(tags, ',') || '=' || calls, ' ' ORDER BY calls)
	         FROM pssc_store_test_entries()}),
		'=1 a,1=2 a,2=3 a,b=4 ab,=5 a,1,b,2=6 a,1,b,3=7 b,1=8',
		'each colliding entry kept its own calls');
	is(rec(q{8, ARRAY['a', '1'], 0, 1, true, 42}), 'inserted', 'colliding key, other queryid');
	is(rec(q{7, ARRAY['a', '1'], 0, 1, false, 42}), 'inserted', 'colliding key, other toplevel');
	is(rec(q{7, ARRAY['a', '1'], 0, 1, true, 43}), 'inserted', 'colliding key, other tags_hash');
	is(rec(q{7, ARRAY['a', '1'], 0, 1, true, 42}), 'updated', 'colliding key, existing entry found');
	is(counters()->{entries}, scalar(@sets) + 3, 'entry count with collisions');
}

# ------------- collision mode switched between hashing and locking (race)
for my $to ('t', 'f')
{
	my $from = $to eq 't' ? 'f' : 't';
	sql('SELECT pssc_store_test_reset()');
	sql("SELECT pssc_store_test_force_collisions('$from')");
	is(sql(qq{SELECT pssc_store_test_flip_collisions_in_next_record('$to'),
	                 pssc_store_test_record(5, ARRAY['r', '1'])}), '|inserted',
		"collisions $from->$to while a record stalls after hashing: inserted");
	is(counters()->{force_collisions}, $to, "collisions now $to");
	is(rec(q{5, ARRAY['r', '1']}), 'updated',
		"collisions $from->$to: the entry is found under the new hash mode");
	is(sql(q{SELECT count(*) || ' ' || sum(calls) FROM pssc_store_test_entries()}), '1 2',
		"collisions $from->$to: one entry, no duplicate");
	sql('SELECT pssc_store_test_reset()');
	my $c = counters();
	is("$c->{entries} $c->{hash_entries}", '0 0',
		"collisions $from->$to: reset removes the entry");
}
sql('SELECT pssc_store_test_force_collisions(false)');

# ---------------------------------------- concurrent recording (pgbench)
for my $collide ('f', 't')
{
	sql('SELECT pssc_store_test_reset()');
	sql("SELECT pssc_store_test_force_collisions('$collide')");
	my $n = pgbench(q{\set q random(1, 20)
SELECT pssc_store_test_record(:q, ARRAY['k', (:q % 3)::text], 7, 0.5);
}, 8, 1000);
	is(sql('SELECT sum(calls) || \' \' || sum(total_exec_time) FROM pssc_store_test_entries()'),
		"$n " . ($n / 2), "collisions=$collide: no lost updates (calls and time)");
	is(sql('SELECT count(*) FROM pssc_store_test_entries()'), 20,
		"collisions=$collide: one entry per key, no duplicates from racing inserts");
	is(counters()->{entries}, 20, "collisions=$collide: header entries");
}
sql('SELECT pssc_store_test_reset()');
sql('SELECT pssc_store_test_force_collisions(false)');

# ------------------------------------------------------------ max_entries
configure(max_entries => 100);
{
	is(sql(q{SELECT string_agg(r || ':' || n, ' ' ORDER BY r) FROM
	         (SELECT pssc_store_test_record(q) r, count(*) n
	            FROM generate_series(1, 150) q GROUP BY 1) s}),
		'inserted:150', 'a full table evicts to make room for new keys');
	my $c = counters();
	is("$c->{entries} $c->{hash_entries} $c->{dealloc} $c->{evicted_entries} "
		  . "$c->{dropped_records}", '100 100 10 50 0',
		'entries capped at max_entries; 10 passes of 5 evictions, nothing dropped');
	is(rec(q{150}), 'updated', 'existing keys are still recorded when full');

	sql('SELECT pssc_store_test_reset()');
	my $n = pgbench(q{\set q random(1, 1000)
SELECT pssc_store_test_record(:q);
}, 8, 500);
	$c = counters();
	ok($c->{entries} >= 96 && $c->{entries} <= 100 && $c->{entries} == $c->{hash_entries},
		"concurrent inserts never exceed max_entries ($c->{entries})");
	is($c->{dropped_records}, 0, 'concurrent records into a full table are not dropped');
	ok(sql('SELECT sum(calls) FROM pssc_store_test_entries()') <= $n,
		'stored calls do not exceed the records');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|terminated by signal|TRAP/,
	'server log has no crash');

# ----------------------------------------------------- not preloaded
{
	my $np = PostgreSQL::Test::Cluster->new('store_nopreload');
	$np->init;
	$np->start;
	$np->safe_psql('postgres', 'CREATE EXTENSION pssc_store_test');
	is($np->safe_psql('postgres', 'SELECT pssc_store_test_record(1)'), 'unavailable',
		'without shared_preload_libraries the store is unavailable, no crash');
	$np->stop;
}

done_testing();
