# pg_stat_statement_context_info() and pg_stat_statement_context_reset()
# (DESIGN.md §5.1-§5.3, §6.2, §6.7, §6.11, §7; backlog 20261005-091225-21).
#
# Covers: the exact §7 signature and privileges (_info() is PUBLIC like
# pg_stat_statements_info, _reset() is revoked from PUBLIC); a single row
# with entries, max_entries, buckets (= bucket_count) and the exact
# shmem_bytes requested at startup; every counter moving under the activity
# that drives it, through real statements: invalid_tags (malformed tags),
# dropped_tags (a tag set over max_tagset_bytes), heuristic_scans (append
# scan of a statement longer than scan_window), dealloc and evicted_entries
# (churn over max_entries); the eviction outcomes kept apart: dead entries
# reclaimed (reclaimed_entries), live entries evicted (evicted_entries) and
# records lost because a pass freed nothing (dropped_records); the bucket
# metadata (bucket_seconds, current_bucket_start, last_closed_bucket_start)
# and stats_reset_epoch; regex_compile_failures (an injected lazy-compile
# failure, read from another session) and utility_missing_queryid (wrong
# shared_preload_libraries order); _info() sees the calling statement's own
# extraction counters; oldest_bucket is the start of the oldest live slot
# of any entry (NULL when there is none), judged against the watermark the
# row reports even if it moves during the scan (the scan is stalled by a
# test hook while another reader advances it); reset clears entries and counters
# (including the caller's pending counters, and without re-counting a regex
# extractor that stays disabled in a backend) and sets stats_reset; a reset
# never splits a concurrent flush of diagnostic counters (a flush stalled
# under the store lock by a test hook makes the reset wait); and all
# three SQL functions raise the "not preloaded" error without
# shared_preload_libraries. Entries and the clock are also driven through
# the TEST-ONLY module test/modules/pssc_store_test, and regex faults are
# injected with test/modules/pssc_extract_test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;
use IPC::Run;
use Time::HiRes qw(usleep time);

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('info');
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

sql("CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test; "
	  . 'CREATE EXTENSION pssc_extract_test');
sql('CREATE ROLE alice');

my @cols = qw(entries max_entries dealloc reclaimed_entries evicted_entries dropped_records
  buckets bucket_seconds oldest_bucket current_bucket_start last_closed_bucket_start
  shmem_bytes cap_shmem_bytes invalid_tags dropped_tags heuristic_scans regex_compile_failures
  utility_missing_queryid stats_reset stats_reset_epoch exemplar_shmem_bytes
  exemplar_value_bytes exemplar_values_dropped);

# The one row of _info() as a hash; NULLs as 'NULL'. $suffix (e.g. a
# tagged comment) is appended to the statement that calls _info().
sub info
{
	my ($suffix) = @_;
	$suffix //= '';
	my $sel = join(" || '|' || ", map { "coalesce(${_}::text, 'NULL')" } @cols);
	my @v = split /\|/, sql("SELECT $sel FROM ${P}_info() i $suffix"), -1;
	die "unexpected _info() output" unless @v == @cols;
	my %h;
	@h{@cols} = @v;
	return \%h;
}

# "invalid dropped heuristic regex utility"
sub diag_counters
{
	my $i = info(@_);
	return join(' ', @$i{qw(invalid_tags dropped_tags heuristic_scans
		  regex_compile_failures utility_missing_queryid)});
}

# "entries dealloc reclaimed_entries evicted_entries dropped_records"
sub churn_counters
{
	my $i = info();
	return join(' ', @$i{qw(entries dealloc reclaimed_entries evicted_entries
		  dropped_records)});
}

sub set_conf
{
	my ($name, $literal, $expected) = @_;
	sql("ALTER SYSTEM SET $P.$name = $literal");
	sql('SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', "SHOW $P.$name", $expected)
	  or die "$P.$name did not become $expected";
}

# Pin the shared debug clock in the middle of bucket $b.
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}

