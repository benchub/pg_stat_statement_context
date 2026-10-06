# Store, buckets, eviction and reconfiguration end to end, through real
# statements and the executor hooks (DESIGN.md §4, §5, §9; backlog
# 20261005-091225-23). The store primitives themselves are covered with
# the TEST-ONLY driver in 007_store.pl (sizing, forced collisions, header
# counters), 008_buckets.pl (ids, clamping, ring rollover, clock steps,
# stalled writers, a pgbench stress of the store API) and 009_eviction.pl
# (dead entries first, live order, decay, churn); the GUC check/assign
# machinery in 003_guc.pl, 004_extractors.pl and 006_regex.pl. This file
# checks what those cannot: that the hooks' recording path uses them as
# designed.
#
#   - Bucket rollover with a short bucket_interval set at startup: one
#     entry per combination, never re-inserted at a bucket boundary (its
#     usage keeps growing), whose ring slots roll over lazily; a call is
#     attributed to the bucket in which it completes (a statement or cursor
#     that spans a boundary), a stalled writer and a clock stepped back land
#     in the live current bucket.
#   - SIGHUP reconfiguration seen by recording: a bad extractor DSL is
#     rejected and both an existing and a new backend keep recording with
#     the old config; a good change makes an existing backend recompile its
#     regex.
#   - Restarts: no persistence in v1, so the store, its counters, the bucket
#     watermark and the debug clock start fresh; resizing through the
#     postmaster GUCs (max_entries, bucket_count, bucket_interval,
#     max_tags, max_tagset_bytes) takes effect for recording.
#   - A forced hash collision (every key in one dynahash chain) followed by
#     eviction and reinsertion of an evicted combination.
#   - Multi-client pgbench (simple and prepared protocol) recording tagged
#     statements while the clock crosses bucket boundaries: no call lost or
#     counted twice, one entry per combination.
# The clock is the shared debug clock of test/modules/pssc_store_test,
# pinned, so bucket boundaries are deterministic.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('store_reconfig');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
max_connections = 40
$P.bucket_interval = '1s'
$P.bucket_count = 3
});
$node->start;
$node->safe_psql('postgres', "CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub buckets
{
	my @cols = qw(epoch interval_us bucket_count current_bucket clock_bucket
	  reader_bucket now advances clock_mode clock_value);
	my @v = split /\|/, sql('SELECT * FROM pssc_store_test_buckets()'), -1;
	my %b;
	@b{@cols} = @v;
	return \%b;
}

my @info_cols = qw(entries max_entries dealloc evicted_entries buckets oldest_bucket
  shmem_bytes invalid_tags dropped_tags heuristic_scans regex_compile_failures
  utility_missing_queryid stats_reset);
sub info
{
	my $sel = join(" || '|' || ", map { "coalesce(${_}::text, 'NULL')" } @info_cols);
	my @v = split /\|/, sql("SELECT $sel FROM ${P}_info()"), -1;
	my %h;
	@h{@info_cols} = @v;
	return \%h;
}

# Pin the shared debug clock in the middle of bucket $b.
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}

# A bucket beyond any seen so far (current_bucket never decreases).
sub fresh_bucket { return buckets()->{reader_bucket} + 100; }

# Live buckets of the view rows whose tags->>'controller' is $c, as
# "<bucket id - $base>:<calls>", oldest first.
sub vrows
{
	my ($c, $base, $from) = @_;
	$from //= $P;
	return sql(qq{SELECT string_agg((id - $base) || ':' || calls, ' ' ORDER BY id)
	  FROM (SELECT round(extract(epoch FROM r.bucket_start - b.epoch) * 1000000
	                     / b.interval_us)::bigint AS id, r.calls
	          FROM $from r, pssc_store_test_buckets() b
	         WHERE r.tags->>'controller' = '$c') s});
}

# The stored slots (expired or not) of the entry tagged controller = $c, as
# "<bucket id - $base>:<calls>" by bucket, and its usage.
sub raw_slots
{
	my ($c, $base) = @_;
	return sql(qq{SELECT string_agg((bucket_id - $base) || ':' || calls, ' ' ORDER BY bucket_id)
	  FROM pssc_store_test_entries() WHERE tags = ARRAY['controller', '$c']});
}
sub usage
{
	return sql(qq{SELECT string_agg(DISTINCT usage::text, ',')
	  FROM pssc_store_test_entries() WHERE tags = ARRAY['controller', '$_[0]']});
}

