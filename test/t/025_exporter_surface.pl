# The exporter-friendly SQL surface (DESIGN.md §5.1, §7; backlog
# 20261006-010149-1): the monotonic per-entry counters calls_total,
# exec_time_total and stats_since of the stats SRF and its views, and the
# view pg_stat_statement_context_last_bucket.
#
# Covers: the per-entry counters grow with every call whatever bucket it
# lands in, survive the expiry of the slots that counted them, repeat on
# every bucket row of an entry and appear once per entry in _totals,
# continue when a dead but not yet reclaimed entry is written again, start
# over (with a newer stats_since) when the entry was reclaimed or evicted,
# and are cleared by _reset(); they move through the hooks too, and lose no
# update under concurrent pgbench writers, also while the ring expires and
# is rewritten under them. The _last_bucket view shows only the bucket
# before the store's current one (_info().last_closed_bucket_start), only
# entries with a live slot there, follows the watermark (not a clock that
# stepped back), honours showtags, hides other roles' queryid and tags
# without pg_read_all_stats, and is readable by PUBLIC. The bucket in
# progress at startup is partial (it starts at the aligned boundary before
# stats_reset) and becomes the last closed one at the first aligned
# boundary. Entries and the clock are driven through the TEST-ONLY module
# test/modules/pssc_store_test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('exporter_surface');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
$P.max_entries = 100
$P.bucket_count = 4
$P.tags = '*'
$P.exclude_tags = ''
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub rec { return sql("SELECT pssc_store_test_record($_[0])"); }

sql("CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");
sql('CREATE ROLE alice; CREATE ROLE monitor LOGIN');

# Pin the shared debug clock in the middle of bucket $b.
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}

# Rows of $from as "b<bucket offset from $base>|queryid|tags|calls|
# total_exec_time|calls_total|exec_time_total", sorted.
sub rows
{
	my ($from, $base, $where) = @_;
	$where //= 'true';
	return sql(qq{SELECT coalesce(string_agg(concat_ws('|',
	                 'b' || coalesce((SELECT (id - $base)::text
	                                  FROM generate_series($base - 10, $base + 10) id
	                                  WHERE pssc_store_test_bucket_start(id) = r.bucket_start), '?'),
	                 coalesce(r.queryid::text, 'NULL'), coalesce(r.tags::text, 'NULL'),
	                 r.calls, r.total_exec_time, r.calls_total, r.exec_time_total), E'\n'
	               ORDER BY r.queryid, r.tags::text, r.bucket_start), '')
	             FROM $from r WHERE $where});
}

# ------------------------------------------------------------- catalog
{
	like(sql(qq{SELECT pg_get_viewdef('${P}_last_bucket'::regclass)}),
		qr/\Q${P}_last_bucket\E\(true\)/, '_last_bucket view reads its function with showtags');
	is(sql(qq{SELECT pg_get_function_arguments(p.oid) || ' ' || concat_ws(' ', provolatile,
	                 proparallel, proisstrict, proretset)
	          FROM pg_proc p WHERE proname = '${P}_last_bucket'}),
		'showtags boolean DEFAULT true, OUT bucket_start timestamp with time zone, '
		  . 'OUT userid oid, OUT dbid oid, OUT queryid bigint, OUT toplevel boolean, '
		  . 'OUT tags jsonb, OUT calls bigint, OUT total_exec_time double precision, '
		  . 'OUT calls_total bigint, OUT exec_time_total double precision, '
		  . 'OUT stats_since timestamp with time zone, OUT exemplars jsonb v s t t',
		'_last_bucket(): the SRF columns; VOLATILE PARALLEL SAFE STRICT like the SRF');
	is(sql(qq{SELECT has_table_privilege('monitor', '${P}_last_bucket', 'SELECT'),
	                 has_function_privilege('monitor', '${P}_last_bucket(boolean)', 'EXECUTE')}),
		't|t', '_last_bucket is readable by PUBLIC');
}