# ------------------------------------------------------- catalog: §7
{
	my $info_cols = 'OUT entries bigint, OUT max_entries bigint, OUT dealloc bigint, '
		  . 'OUT reclaimed_entries bigint, OUT evicted_entries bigint, '
		  . 'OUT dropped_records bigint, OUT buckets integer, OUT bucket_seconds integer, '
		  . 'OUT oldest_bucket timestamp with time zone, '
		  . 'OUT current_bucket_start timestamp with time zone, '
		  . 'OUT last_closed_bucket_start timestamp with time zone, OUT shmem_bytes bigint, '
		  . 'OUT cap_shmem_bytes bigint, OUT invalid_tags bigint, OUT dropped_tags bigint, '
		  . 'OUT heuristic_scans bigint, OUT regex_compile_failures bigint, '
		  . 'OUT utility_missing_queryid bigint, '
		  . 'OUT capped_tags bigint, OUT cap_table_full bigint, '
		  . 'OUT stats_reset timestamp with time zone, OUT stats_reset_epoch bigint, '
		  . 'OUT exemplar_shmem_bytes bigint, OUT exemplar_value_bytes integer, '
		  . 'OUT exemplar_values_dropped bigint';
	is(sql(qq{SELECT pg_get_function_arguments(p.oid) || ' -> ' || pg_get_function_result(p.oid)
	          || ' ' || concat_ws(' ', provolatile, proretset)
	          FROM pg_proc p WHERE proname = '${P}_info'}),
		"$info_cols -> record v f",
		'_info(): exactly the §7 columns, one row, VOLATILE');
	is(sql(qq{SELECT pg_get_function_arguments(p.oid) || ' -> ' || pg_get_function_result(p.oid)
	          || ' ' || provolatile::text FROM pg_proc p WHERE proname = '${P}_reset'}),
		' -> void v', '_reset(): no arguments, returns void, VOLATILE');
	is(sql(qq{SELECT has_function_privilege('alice', '${P}_info()', 'EXECUTE'),
	                 has_function_privilege('alice', '${P}_reset()', 'EXECUTE')}),
		't|f', '_info() is executable by PUBLIC, _reset() is not');
	is(sql(qq{SELECT count(*) FROM ${P}_info()}), 1, '_info() returns one row');

	(my $counters_cols = $info_cols) =~ s/OUT oldest_bucket timestamp with time zone, //;
	is(sql(qq{SELECT pg_get_function_arguments(p.oid) || ' -> ' || pg_get_function_result(p.oid)
	          || ' ' || concat_ws(' ', provolatile, proparallel, proisstrict, proretset)
	          FROM pg_proc p WHERE proname = '${P}_counters'}),
		"$counters_cols -> record v r t f",
		'_counters(): the _info() columns but oldest_bucket, VOLATILE PARALLEL RESTRICTED STRICT');
	is(sql(qq{SELECT concat_ws(' ', provolatile, proparallel, proisstrict, prosrc)
	          FROM pg_proc WHERE proname = '${P}_info'}),
		'v r t pg_stat_statement_context_info_1_0', '_info(): versioned C symbol');
	is(sql(qq{SELECT prosrc FROM pg_proc WHERE proname = '${P}_counters'}),
		'pg_stat_statement_context_counters_1_0', '_counters(): versioned C symbol');
	is(sql(qq{SELECT has_function_privilege('alice', '${P}_counters()', 'EXECUTE')}),
		't', '_counters() is executable by PUBLIC, like _info()');
	is(sql(qq{SELECT count(*) FROM ${P}_counters()}), 1, '_counters() returns one row');
}

# ------------------------------------------------- sizes, empty store
{
	my $i = info();
	is("$i->{entries} $i->{max_entries} $i->{buckets} $i->{oldest_bucket}",
		'0 100 4 NULL', 'empty: entries 0, max_entries, buckets = bucket_count, '
		  . 'oldest_bucket NULL');
	is($i->{shmem_bytes}, sql('SELECT pssc_store_test_shmem_size_for(100, 512, 4)'),
		'shmem_bytes equals the startup request for these settings');
	is($i->{shmem_bytes}, sql('SELECT shmem_bytes FROM pssc_store_test_counters()'),
		'shmem_bytes equals the size the store recorded when it requested it');
	ok( sql(qq{SELECT sum(allocated_size) BETWEEN 1 AND $i->{shmem_bytes}
	           FROM pg_shmem_allocations WHERE name LIKE '$P%'
	             AND name <> '$P cardinality caps'}) eq 't',
		'the named allocations (but the cardinality caps table) fit within shmem_bytes');
	is($i->{cap_shmem_bytes},
		sql(qq{SELECT size FROM pg_shmem_allocations WHERE name = '$P cardinality caps'}),
		'cap_shmem_bytes is the exact size requested for the cardinality caps table');
	# default cardinality_cap_slots: 16384 value words plus 1024 two-word key
	# slots, 8 bytes each (up to 16 with emulated 64-bit atomics)
	ok( $i->{cap_shmem_bytes} > (16384 + 2 * 1024) * 8
		  && $i->{cap_shmem_bytes} <= (16384 + 2 * 1024) * 16 + 64,
		"cap_shmem_bytes ($i->{cap_shmem_bytes}) is the default table's words plus a small header");
	is(sql(qq{SELECT stats_reset <= now() AND stats_reset > now() - interval '1 hour'
	          FROM ${P}_info()}), 't', 'stats_reset is set at startup');
	is(diag_counters() . ' ' . churn_counters(), '0 0 0 0 0 0 0 0 0 0',
		'all counters start at zero');
}