# Run the tagged statement "SELECT 1 /*controller='$c'*/" $n times.
sub run_tagged
{
	my ($c, $n) = @_;
	sql(join('', map { "SELECT 1 /*controller='$c'*/;\n" } 1 .. ($n // 1)));
}

# ------------------------------- rollover through the hooks (1 s, 3 slots)
{
	my $b = buckets();
	is("$b->{interval_us} $b->{bucket_count}", '1000000 3',
		'bucket_interval = 1s and bucket_count = 3 set at startup');
	sql("SELECT ${P}_reset()");
	my $base = fresh_bucket();

	for my $i (0 .. 5)
	{
		pin($base + $i);
		run_tagged('roll', $i == 5 ? 2 : 1);
	}
	is(vrows('roll', $base), '3:1 4:1 5:2',
		'six buckets, three slots: the view shows the live window [base+3, base+5]');
	is(sql(qq{SELECT count(*) FROM (SELECT DISTINCT dbid, userid, queryid, toplevel, tags
	          FROM pssc_store_test_entries() WHERE tags = ARRAY['controller', 'roll']) e}),
		1, 'one entry for the combination');
	my $i = info();
	is("$i->{entries} $i->{dealloc} $i->{evicted_entries}", '1 0 0',
		'_info(): one entry, no eviction pass');
	is(usage('roll'), 8,
		'usage 1 + 7 calls: the entry was never re-inserted at a bucket boundary');
	is(raw_slots('roll', $base), '3:1 4:1 5:2',
		'each ring slot was relabeled when its bucket came round again');

	# Lazy rollover: no writes, so the expired slots stay stored.
	pin($base + 7);
	is(vrows('roll', $base), '5:2', 'clock at base+7: readers hide the expired slots');
	is(raw_slots('roll', $base), '3:1 4:1 5:2', 'without writes, nothing is rolled over');
	run_tagged('roll');
	is(raw_slots('roll', $base), '3:1 5:2 7:1',
		'a write at base+7 relabels only its own slot (that of base+4)');
	is(vrows('roll', $base), '5:2 7:1', 'view: base+5 and base+7');
	is(usage('roll'), 9, 'still the same entry');

	# A call is attributed to the bucket in which it completes.
	pin($base + 20);
	sql(qq{SELECT pssc_store_test_advance_clock(2 * interval_us)
	         FROM pssc_store_test_buckets() /*controller='long'*/});
	is(vrows('long', $base), '22:1',
		'a statement started in base+20 that completes in base+22 counts in base+22');
	# A PL/pgSQL cursor opened in base+30, fetched across the boundary and
	# left open is recorded at COMMIT, in base+31 (with track = all, at top
	# level, as pgss does; it inherits the DO's tags). The DO itself is
	# recorded in base+30. (An SQL DECLARE's inner query has no queryId
	# before PG18.)
	pin($base + 30);
	sql(qq{SET $P.track = 'all';
BEGIN;
DO \$\$DECLARE c refcursor := 'pc';
BEGIN OPEN c FOR SELECT g FROM generate_series(1, 3) g; END\$\$ /*controller='cur'*/;
FETCH 1 FROM pc;
SELECT pssc_store_test_advance_clock(interval_us) FROM pssc_store_test_buckets();
FETCH ALL FROM pc;
COMMIT;});
	is(vrows('cur', $base), '30:1 31:1',
		'a cursor opened in base+30 and finished at COMMIT in base+31 counts in base+31');

	# Stale-bucket insertion across a rollover: the record stalls after it
	# computed base+40 while another backend moves the clock past the whole
	# ring (base+44) and records; the stalled call lands in the live current
	# bucket, not in base+40's (expired) slot.
	pin($base + 40);
	run_tagged('stall');
	sql(qq{SELECT pssc_store_test_stall_next_record(4 * interval_us, 77)
	         FROM pssc_store_test_buckets();
	       SELECT 1 /*controller='stall'*/});
	is(buckets()->{current_bucket}, $base + 44, 'meanwhile current_bucket moved to base+44');
	is(vrows('stall', $base), '44:1',
		'the stalled call is in the live base+44, base+40 has expired');
	is(raw_slots('stall', $base), '40:1 44:1', 'base+40 kept its one earlier call');

	# The clock steps back: calls land in the current bucket, which stays live.
	pin($base - 1000);
	run_tagged('back', 2);
	is(vrows('back', $base), '44:2', 'clock 1000 buckets back: calls land in current_bucket');
	is(vrows('stall', $base), '44:1', 'and nothing became live again');
}

# ------------------------------------------- SIGHUP reconfiguration
{
	# A persistent session (one backend).
	my %s = (in => '', out => '', err => '');
	$s{h} = IPC::Run::start([ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d',
			$node->connstr('postgres') ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err}, IPC::Run::timeout(300));
	my $marker = 0;
	my $sq = sub {
		my ($q) = @_;
		my $m = '__pssc_done_' . ++$marker . '__';
		$s{out} = '';
		$s{err} = '';
		$s{in} .= "$q;\n\\echo $m\n";
		$s{h}->pump until $s{out} =~ /^\Q$m\E$/m;
		(my $r = $s{out}) =~ s/^\Q$m\E\n\z//m;
		chomp $r;
		die "session error for <$q>: $s{err}" if $s{err} ne '';
		return $r;
	};
	my $spid = $sq->('SELECT pg_backend_pid()');
	# Calls recorded here are read back later: a pinned clock keeps them in
	# the live window (3 s here) however slow the reloads are.
	pin(fresh_bucket());

	# Append $line to postgresql.conf and reload; wait until the postmaster
	# and the session have processed it (scan_window moves to a sentinel).
	my $sentinel = 1000;
	my $reload = sub {
		my ($line) = @_;
		$sentinel++;
		$node->append_conf('postgresql.conf', "$line\n$P.scan_window = $sentinel\n");
		$node->reload;
		$node->poll_query_until('postgres',
			"SELECT setting FROM pg_settings WHERE name = '$P.scan_window'", $sentinel)
		  or die "reload not processed";
		for (1 .. 600)
		{
			return
			  if $sq->("SELECT setting FROM pg_settings WHERE name = '$P.scan_window'")
			  eq $sentinel;
			usleep(100_000);
		}
		die 'session did not process the reload';
	};
	# Controllers recorded for the statement tagged /* $tag */.
	my $controllers = sub {
		return sql(qq{SELECT string_agg(tags->>'controller', ',' ORDER BY tags->>'controller')
		  FROM ${P}_totals WHERE queryid = (SELECT queryid FROM ${P}_totals
		    WHERE tags->>'controller' = 'probe')});
	};

	sql("SELECT ${P}_reset()");
	my $svc = q{regex(pattern=''svc=(\\\\w+)'', keys=controller)};
	$reload->("$P.extractors = '$svc'");
	is(sql("SHOW $P.extractors"), q{regex(pattern='svc=(\\w+)', keys=controller)},
		'regex extractor from postgresql.conf');
	$sq->(q{SELECT 11, 'r' /* svc=probe */});
	$sq->(q{SELECT 12, 'r' /* svc=a */});
	is($controllers->(), 'a,probe', 'session records with the regex');

	my $logpos = -s $node->logfile;
	$reload->(qq{$P.extractors = 'regex(pattern=''(a)\\\\1'', keys=controller)'});
	$node->wait_for_log(qr/invalid value for parameter "$P\.extractors"/, $logpos);
	like(substr(slurp_file($node->logfile), $logpos),
		qr/DETAIL:  Pattern of extractor "regex" uses back-references/,
		'a bad DSL (back-reference) is rejected on reload');
	is(sql("SHOW $P.extractors"), q{regex(pattern='svc=(\w+)', keys=controller)},
		'the previous value is kept');
	$sq->(q{SELECT 13, 'r' /* svc=b */});
	sql(q{SELECT 14, 'r' /* svc=c */});
	is($controllers->(), 'a,b,c,probe',
		'existing and new backends keep recording with the previous config');

	$reload->(qq{$P.extractors = 'regex(pattern=''op=(\\\\w+)'', keys=controller)'});
	$sq->(q{SELECT 15, 'r' /* svc=x op=d */});
	sql(q{SELECT 16, 'r' /* svc=y op=e */});
	is($controllers->(), 'a,b,c,d,e,probe',
		'after a good change the session recompiles its regex (op=, not svc=); so does a new backend');
	is(info()->{regex_compile_failures}, 0, 'no compile failure');
	is($sq->('SELECT pg_backend_pid()'), $spid, 'the session kept one backend throughout');

	$s{in} .= "\\q\n";
	$s{h}->finish;
	$reload->("$P.extractors = 'sqlcommenter, marginalia'");
}

# ------------------------------------------- restart and resizing
my $big = q{SELECT 1 /*action='} . ('a' x 60) . q{',controller='} . ('c' x 60) . q{',job='j'*/};
my $three = q{SELECT 1 /*action='x',controller='y',job='z'*/};
# Sorted tag keys of the entries recorded for $stmt's tags.
sub keys_of
{
	my ($action) = @_;
	return sql(qq{SELECT string_agg(k, ',' ORDER BY k) FROM ${P}_totals,
	  jsonb_object_keys(tags) k WHERE tags->>'action' LIKE '$action%'});
}
{
	sql("SELECT ${P}_reset()");
	pin(fresh_bucket());	# kept until the restart, which must clear it
	sql($big);
	sql($three);
	is(keys_of('a') . ' ' . keys_of('x'), 'action,controller,job action,controller,job',
		'before: all three tags fit the default limits');
	my $before = info();
	is("$before->{entries} $before->{dropped_tags}", '2 0', 'before: 2 entries, nothing dropped');

	sql("ALTER SYSTEM SET $P.max_entries = 100");
	sql("ALTER SYSTEM SET $P.bucket_count = 2");
	sql("ALTER SYSTEM SET $P.bucket_interval = '2s'");
	sql("ALTER SYSTEM SET $P.max_tags = 2");
	sql("ALTER SYSTEM SET $P.max_tagset_bytes = 128");
	$node->restart;

	my $b = buckets();
	is("$b->{interval_us} $b->{bucket_count} $b->{current_bucket} $b->{clock_mode} $b->{clock_value}",
		'2000000 2 0 real 0',
		'restart: new interval and ring size; fresh watermark; the pinned debug clock is gone');
	my $i = info();
	is("$i->{entries} $i->{max_entries} $i->{buckets} $i->{oldest_bucket}", '0 100 2 NULL',
		'restart: the store starts empty with the new sizes (no persistence in v1)');
	is(join(' ', @$i{qw(dealloc evicted_entries invalid_tags dropped_tags heuristic_scans
		  regex_compile_failures utility_missing_queryid)}), '0 0 0 0 0 0 0',
		'restart: every counter starts at zero');
	is(sql(qq{SELECT stats_reset > '$before->{stats_reset}' FROM ${P}_info()}), 't',
		'restart: stats_reset is the new start time');
	is(sql("SELECT count(*) FROM $P"), 0, 'restart: the view is empty');
	is($i->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(100, 128, 2)'),
		'restart: shmem_bytes sized for the new settings');

	# Tag limits: greedy fill in allowlist order (action, controller, job).
	# Pinned, so the 4 s window cannot expire the entries before they are read.
	pin(fresh_bucket());
	sql($big);
	is(keys_of('a'), 'action,job',
		'max_tagset_bytes = 128: controller no longer fits, the smaller job still does');
	sql($three);
	is(keys_of('x'), 'action,controller', 'max_tags = 2: job dropped');
	is(info()->{dropped_tags}, 2, 'both drops counted');

	# The new ring: 2 slots of 2 s, aligned on multiples of 2 s.
	my $base = fresh_bucket();
	pin($base + $_), run_tagged('rs') for 0 .. 2;
	is(vrows('rs', $base), '1:1 2:1', 'bucket_count = 2: two live buckets');
	is(raw_slots('rs', $base), '1:1 2:1', 'two slots per entry');
	is(sql(qq{SELECT bool_and(extract(epoch FROM bucket_start)::numeric % 2 = 0)
	          AND max(bucket_start) - min(bucket_start) = interval '2 s'
	          FROM $P WHERE tags->>'controller' = 'rs'}), 't',
		'bucket_interval = 2s: bucket_start on multiples of 2 s, 2 s apart');

	# max_entries = 100: churn stays within it, the newest call is recorded.
	sql(q{SELECT format('SELECT 1 /*controller=''m%s''*/', i)
	        FROM generate_series(1, 120) i \gexec});
	$i = info();
	ok($i->{entries} <= 100 && $i->{dealloc} >= 1,
		"max_entries = 100: $i->{entries} entries after 120 more combinations, "
		  . "$i->{dealloc} eviction passes");
	is(vrows('m120', $base), '2:1', 'the newest combination is recorded');
}

# ------------------------------ forced collision, eviction, reinsertion
{
	sql("SELECT ${P}_reset()");
	sql('SELECT pssc_store_test_force_collisions(true)');
	is(sql('SELECT force_collisions FROM pssc_store_test_counters()'), 't',
		'forced collisions: every key in one dynahash chain');
	pin(fresh_bucket());
	# k1..k5 called once (usage 2), the others twice (usage 3).
	sql(q{SELECT format('SELECT 1 /*controller=''k%s''*/', i)
	        FROM generate_series(1, 100) i, generate_series(1, CASE WHEN i <= 5 THEN 1 ELSE 2 END) j
	       ORDER BY i, j \gexec});
	is(info()->{entries}, 100, 'the table is full');
	run_tagged('k101');
	my $i = info();
	is("$i->{entries} $i->{dealloc} $i->{evicted_entries}", '96 1 5',
		'one eviction pass removed 5 entries from the chain');
	is(sql(qq{SELECT string_agg(tags->>'controller', ',' ORDER BY tags->>'controller')
	          FROM $P WHERE tags->>'controller' IN ('k1', 'k2', 'k3', 'k4', 'k5', 'k6', 'k101')}),
		'k101,k6', 'the five lowest-usage entries k1..k5 were evicted');

	run_tagged('k3');
	is(sql(qq{SELECT calls FROM $P WHERE tags->>'controller' = 'k3'}), 1,
		'an evicted combination is reinserted as a new entry (old calls gone)');
	is(usage('k3'), 2, 'with a fresh usage');
	run_tagged('k3');
	is(sql(qq{SELECT calls FROM $P WHERE tags->>'controller' = 'k3'}), 2,
		'the reinserted entry is found again in the shared chain');
	is(sql(q{SELECT count(*) FROM pssc_store_test_entries()
	         WHERE tags = ARRAY['controller', 'k3']}), 1, 'no duplicate');
	is(info()->{entries}, 97, 'entries: 96 + k3');
	is(sql('SELECT pssc_store_test_check_invariants()'), 97, 'invariants hold');
	sql("SELECT ${P}_reset()");
	sql('SELECT pssc_store_test_force_collisions(false)');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ------------------------------------ pgbench across bucket boundaries
{
	sql("ALTER SYSTEM RESET $P.max_entries");
	sql("ALTER SYSTEM RESET $P.max_tags");
	sql("ALTER SYSTEM RESET $P.max_tagset_bytes");
	sql("ALTER SYSTEM SET $P.bucket_interval = '1s'");
	sql("ALTER SYSTEM SET $P.bucket_count = 64");
	$node->restart;
	my $i = info();
	is("$i->{entries} $i->{max_entries} $i->{buckets}", '0 10000 64',
		'restart grows the store back: empty, max_entries 10000, 64 buckets');
	is(sql('SELECT force_collisions FROM pssc_store_test_counters()'), 'f',
		'forced collisions do not survive a restart');

	my $file = $node->basedir . '/pgbench_reconfig.sql';
	open my $fh, '>', $file or die "open $file: $!";
	# Each transaction runs exactly one tagged statement; 2% of them first
	# step the clock by a quarter bucket (racing with the other clients).
	print $fh q{\set r random(1, 100)
\if :r <= 2
SELECT pssc_store_test_advance_clock(250000);
\endif
\if :r % 2 = 0
SELECT 1 /*controller='pa'*/;
\else
SELECT 2 /*controller='pb',action='x'*/;
\endif
};
	close $fh;
	for my $mode (qw(simple prepared))
	{
		sql("SELECT ${P}_reset()");
		my $base = fresh_bucket();
		pin($base);
		my ($clients, $txns) = (8, 500);
		my ($out, $err);
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		IPC::Run::run([ 'pgbench', '-n', '-M', $mode, '-c', $clients, '-j', $clients,
				'-t', $txns, '-f', $file, 'postgres' ],
			'>', \$out, '2>', \$err)
		  or die "pgbench failed: $err";
		my $n = $clients * $txns;
		like($out, qr{number of transactions actually processed: $n/$n},
			"$mode: pgbench ran all $n transactions");

		my $cur = buckets()->{current_bucket};
		ok($cur - $base >= 3 && $cur - $base < 64,
			"$mode: the clock crossed " . ($cur - $base) . ' bucket boundaries within the ring');
		is(sql(qq{SELECT sum(calls) || ' ' || count(*) FROM ${P}_totals
		          WHERE tags->>'controller' IN ('pa', 'pb')}), "$n 2",
			"$mode: every call counted exactly once, in two entries");
		is(sql(qq{SELECT sum(calls) FROM $P WHERE tags->>'controller' IN ('pa', 'pb')}), $n,
			"$mode: the per-bucket rows add up to the same");
		my $nb = sql(qq{SELECT count(DISTINCT bucket_start) FROM $P});
		ok($nb >= 3, "$mode: calls spread over $nb buckets");
		is(sql(qq{SELECT max(bucket_start) = pssc_store_test_bucket_start($cur) FROM $P}), 't',
			"$mode: the newest row is in current_bucket");
		$i = info();
		is("$i->{entries} $i->{dealloc}", '2 0', "$mode: _info(): 2 entries, no eviction");
		is(sql('SELECT dropped_records FROM pssc_store_test_counters()'), 0,
			"$mode: no record dropped");
		is(sql('SELECT pssc_store_test_check_invariants()'), 2, "$mode: invariants hold");
	}
	sql('SELECT pssc_store_test_set_clock_offset(0)');
	sql('ALTER SYSTEM RESET ALL');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|TRAP|terminated by signal/, 'server log has no crash');

done_testing();