# ------------------------------------------------ monotonic counters
my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
{
	sql("SELECT ${P}_reset()");
	my $t0 = sql('SELECT clock_timestamp()');
	pin($b);
	rec(q{1, ARRAY['a', '1'], NULL, 1.5}) for 1 .. 2;
	rec(q{2, '{}', NULL, 2});
	my $t1 = sql('SELECT clock_timestamp()');
	is(rows("${P}_totals", $b), join("\n",
			"b0|1|{\"a\": \"1\"}|2|3|2|3",
			"b0|2|{}|1|2|1|2"),
		'one bucket: calls_total and exec_time_total equal the bucket counters');
	is(sql(qq{SELECT bool_and(stats_since BETWEEN '$t0' AND '$t1') FROM ${P}_totals}), 't',
		'stats_since is the time the entry was created');
	my $since1 = sql(qq{SELECT stats_since FROM ${P}_totals WHERE queryid = 1});
	my $since2 = sql(qq{SELECT stats_since FROM ${P}_totals WHERE queryid = 2});

	pin($b + 1);
	rec(q{1, ARRAY['a', '1'], NULL, 3});
	is(rows($P, $b), join("\n",
			"b0|1|{\"a\": \"1\"}|2|3|3|6",
			"b1|1|{\"a\": \"1\"}|1|3|3|6",
			"b0|2|{}|1|2|1|2"),
		'per-bucket view: the entry\'s counters repeat on each of its bucket rows');
	is(rows("${P}_totals", $b), join("\n",
			"b0|1|{\"a\": \"1\"}|3|6|3|6",
			"b0|2|{}|1|2|1|2"),
		'_totals: once per entry');
	is(sql(qq{SELECT stats_since = '$since1' FROM ${P}_totals WHERE queryid = 1}), 't',
		'stats_since does not move when the entry is updated');

	# bucket_count 4: at b+4 the window is [b+1, b+4]; slot b has expired.
	pin($b + 4);
	is(rows("${P}_totals", $b), "b1|1|{\"a\": \"1\"}|1|3|3|6",
		'expired slots leave the bucket sums but not calls_total/exec_time_total; '
		  . 'the dead entry 2 is hidden');
	rec(q{2, '{}', NULL, 4});
	is(rows("${P}_totals", $b, 'queryid = 2'), "b4|2|{}|1|4|2|6",
		'a dead entry written again before it was reclaimed continues its counters');
	is(sql(qq{SELECT stats_since = '$since2' FROM ${P}_totals WHERE queryid = 2}), 't',
		'and keeps its stats_since');

	# Reclaimed: entry 1 dies at b+5 (window [b+2, b+5]), the table fills,
	# and the next insert's eviction pass reclaims it.
	pin($b + 5);
	sql(q{SELECT count(*) FROM generate_series(10, 107) q, pssc_store_test_record(q)});
	is(sql(q{SELECT entries FROM pg_stat_statement_context_info()}), 100, 'the table is full');
	is(rec(q{500}), 'inserted', 'an insert runs an eviction pass');
	is(sql(qq{SELECT reclaimed_entries > 0 FROM ${P}_info()}), 't', 'it reclaimed entry 1');
	rec(q{1, ARRAY['a', '1'], NULL, 7});
	is(rows("${P}_totals", $b, 'queryid = 1'), "b5|1|{\"a\": \"1\"}|1|7|1|7",
		'a reclaimed entry starts over: its counters went with it');
	is(sql(qq{SELECT stats_since > '$since1' FROM ${P}_totals WHERE queryid = 1}), 't',
		'and its stats_since is the new entry\'s creation time');

	# Evicted while live: a full table of live entries, a new key evicts
	# the 5 least used (entry 600, recorded once, among them).
	sql("SELECT ${P}_reset()");
	pin($b + 6);
	is(sql(q{SELECT count(*) FROM generate_series(1, 99) q, generate_series(1, 3) n
	          WHERE pssc_store_test_record(q + 0 * n) NOT IN ('inserted', 'updated')}), 0,
		'99 entries recorded 3 times each');
	rec(q{600, '{}', NULL, 2});
	is(rows("${P}_totals", $b, 'queryid = 600'), "b6|600|{}|1|2|1|2", 'entry 600');
	is(rec(q{700}), 'inserted', 'an insert evicts live entries');
	is(sql(qq{SELECT evicted_entries FROM ${P}_info()}), 5, '5 live entries evicted');
	is(rows("${P}_totals", $b, 'queryid = 600'), '', 'entry 600 was evicted');
	rec(q{600, '{}', NULL, 5});
	is(rows("${P}_totals", $b, 'queryid = 600'), "b6|600|{}|1|5|1|5",
		'an evicted entry starts over when it comes back');

	# Reset clears everything.
	my $tr = sql('SELECT clock_timestamp()');
	sql("SELECT ${P}_reset()");
	rec(q{600, '{}', NULL, 1});
	is(rows("${P}_totals", $b), "b6|600|{}|1|1|1|1", 'after _reset(): counters start over');
	is(sql(qq{SELECT stats_since > '$tr' FROM ${P}_totals}), 't',
		'stats_since is after the reset');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ------------------------------------------------ through the hooks
{
	sql("SELECT ${P}_reset()");
	sql(q{SELECT 1 /*k='hooks'*/}) for 1 .. 3;
	is(sql(qq{SELECT calls_total || ' ' || (exec_time_total = total_exec_time)
	                 || ' ' || (exec_time_total > 0)
	          FROM ${P}_totals WHERE tags = '{"k": "hooks"}'}), '3 true true',
		'statements recorded by the hooks move calls_total and exec_time_total');
}

# ------------------------------------------------ _last_bucket
my $lb = $b + 100;
{
	sql("SELECT ${P}_reset()");
	my $alice = sql(q{SELECT 'alice'::regrole::oid});
	pin($lb);
	rec(q{1, ARRAY['a', '1'], NULL, 1}) for 1 .. 2;
	rec(q{2, '{}', NULL, 2});
	rec(qq{3, ARRAY['who', 'alice'], NULL, 4, true, NULL, NULL, $alice});
	pin($lb + 1);
	rec(q{1, ARRAY['a', '1'], NULL, 3}) for 1 .. 3;
	rec(q{4, '{}', NULL, 5});

	is(rows("${P}_last_bucket", $lb), join("\n",
			"b0|1|{\"a\": \"1\"}|2|2|5|11",
			"b0|2|{}|1|2|1|2",
			"b0|3|{\"who\": \"alice\"}|1|4|1|4"),
		'_last_bucket: only the bucket before the current one, only entries with a slot there');
	is(sql(qq{SELECT count(*) FILTER (WHERE bucket_start = i.last_closed_bucket_start)
	                 || '/' || count(*)
	          FROM ${P}_last_bucket, ${P}_info() i}), '3/3',
		'bucket_start is _info().last_closed_bucket_start');
	is(rows("${P}_last_bucket", $lb),
		rows($P, $lb, "bucket_start = pssc_store_test_bucket_start($lb)"),
		'the same rows as the per-bucket view for that bucket');

	pin($lb + 2);
	is(rows("${P}_last_bucket", $lb), join("\n",
			"b1|1|{\"a\": \"1\"}|3|9|5|11",
			"b1|4|{}|1|5|1|5"),
		'it moves with the current bucket');
	pin($lb);
	is(rows("${P}_last_bucket", $lb), join("\n",
			"b1|1|{\"a\": \"1\"}|3|9|5|11",
			"b1|4|{}|1|5|1|5"),
		'a backward clock step does not move it back (the watermark never decreases)');
	is(sql(qq{SELECT count(*) FILTER (WHERE tags IS NULL) || '/' || count(*)
	          FROM ${P}_last_bucket(false)}), '2/2', 'showtags = false: tags NULL');

	pin($lb + 6);
	is(sql(qq{SELECT count(*) FROM ${P}_last_bucket}), 0,
		'no slot in the last closed bucket: the view is empty');

	# Visibility: alice's row (bucket lb) as an unprivileged role.
	sql("SELECT ${P}_reset()");
	pin($lb + 7);
	rec(qq{3, ARRAY['who', 'alice'], NULL, 4, true, NULL, NULL, $alice});
	rec(q{5, ARRAY['who', 'me']});
	pin($lb + 8);
	my $as_monitor = sub {
		my ($ret, $out, $err) = $node->psql('postgres', $_[0],
			extra_params => [ '-U', 'monitor' ]);
		return $ret == 0 ? $out : "ERROR: $err";
	};
	my $q = qq{SELECT string_agg(coalesce(queryid::text, 'NULL') || ' '
	                             || coalesce(tags::text, 'NULL') || ' ' || calls, ',' ORDER BY calls)
	             FROM ${P}_last_bucket WHERE userid = '$alice'};
	is($as_monitor->($q), 'NULL NULL 1',
		'another role\'s queryid and tags are hidden without pg_read_all_stats');
	sql('GRANT pg_read_all_stats TO monitor');
	is($as_monitor->($q), '3 {"who": "alice"} 1', 'and shown with it');
	is(sql(q{SELECT count(*) FROM pg_stat_statement_context_last_bucket
	          WHERE queryid = 5 AND tags = '{"who": "me"}'}), 1, 'own rows are complete');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ------------------------------------- monotonic counters, concurrently
# pgbench clients hammer one entry (elapsed 0.25 ms, exact in binary, so the
# float sums are exact). calls_total/exec_time_total are bumped under the
# entry spinlock with the slot counters, so not one update may be lost:
# first in one bucket, then while 5% of the transactions step the clock by
# a quarter bucket, so the slots expire and are rewritten under the writers
# many times over (bucket_count 4) while the per-entry counters keep growing.
{
	my $file = $node->basedir . '/pgbench_monotonic.sql';
	my $quarter = sql('SELECT interval_us / 4 FROM pssc_store_test_buckets()');
	# records per transaction, back to back in one statement (more contention)
	my $per_txn = 50;
	my $record = qq{SELECT pssc_store_test_record(77, '{}', NULL, 0.25)
	                FROM generate_series(1, $per_txn);\n};
	my $run = sub {
		my ($script) = @_;
		open my $fh, '>', $file or die "open $file: $!";
		print $fh $script;
		close $fh;
		my ($clients, $txns) = (8, 200);
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
		return $n * $per_txn;
	};
	my $totals = sub {
		sql(qq{SELECT calls || ' ' || total_exec_time || ' ' || calls_total || ' '
		              || exec_time_total FROM ${P}_totals WHERE queryid = 77});
	};

	sql("SELECT ${P}_reset()");
	my $cb = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	pin($cb);
	my $n = $run->($record);
	is($totals->(), "$n " . $n / 4 . " $n " . $n / 4,
		'concurrent writers in one bucket: calls_total/exec_time_total exact, '
		  . 'equal to the bucket sums');

	pin($cb + 4);    # the window is [cb+1, cb+4]: every slot written so far expired
	is(sql(qq{SELECT calls_total || ' ' || exec_time_total FROM $P WHERE queryid = 77}), '',
		'ring expired: no live slot left (the per-bucket view shows no row)');
	my $m = $run->(qq{\\set r random(1, 100)
\\if :r <= 5
SELECT pssc_store_test_advance_clock($quarter);
\\endif
} . $record);
	my $cur = sql('SELECT current_bucket FROM pssc_store_test_buckets()');
	ok($cur - ($cb + 4) >= 8,
		'the slots were expired and rewritten during the run ('
		  . ($cur - $cb - 4) . ' bucket boundaries crossed)');
	my ($calls, $time, $ctot, $ttot) = split / /, $totals->();
	is("$ctot $ttot", ($n + $m) . ' ' . ($n + $m) / 4,
		'concurrent writers across expiry and rewrites: calls_total/exec_time_total exact');
	ok($calls < $m, "the bucket sums kept only the live slots ($calls of $m calls)");
	is(sql('SELECT pssc_store_test_check_invariants()'), 1, 'invariants hold');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ----------------------------------------- the first closed bucket
# Bucket boundaries fall on wall-clock multiples of bucket_interval, so the
# bucket in progress at startup started before it (a partial bucket) and
# closes at the first aligned boundary, less than a full interval after
# startup. With a 1h interval, the clock is pinned at stats_reset before the
# real clock can plausibly cross that boundary.
{
	$node->append_conf('postgresql.conf', "$P.bucket_interval = '1h'");
	$node->restart;
	sql(q{SELECT pssc_store_test_pin_clock(stats_reset) FROM pg_stat_statement_context_info()});
	my ($s, $c, $l) = split /\|/, sql(qq{SELECT stats_reset || '|' || current_bucket_start
	                                           || '|' || last_closed_bucket_start FROM ${P}_info()});
	is(sql(qq{SELECT '$c'::timestamptz = date_bin('1 hour', '$s'::timestamptz, '2000-01-01 00:00+00')
	              AND '$l'::timestamptz = '$c'::timestamptz - interval '1h'}), 't',
		'startup: the current bucket is the aligned one around stats_reset, '
		  . 'the last closed one predates startup');
	rec(5);
	is(sql(qq{SELECT count(*) FROM ${P}_last_bucket}), '0',
		'a call right after startup is not in the last closed bucket yet');
	sql(qq{SELECT pssc_store_test_pin_clock('$c'::timestamptz + interval '1h' - interval '1 microsecond')});
	is(sql(qq{SELECT count(*) FROM ${P}_last_bucket}), '0', 'nor just before the boundary');
	sql(qq{SELECT pssc_store_test_pin_clock('$c'::timestamptz + interval '1h')});
	is(sql(qq{SELECT bucket_start || ' ' || calls FROM ${P}_last_bucket WHERE queryid = 5}),
		"$c 1", 'at the first aligned boundary the startup bucket is the last closed one');
	is(sql(qq{SELECT last_closed_bucket_start = '$c' AND '$c'::timestamptz <= stats_reset
	              AND current_bucket_start - stats_reset < interval '1h'
	          FROM ${P}_info()}), 't',
		'it starts before stats_reset (partial) and closes less than an interval after startup');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/, 'no crash');

done_testing();