# ------------------------------------------------- per-counter activity
my $long = q{'SELECT length(''' || repeat('x', 3000) || ''') /*a=''1''*/'};
my $big = q{'SELECT 2 /*' || (SELECT string_agg('k' || i || '=''' || repeat('v', 64)
  || '''', ',') FROM generate_series(1, 8) i) || '*/'};
{
	sql("SELECT ${P}_reset()");
	sql(q{SELECT 1 /*a='1',bad='%00'*/});
	is(diag_counters(), '1 0 0 0 0', 'invalid_tags counts a malformed tag');

	sql("SELECT ${P}_reset()");
	sql("SELECT $big \\gexec");
	is(diag_counters(), '0 1 0 0 0',
		'dropped_tags counts a tag that does not fit max_tagset_bytes');

	sql("SELECT ${P}_reset()");
	sql("SELECT $long \\gexec");
	is(diag_counters(), '0 0 1 0 0',
		'heuristic_scans counts an append scan of a statement over scan_window');
	sql("SELECT $long \\gexec") for 1 .. 2;
	is(diag_counters(), '0 0 3 0 0', 'heuristic_scans counts every such scan');

	# The calling statement's own extraction counters are flushed before
	# reading (they would otherwise only be flushed at its ExecutorEnd).
	sql("SELECT ${P}_reset()");
	is(diag_counters(q{/*a='1',bad='%00'*/}), '1 0 0 0 0',
		'_info() sees the invalid tag of the statement calling it');
	is(diag_counters(), '1 0 0 0 0', 'and it is counted only once');
	is(info()->{entries}, 1, 'that statement was recorded at its end');
}

# ----------------------------------------------- churn: dealloc, evicted
{
	sql("SELECT ${P}_reset()");
	is(churn_counters(), '0 0 0 0 0', 'reset: no entries, no passes');
	sql(q{SELECT format('SELECT 1 /*k=''%s''*/', i) FROM generate_series(1, 100) i \gexec});
	is(churn_counters(), '100 0 0 0 0', '100 distinct tag sets fill the table, no pass yet');
	is(info()->{entries},
		sql(q{SELECT count(*) FROM (SELECT DISTINCT dbid, userid, queryid, toplevel, tags
		      FROM pssc_store_test_entries()) e}),
		'entries equals the number of entries in the table');
	sql(q{SELECT format('SELECT 1 /*k=''x%s''*/', i) FROM generate_series(1, 10) i \gexec});
	my ($n, $d, $r, $e, $x) = split / /, churn_counters();
	ok($d >= 2, "churn: dealloc counts eviction passes ($d)");
	ok($e >= 10 && $e >= $d * 5, "churn: evicted_entries counts evicted entries ($e)");
	is("$r $x", '0 0', 'churn of live entries: nothing reclaimed, nothing dropped');
	ok($n <= 100 && $n == 100 + 10 - $e,
		"churn: entries ($n) = 110 inserted - $e evicted, within max_entries");
}

# ------------------- eviction outcomes: reclaimed vs evicted vs dropped
# Driven at a pinned clock with pssc_store_test_record() (max_entries 100,
# bucket_count 4, so a pass aims to free 5 entries).
{
	my $fill = sub {
		my ($from, $to) = @_;
		sql("SELECT count(*) FROM generate_series($from, $to) q, pssc_store_test_record(q)");
	};
	my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');

	# Expired-only reclamation: every entry dead, the pass frees them all.
	sql("SELECT ${P}_reset()");
	pin($b);
	$fill->(1, 100);
	is(churn_counters(), '100 0 0 0 0', 'reclaim: 100 live entries, no pass yet');
	pin($b + 4);    # window [b+1, b+4]: every entry is dead
	is(sql(q{SELECT pssc_store_test_record(1001)}), 'inserted', 'reclaim: the insert succeeds');
	is(churn_counters(), '1 1 100 0 0',
		'dead entries only: reclaimed_entries moves, evicted_entries does not');

	# Undersized churn: every entry live, the pass evicts live ones.
	sql("SELECT ${P}_reset()");
	pin($b + 10);
	$fill->(1, 100);
	is(sql(q{SELECT pssc_store_test_record(1001)}), 'inserted', 'evict: the insert succeeds');
	is(churn_counters(), '96 1 0 5 0',
		'live entries only: evicted_entries moves, reclaimed_entries does not');

	# Both in one pass: 2 dead entries fall short of the target of 5.
	sql("SELECT ${P}_reset()");
	pin($b + 20);
	$fill->(1, 2);
	pin($b + 24);    # entries 1, 2 dead
	$fill->(3, 100);
	is(sql(q{SELECT pssc_store_test_record(1001)}), 'inserted', 'mixed: the insert succeeds');
	is(churn_counters(), '96 1 2 3 0',
		'a pass that reclaims 2 dead and evicts 3 live entries counts each apart');

	# Dropped: a full table of live entries and a pass that frees nothing
	# (its candidate buffer cannot be allocated).
	sql("SELECT ${P}_reset()");
	pin($b + 30);
	$fill->(1, 100);
	is(sql(q{SELECT pssc_store_test_fail_next_eviction_alloc(),
	                pssc_store_test_record(1001)}), '|full', 'drop: the record is lost');
	is(churn_counters(), '100 1 0 0 1',
		'a pass that frees nothing: dropped_records moves, nothing reclaimed or evicted');
	is(info()->{dropped_records}, sql('SELECT dropped_records FROM pssc_store_test_counters()'),
		'dropped_records is the store\'s own counter');
	sql("SELECT ${P}_reset()");
	is(churn_counters(), '0 0 0 0 0', 'reset zeroes all three');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ------------------------------------- bucket metadata, stats_reset_epoch
{
	sql("SELECT ${P}_reset()");
	my $i = info();
	is($i->{bucket_seconds}, sql('SELECT interval_us / 1000000 FROM pssc_store_test_buckets()'),
		'bucket_seconds is bucket_interval in seconds');
	is($i->{bucket_seconds}, sql("SELECT setting FROM pg_settings WHERE name = '$P.bucket_interval'"),
		'bucket_seconds equals the bucket_interval setting');

	my $b = sql('SELECT reader_bucket + 50 FROM pssc_store_test_buckets()');
	my $start = sub { sql("SELECT pssc_store_test_bucket_start($_[0])") };
	pin($b);
	$i = info();
	is($i->{current_bucket_start}, $start->($b),
		'current_bucket_start: the start of the current bucket');
	is($i->{last_closed_bucket_start}, $start->($b - 1),
		'last_closed_bucket_start: the start of the bucket before it');
	is(sql(qq{SELECT current_bucket_start - last_closed_bucket_start
	              = make_interval(secs => bucket_seconds)
	          FROM ${P}_info()}), 't', 'they are one bucket apart');
	is(sql(qq{SELECT current_bucket_start = (SELECT pssc_store_test_bucket_start(current_bucket)
	                                         FROM pssc_store_test_buckets())
	          FROM ${P}_info()}), 't', 'current_bucket_start follows the store watermark');

	# The watermark never moves back: after a backward clock step the
	# current bucket stays the newest one observed.
	pin($b - 3);
	$i = info();
	is("$i->{current_bucket_start} $i->{last_closed_bucket_start}",
		$start->($b) . ' ' . $start->($b - 1),
		'a backward clock step does not move the current or last closed bucket back');
	pin($b + 2);
	$i = info();
	is("$i->{current_bucket_start} $i->{last_closed_bucket_start}",
		$start->($b + 2) . ' ' . $start->($b + 1), 'they advance with the clock');
	sql('SELECT pssc_store_test_set_clock_offset(0)');

	is(sql(qq{SELECT stats_reset_epoch = floor(extract(epoch FROM stats_reset))::bigint
	          FROM ${P}_info()}), 't',
		'stats_reset_epoch is stats_reset in whole Unix epoch seconds');
	ok(sql(qq{SELECT stats_reset_epoch FROM ${P}_info()}) > 1700000000,
		'stats_reset_epoch is a Unix time');
}

# ---------------------------------------------- regex_compile_failures
{
	sql("SELECT ${P}_reset()");
	set_conf('extractors', q{'regex(pattern=''svc=(\w+)'', keys=service)'},
		q{regex(pattern='svc=(\w+)', keys=service)});
	# A failed lazy compile disables the extractor for the backend: counted
	# once, then flushed into the shared counter at the statement's end.
	is( sql(
			"SELECT pssc_extract_test_regex_inject('compile', 0, 'oom', -1); "
			  . 'SELECT 1 /* svc=s */; SELECT 2 /* svc=s */; '
			  . 'SELECT pssc_extract_test_regex_injected()'),
		"\n1\n2\n1", 'the injected compile failure fired once in that session');
	is(info()->{regex_compile_failures}, 1,
		'regex_compile_failures is visible from another session');

	# Reset in the failing backend: the extractor stays disabled there and
	# is not counted again (no pre-reset count comes back).
	is( sql(
			"SELECT pssc_extract_test_regex_inject('compile', 0, 'oom', -1); "
			  . 'SELECT 1 /* svc=s */; '
			  . "SELECT ${P}_reset(); "
			  . 'SELECT 2 /* svc=s */; '
			  . "SELECT regex_compile_failures FROM ${P}_info()"),
		"\n1\n\n2\n0", 'after a reset, a backend whose regex failed earlier counts nothing');
	is(info()->{regex_compile_failures}, 0, 'and nothing is flushed later');
	set_conf('extractors', q{'sqlcommenter, marginalia'}, 'sqlcommenter, marginalia');
}

# ------------------------------------------------------------- reset
{
	sql(q{SELECT 1 /*a='1',bad='%00'*/});
	sql("SELECT $long \\gexec");
	sql("SELECT $big \\gexec");
	sql('SELECT pssc_store_test_add_stats(0, 0, 0, 5); '
		  . 'SELECT pssc_store_test_utility_missing_queryid()');
	my $before = info();
	is(join(' ', map { $before->{$_} > 0 ? 1 : 0 } qw(entries invalid_tags dropped_tags
		heuristic_scans regex_compile_failures utility_missing_queryid)),
		'1 1 1 1 1 1', 'before reset: entries and every diagnostic counter are non-zero');
	sql(q{SELECT format('SELECT 1 /*r=''%s''*/', i) FROM generate_series(1, 101) i \gexec});
	ok(info()->{dealloc} > 0, 'before reset: dealloc non-zero');

	my $ts = sql(qq{SELECT statement_timestamp() FROM ${P}_reset()});
	my $after = info();
	is(diag_counters() . ' ' . churn_counters(), '0 0 0 0 0 0 0 0 0 0',
		'reset zeroes entries and every counter');
	is($after->{oldest_bucket}, 'NULL', 'reset: oldest_bucket NULL');
	is(sql(qq{SELECT stats_reset >= '$ts' AND stats_reset > '$before->{stats_reset}'
	          FROM ${P}_info()}), 't', 'reset sets stats_reset to the time of the reset');
	is(sql("SELECT count(*) FROM $P"), 0, 'the stats view is empty after reset');
	is( join(' ', @$after{qw(max_entries buckets shmem_bytes)}),
		join(' ', @$before{qw(max_entries buckets shmem_bytes)}),
		'reset keeps the sizes');

	# The reset statement's own extraction counters predate the reset.
	sql(qq{SELECT ${P}_reset() /*a='1',bad='%00'*/});
	is(diag_counters(), '0 0 0 0 0',
		'reset discards the calling statement\'s pending counters');
	is(info()->{entries}, 1, 'the (tagged) reset statement itself is recorded after it');

	my ($ret, $out, $err) = $node->psql('postgres',
		"SET ROLE alice; SELECT ${P}_reset()");
	isnt($ret, 0, 'unprivileged reset fails');
	like($err, qr/permission denied for function ${P}_reset/,
		'unprivileged role gets permission denied');
	is(info()->{entries}, 1, 'and nothing was reset');
	($ret, $out, $err) = $node->psql('postgres',
		"SET ROLE alice; SELECT entries FROM ${P}_info()");
	is("$ret|$out|$err", '0|1|', 'an unprivileged role can call _info()');
	sql("GRANT EXECUTE ON FUNCTION ${P}_reset() TO alice");
	($ret, $out, $err) = $node->psql('postgres',
		"SET ROLE alice; SELECT ${P}_reset(); SELECT entries FROM ${P}_info()");
	is("$ret|$out|$err", "0|\n0|", 'reset works once granted');
}

# ------------------------------------- reset vs a concurrent flush
# A backend's flush of its diagnostic counters is stalled (by a TEST-ONLY
# hook) under the store lock, after it has added invalid_tags and before the
# other counters; a reset started meanwhile must wait for the whole flush,
# so the counters are all zero afterwards (an unlocked flush would leave the
# counters added after the reset, e.g. heuristic_scans 1).
{
	my $release = $node->basedir . '/flush_release';

	# psql running the given statements (one -c each) in the background.
	my $bg = sub {
		my ($app, @cmds) = @_;
		my %r = (out => '', err => '');
		$r{h} = IPC::Run::start(
			[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d',
			  $node->connstr('postgres') . " application_name=$app",
			  map { ('-c', $_) } @cmds ],
			'>', \$r{out}, '2>', \$r{err}, IPC::Run::timeout(180));
		return \%r;
	};
	my $wait_for = sub {
		for (1 .. 300)
		{
			return 1 if sql($_[0]) eq 't';
			usleep(100_000);
		}
		return 0;
	};
	my $waiting = sub {
		my ($app, $cond) = @_;
		return $wait_for->(qq{SELECT EXISTS (SELECT FROM pg_stat_activity
		                      WHERE application_name = '$app' AND $cond)});
	};

	my $bad_long = "SELECT length('" . ('x' x 3000) . q{') /*a='1',bad='%00'*/};
	sql("SELECT ${P}_reset()");
	sql($bad_long);
	is(diag_counters(), '1 0 1 0 0',
		'a long statement with a malformed tag counts invalid_tags and heuristic_scans');

	for my $t (
		[ 'statement end (flush under the record\'s lock)', $bad_long ],
		[ 'standalone flush', 'SELECT pssc_store_test_add_stats(1, 1, 1, 1)' ],
		[ 'utility_missing_queryid', 'SELECT pssc_store_test_utility_missing_queryid()' ])
	{
		my ($name, $trigger) = @$t;
		unlink $release;
		sql("SELECT ${P}_reset()");
		my $a = $bg->('pssc_flusher',
			"SELECT pssc_store_test_stall_next_flush('$release')", $trigger);
		ok($waiting->('pssc_flusher', q{wait_event = 'PgSleep'}),
			"$name: the flush stalls under the store lock");
		my $b = $bg->('pssc_resetter', "SELECT ${P}_reset()");
		ok($waiting->('pssc_resetter', q{wait_event_type = 'LWLock'}),
			"$name: a concurrent reset waits for the flush");
		open(my $fh, '>', $release) or die "cannot create $release: $!";
		close($fh);
		$_->{h}->finish for $a, $b;
		is("$a->{err}$b->{err}", '', "$name: both sessions succeed");
		is(diag_counters(), '0 0 0 0 0',
			"$name: the reset came wholly after the flush (nothing split)");
	}

	# _info() vs a concurrent watermark advance: the one entry's only slot
	# (bucket b) is judged live in the window ending at b + 3; while the
	# scan is stalled after it, another reader moves the watermark to b + 4,
	# which expires b. The row must still be self-consistent: oldest_bucket
	# judged against the watermark it reports (NULL here), never a slot
	# that has expired at current_bucket_start.
	unlink $release;
	sql("SELECT ${P}_reset()");
	my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	pin($b);
	sql('SELECT pssc_store_test_record(1)');
	pin($b + 3);
	my $a = $bg->('pssc_info_scan', "SELECT pssc_store_test_stall_next_info_scan('$release')",
		qq{SELECT coalesce(oldest_bucket::text, 'NULL') || '|' || current_bucket_start
		   FROM ${P}_info()});
	ok($waiting->('pssc_info_scan', q{wait_event = 'PgSleep'}),
		'_info() scan stalls after judging the entry');
	pin($b + 4);
	is(sql("SELECT count(*) FROM $P"), '0', 'another reader moves the watermark: b expired');
	open(my $fh, '>', $release) or die "cannot create $release: $!";
	close($fh);
	$a->{h}->finish;
	is($a->{err}, '', 'the stalled _info() succeeds');
	is($a->{out} =~ s/^\s+|\s+$//gr, 'NULL|' . sql("SELECT pssc_store_test_bucket_start($b + 4)"),
		'_info() row is self-consistent: oldest_bucket judged at its own current_bucket_start');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
	unlink $release;
}

# --------------------------------------- bounded, interruptible scan
# The oldest_bucket walk is repeated when the watermark moves during it,
# at most 3 passes (DESIGN.md §7): a TEST-ONLY hook advances the watermark
# after every entry judged, so every pass ends with a moved watermark. The
# hook stops advancing after 1000 calls, so an unbounded loop ends too (and
# fails the call count) instead of hanging.
{
	sql("SELECT ${P}_reset()");
	my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	my $us = sql('SELECT interval_us FROM pssc_store_test_buckets()');
	pin($b);
	sql("SELECT pssc_store_test_record(q) FROM generate_series(1, 5) q");
	my $out = sql(qq{SELECT pssc_store_test_info_scan_hook($us, 0, 1000);
		SELECT coalesce(oldest_bucket::text, 'NULL') || '|' || current_bucket_start
		  || '|' || entries FROM ${P}_info();
		SELECT pssc_store_test_info_scan_hook_off()});
	my (undef, $row, $calls) = split /\n/, $out;
	is($calls, 15, 'a moving watermark: _info() gives up after 3 passes over the 5 entries');
	is($row, 'NULL|' . sql("SELECT pssc_store_test_bucket_start($b + 10)") . '|5',
		'the row is judged against the watermark of its last pass (b+10: slot b expired)');
	is(sql('SELECT current_bucket FROM pssc_store_test_buckets()'), $b + 15,
		'meanwhile the watermark moved on (the documented bounded staleness)');

	# A still watermark: one pass.
	pin($b + 16);
	sql("SELECT pssc_store_test_record(q) FROM generate_series(1, 5) q");
	$out = sql(qq{SELECT pssc_store_test_info_scan_hook(0, 0, 0);
		SELECT oldest_bucket = pssc_store_test_bucket_start($b + 16) FROM ${P}_info();
		SELECT pssc_store_test_info_scan_hook_off()});
	is($out, "\nt\n5", 'a still watermark: one pass, oldest_bucket exact');

	# A cancel is serviced in the middle of a pass: 50 entries, the hook
	# sleeps 200 ms after each (a 10 s pass) under the lock, where the
	# interrupt is held off; the walk checks for it after each entry,
	# releases the lock and lets it through.
	sql("SELECT pssc_store_test_record(q) FROM generate_series(1, 50) q");
	my %r = (out => '', err => '');
	my $h = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d',
		  $node->connstr('postgres') . ' application_name=pssc_cancel',
		  '-c', 'SELECT pssc_store_test_info_scan_hook(0, 200, 0)',
		  '-c', "SELECT entries FROM ${P}_info()" ],
		'>', \$r{out}, '2>', \$r{err}, IPC::Run::timeout(180));
	ok($node->poll_query_until('postgres',
			q{SELECT EXISTS (SELECT FROM pg_stat_activity
			  WHERE application_name = 'pssc_cancel' AND wait_event = 'PgSleep')}),
		'the _info() walk is under way');
	my $t0 = time();
	sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity
	      WHERE application_name = 'pssc_cancel'});
	$h->finish;
	my $elapsed = time() - $t0;
	like($r{err}, qr/canceling statement due to user request/, 'the _info() call is canceled');
	ok($elapsed < 5, "promptly, within the pass ($elapsed s, a pass takes 10 s)");
	unlike($r{out}, qr/\d/, 'no row');
	sql('SELECT pssc_store_test_set_clock_offset(0)');

	# The last pass gives way to a cancel too: over 50 entries, the hook
	# moves the watermark after each of the first 100 (passes 1 and 2), and
	# only then sleeps 200 ms after each, so the backend is sleeping only
	# in pass 3, a 10 s pass when the cancel arrives.
	sql("SELECT ${P}_reset()");
	$b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	pin($b);
	sql("SELECT pssc_store_test_record(q) FROM generate_series(1, 50) q");
	%r = (out => '', err => '');
	$h = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d',
		  $node->connstr('postgres') . ' application_name=pssc_cancel3',
		  '-c', "SELECT pssc_store_test_info_scan_hook($us, 200, 100)",
		  '-c', "SELECT entries FROM ${P}_info()" ],
		'>', \$r{out}, '2>', \$r{err}, IPC::Run::timeout(180));
	ok($node->poll_query_until('postgres',
			q{SELECT EXISTS (SELECT FROM pg_stat_activity
			  WHERE application_name = 'pssc_cancel3' AND wait_event = 'PgSleep')}),
		'the third _info() pass is under way');
	$t0 = time();
	sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity
	      WHERE application_name = 'pssc_cancel3'});
	$h->finish;
	$elapsed = time() - $t0;
	like($r{err}, qr/canceling statement due to user request/, 'the third pass is canceled');
	ok($elapsed < 5, "promptly, within the last pass ($elapsed s, the pass takes 10 s)");
	unlike($r{out}, qr/\d/, 'no row');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# --------------------------------------------------------- _counters()
# The cheap counters-only read: every _info() column but oldest_bucket,
# from the same header snapshot, without walking the entries.
{
	sql("SELECT ${P}_reset()");
	my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	pin($b);
	sql("SELECT pssc_store_test_record(q) FROM generate_series(1, 7) q");
	sql(q{SELECT 1 /*a='1',bad='%00'*/});
	my @c = grep { $_ ne 'oldest_bucket' } @cols, qw(capped_tags cap_table_full);
	my $row = join(', ', @c);
	is(sql(qq{SELECT (SELECT row($row) FROM ${P}_counters())
	                 IS NOT DISTINCT FROM (SELECT row($row) FROM ${P}_info())}),
		't', '_counters() equals the corresponding _info() columns');
	is(sql(qq{SELECT entries || ' ' || invalid_tags || ' ' || (current_bucket_start
	                 = pssc_store_test_bucket_start($b)) FROM ${P}_counters()}),
		'8 1 true', '_counters(): the entries (7 + the tagged statement), a counter and the current bucket');
	is(sql(qq{SELECT pssc_store_test_info_scan_hook(0, 0, 0);
		SELECT count(*) FROM ${P}_counters();
		SELECT pssc_store_test_info_scan_hook_off()}), "\n1\n0",
		'_counters() does not walk the entries');
	is(sql(qq{SELECT pssc_store_test_info_scan_hook(0, 0, 0);
		SELECT count(*) FROM ${P}_info();
		SELECT pssc_store_test_info_scan_hook_off()}), "\n1\n8",
		'(_info() walks each of the 8 entries)');
	is(sql(qq{SELECT invalid_tags FROM ${P}_counters() /*a='1',bad='%00'*/}), 2,
		'_counters() sees the calling statement\'s own extraction counters');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# --------------------------------------------------------- oldest_bucket
{
	sql("SELECT ${P}_reset()");
	my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
	my $start = sub { sql("SELECT pssc_store_test_bucket_start($_[0])") };
	pin($b);
	sql('SELECT pssc_store_test_record(1)');
	is(info()->{oldest_bucket}, $start->($b), 'oldest_bucket: the bucket of the only slot');
	pin($b + 1);
	sql('SELECT pssc_store_test_record(2)');
	sql('SELECT pssc_store_test_record(1)');
	is(info()->{oldest_bucket}, $start->($b), 'oldest_bucket: the oldest live slot of any entry');
	is(info()->{oldest_bucket}, sql("SELECT min(bucket_start) FROM $P"),
		'oldest_bucket = the oldest bucket_start in the stats view');
	pin($b + 4);    # window [b+1, b+4]: slot b expired
	is(info()->{oldest_bucket}, $start->($b + 1), 'oldest_bucket skips expired slots');
	is(info()->{oldest_bucket}, sql("SELECT min(bucket_start) FROM $P"),
		'still the oldest bucket_start in the stats view');
	pin($b + 8);    # everything expired, entries still stored
	my $i = info();
	is("$i->{entries} $i->{oldest_bucket}", '2 NULL',
		'oldest_bucket NULL when no slot is live, though entries remain');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# ------------------------------------------- utility_missing_queryid
SKIP:
{
	skip 'pg_stat_statements is not installed', 2
	  unless defined pgss_suffix($node);
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = '$P, pg_stat_statements'\n");
	$node->restart;
	sql('CREATE EXTENSION pg_stat_statements');
	sql("SELECT ${P}_reset()");
	sql(q{CREATE TABLE info_wrong(i int) /*controller='x'*/});
	sql(q{DROP TABLE info_wrong});
	is(info()->{utility_missing_queryid}, 2,
		'wrong load order: utilities counted in utility_missing_queryid');
	is(info()->{entries}, 0, 'and not recorded');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/, 'no crash');

# ------------------------------------------------------ not preloaded
{
	my $np = PostgreSQL::Test::Cluster->new('info_nopreload');
	$np->init;
	$np->start;
	$np->safe_psql('postgres', "CREATE EXTENSION $P");
	for my $q ("SELECT * FROM ${P}_info()", "SELECT * FROM ${P}_counters()",
		"SELECT ${P}_reset()",
		"SELECT * FROM $P", "SELECT * FROM ${P}_totals", "SELECT * FROM ${P}_last_bucket")
	{
		my ($ret, $out, $err) = $np->psql('postgres', $q);
		like($err, qr/ERROR:  $P must be loaded via "shared_preload_libraries"/,
			"not preloaded: $q raises the preload error");
	}
	$np->stop;
}

done_testing();
