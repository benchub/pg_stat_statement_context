# Persistence across clean restarts (DESIGN.md §5.5, §8; backlog
# 20261005-091225-35). With pg_stat_statement_context.save = on (the
# default, as pg_stat_statements.save), the postmaster dumps the store to
# pg_stat/pg_stat_statement_context.stat when it exits after a clean
# shutdown, and loads (then unlinks) it at the next start:
#   - the entries, their rings, usage, monotonic counters, stats_since, the
#     eviction array order, the header counters, stats_reset and the epoch
#     survive; recording finds the reloaded entries again;
#   - slots and entries that expired by the load are dropped (dead entries
#     count as reclaimed);
#   - a smaller max_entries loads what fits and evicts the rest in the §5.3
#     order (last_bucket, then usage); a larger one loads everything;
#   - entries whose tag set no longer fits max_tagset_bytes are skipped and
#     counted in one LOG line;
#   - a changed bucket_interval or bucket_count, a different file format,
#     PostgreSQL major or extension version, and a corrupt, truncated or
#     foreign file are discarded with a LOG line (the server starts with an
#     empty store);
#   - nothing is saved with save = off (and a saved file is discarded), after
#     an immediate shutdown or a crash.
# The TEST-ONLY module test/modules/pssc_store_test records entries
# directly and reads the raw entries; its debug clock is pinned where
# bucket boundaries matter (it lives in shared memory, so a restart clears it).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('persist');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
restart_after_crash = on
$P.max_entries = 100
});
$node->start;
$node->safe_psql('postgres', "CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");

my $DUMP = $node->data_dir . "/pg_stat/$P.stat";

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub rec { return sql("SELECT pssc_store_test_record($_[0])"); }

# The raw entries (every written slot, expired or not), in a stable order.
sub raw_entries
{
	return sql(q{SELECT string_agg(concat_ws('|', dbid, userid, queryid, toplevel, tags,
	               tags_len, tags_hash, encoding, last_bucket, usage, slot, bucket_id,
	               calls, total_exec_time), E'\n'
	             ORDER BY queryid, tags::text, toplevel, slot)
	             FROM pssc_store_test_entries()});
}
sub view_rows
{
	return sql(qq{SELECT string_agg(concat_ws('|', bucket_start, userid, dbid, queryid,
	                toplevel, tags, calls, total_exec_time, calls_total, exec_time_total,
	                stats_since), E'\n' ORDER BY queryid, tags::text, toplevel, bucket_start)
	              FROM $P});
}
sub evict_order
{
	return sql(q{SELECT string_agg(idx || ':' || queryid || ':' || tags::text || ':'
	               || last_bucket || ':' || usage, ',' ORDER BY idx)
	             FROM pssc_store_test_evict_slots()});
}
my @info_cols = qw(entries max_entries dealloc reclaimed_entries evicted_entries
  dropped_records buckets bucket_seconds invalid_tags dropped_tags
  heuristic_scans regex_compile_failures utility_missing_queryid capped_tags
  cap_table_full stats_reset stats_reset_epoch);
sub info
{
	my $sel = join(" || '|' || ", map { "coalesce(${_}::text, 'NULL')" } @info_cols);
	my @v = split /\|/, sql("SELECT $sel FROM ${P}_info()"), -1;
	my %h;
	@h{@info_cols} = @v;
	return \%h;
}
sub info_str { my $i = info(); return join(' ', map { "$_=$i->{$_}" } @info_cols); }
sub entries { return info()->{entries}; }
sub queryids
{
	return sql(q{SELECT string_agg(DISTINCT queryid::text, ',' ORDER BY queryid::text)
	             FROM pssc_store_test_evict_slots()}) // '';
}
sub epoch { return sql('SELECT epoch FROM pssc_store_test_buckets()'); }
sub watermark { return sql('SELECT current_bucket FROM pssc_store_test_buckets()'); }
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}
sub fresh_bucket { return sql('SELECT reader_bucket + 1 FROM pssc_store_test_buckets()'); }
sub invariants { return sql('SELECT pssc_store_test_check_invariants()'); }

# Stops ($mode, default fast), optionally runs $between while the server is
# down, starts it, and returns the server log written since before the stop.
sub restart
{
	my ($mode, $between) = @_;
	my $pos = -s $node->logfile;
	$node->stop($mode // 'fast');
	$between->() if $between;
	$node->start;
	return substr(slurp_file($node->logfile), $pos);
}
sub stop_only
{
	my ($mode) = @_;
	my $pos = -s $node->logfile;
	$node->stop($mode // 'fast');
	return $pos;
}
sub start_from
{
	my ($pos) = @_;
	$node->start;
	return substr(slurp_file($node->logfile), $pos);
}

# Overwrites bytes of the stopped server's dump file at $offset.
sub patch_dump
{
	my ($offset, $bytes) = @_;
	open my $fh, '+<:raw', $DUMP or die "open $DUMP: $!";
	seek($fh, $offset, 0) or die "seek: $!";
	print $fh $bytes or die "write: $!";
	close $fh or die "close: $!";
}

is(sql("SHOW $P.save"), 'on', 'save is on by default (as pg_stat_statements.save)');
is(sql("SELECT context FROM pg_settings WHERE name = '$P.save'"), 'sighup',
	'save is reloadable (sighup), as pg_stat_statements.save');

# ------------------------------------------------------------ happy path
{
	# 101 entries into a 100-entry table: one eviction pass (5 evicted), so
	# dealloc and evicted_entries are not zero.
	is(sql(q{SELECT count(*) FROM generate_series(1001, 1101) q
	         WHERE pssc_store_test_record(q) NOT IN ('inserted', 'updated')}), 0,
		'recorded 101 combinations');
	sql('SELECT pssc_store_test_add_stats(1, 2, 3, 4)');
	sql('SELECT pssc_store_test_utility_missing_queryid()');
	sql(q{SELECT 1 /*controller='c1',action='a1'*/}) for 1 .. 3;
	sql(q{SELECT pssc_store_test_record(7, ARRAY['k', 'v'], NULL, 2.5, false)});
	# a capped value is stored as a null (key \0 \0 \0, §6.1)
	$node->append_conf('postgresql.conf', "$P.cardinality_cap = 1");
	$node->reload;
	$node->poll_query_until('postgres', "SELECT current_setting('$P.cardinality_cap') = '1'")
	  or die 'reload';
	sql(q{SELECT 2 /*job='j1'*/});
	sql(q{SELECT 2 /*job='j2'*/});
	is(sql(qq{SELECT string_agg(tags::text, ' ' ORDER BY tags::text COLLATE "C") FROM ${P}_totals
	          WHERE tags ? 'job'}), '{"job": "j1"} {"job": null}', 'before: a capped (null) value');
	my $i = info();
	is("$i->{entries} $i->{dealloc} $i->{evicted_entries} $i->{capped_tags}", '100 1 5 1',
		'before: 100 entries after one eviction pass');

	my $raw = raw_entries();
	my $view = view_rows();
	my $order = evict_order();
	my $info = info_str();
	my $epoch = epoch();
	my $wm = watermark();

	my $pos = stop_only();
	ok(-f $DUMP, 'a clean (fast) shutdown writes the dump file');
	ok(!-e "$DUMP.tmp", 'no temporary file left behind');
	my $log = start_from($pos);
	ok(!-e $DUMP, 'the dump file is unlinked after a successful load');
	like($log, qr/loaded 100 of 100 saved entries/, 'LOG: the load is reported');

	is(raw_entries(), $raw, 'raw entries, rings, usage and hashes survive');
	is(view_rows(), $view, 'the stats view shows the same rows');
	is(evict_order(), $order, 'the eviction array keeps its order');
	is(info_str(), $info, '_info() counters and stats_reset survive');
	is(epoch(), $epoch, 'the epoch is kept, so bucket ids stay valid');
	ok(watermark() >= $wm, 'current_bucket never goes back');
	is(invariants(), 100, 'ring and eviction array invariants hold');

	sql(q{SELECT 1 /*controller='c1',action='a1'*/});
	is(sql(qq{SELECT calls_total FROM ${P}_totals WHERE tags->>'controller' = 'c1'}), 4,
		'recording finds a reloaded entry (no duplicate)');
	is(rec('1050'), 'updated', 'the test driver finds one too');
	is(entries(), 100, 'no entry added');
}

# ------------------------ expired slots and dead entries are dropped
{
	sql("SELECT ${P}_reset()");
	my $b = fresh_bucket();
	pin($b);
	rec(2001);					# D: dead once the watermark is b + 12
	rec(2002);					# Z: slots b (expires) and b + 5 (stays)
	pin($b + 5);
	rec(2002);
	pin($b + 12);
	rec(2003);					# L: live
	is(sql(q{SELECT string_agg(queryid || ':' || (bucket_id - } . $b
		  . q{) || ':' || live, ',' ORDER BY queryid, bucket_id) FROM pssc_store_test_entries()}),
		'2001:0:false,2002:0:false,2002:5:true,2003:12:true',
		'before: D dead, Z with one expired slot');

	my $log = restart();
	like($log, qr/loaded 2 of 3 saved entries/, 'LOG: two of three loaded');
	is(sql(q{SELECT string_agg(queryid || ':' || (bucket_id - } . $b
		  . q{) || ':' || live, ',' ORDER BY queryid, bucket_id) FROM pssc_store_test_entries()}),
		'2002:5:true,2003:12:true',
		'the dead entry and the expired slot are gone');
	is(watermark(), $b + 12, 'current_bucket restored (ahead of the real clock)');
	my $i = info();
	is("$i->{entries} $i->{reclaimed_entries} $i->{evicted_entries} $i->{dealloc}", '2 1 0 0',
		'the dead entry counts as reclaimed');
	is(invariants(), 2, 'invariants hold');
}

# --------------------------------------- max_entries shrinks, then grows
{
	$node->append_conf('postgresql.conf', "$P.max_entries = 200");
	restart();
	sql("SELECT ${P}_reset()");
	my $b = fresh_bucket();
	pin($b);
	# 1..10: five calls in bucket b (usage 6, but the oldest last_bucket);
	# 11..20: one call in b and one in b + 1 (usage 3); 21..120: one in b
	# and two in b + 1 (usage 4).
	sql(q{SELECT pssc_store_test_record(q) FROM generate_series(1, 10) q, generate_series(1, 5)});
	sql(q{SELECT pssc_store_test_record(q) FROM generate_series(11, 120) q});
	pin($b + 1);
	sql(q{SELECT pssc_store_test_record(q) FROM generate_series(11, 120) q});
	sql(q{SELECT pssc_store_test_record(q) FROM generate_series(21, 120) q});
	is(entries(), 120, 'before: 120 entries');
	my $survivors = sql(q{SELECT string_agg(queryid || ':' || usage, ',' ORDER BY idx)
	                      FROM pssc_store_test_evict_slots() WHERE queryid >= 21});

	$node->append_conf('postgresql.conf', "$P.max_entries = 100");
	my $log = restart();
	like($log, qr/max_entries shrank from 200 to 100: evicted 20 saved entries/,
		'LOG: the shrink is reported');
	like($log, qr/loaded 100 of 120 saved entries/, 'LOG: 100 of 120 loaded');
	is(queryids(), join(',', sort map { "$_" } 21 .. 120),
		'evicted in §5.3 order: oldest last_bucket first (1..10), then lowest usage (11..20)');
	my $i = info();
	is("$i->{entries} $i->{max_entries} $i->{evicted_entries} $i->{dealloc} $i->{reclaimed_entries}",
		'100 100 20 1 0', 'evictions counted as one pass');
	is(invariants(), 100, 'invariants hold');
	is(sql(q{SELECT string_agg(queryid || ':' || usage, ',' ORDER BY idx)
	         FROM pssc_store_test_evict_slots()}), $survivors,
		'the survivors keep their usage and relative order');

	$node->append_conf('postgresql.conf', "$P.max_entries = 300");
	$log = restart();
	like($log, qr/loaded 100 of 100 saved entries/, 'a larger max_entries loads everything');
	is(info()->{max_entries}, 300, 'with the new size');
	is(invariants(), 100, 'invariants hold');
}

# ------------------------------------- max_tagset_bytes shrinks
{
	sql("SELECT ${P}_reset()");
	my $long = 'v' x 200;
	sql(qq{SELECT pssc_store_test_record(3001, ARRAY['k', '$long'])});
	sql(q{SELECT pssc_store_test_record(3002, ARRAY['k', 'small'])});
	$node->append_conf('postgresql.conf', "$P.max_tagset_bytes = 128");
	my $log = restart();
	like($log, qr/skipped 1 saved entries whose tag set exceeds max_tagset_bytes \(128\)/,
		'LOG: one entry skipped for its tag set');
	like($log, qr/loaded 1 of 2 saved entries/, 'LOG: one loaded');
	is(queryids(), '3002', 'the entry that still fits is loaded');
	is(invariants(), 1, 'invariants hold');
}

# ---------------------------- bucket_interval / bucket_count changed
{
	rec(4001);
	$node->append_conf('postgresql.conf', "$P.bucket_interval = '600s'");
	my $log = restart();
	like($log, qr/discarding saved statistics in ".*$P\.stat": bucket_interval or bucket_count changed/,
		'LOG: a new bucket_interval discards the file');
	is(entries(), 0, 'the store starts empty');
	ok(!-e $DUMP, 'the discarded file is removed');

	rec(4002);
	$node->append_conf('postgresql.conf', "$P.bucket_count = 6");
	$log = restart();
	like($log, qr/bucket_interval or bucket_count changed/,
		'LOG: a new bucket_count discards the file');
	is(entries(), 0, 'the store starts empty');
	is(rec(4003), 'inserted', 'recording works');
}

# ----------------------------------- version mismatches and corruption
# Header layout (src/store.c, PsscDumpHeader): magic uint32 at 0, format
# version uint32 at 4, PostgreSQL major uint32 at 8, extension version
# char[20] at 12.
{
	my @cases = (
		[ 'format version', sub { patch_dump(4, pack('L', 0xdead)) },
			qr/discarding saved statistics in ".*": written by a different version/ ],
		[ 'PostgreSQL major version', sub { patch_dump(8, pack('L', 9)) },
			qr/discarding saved statistics in ".*": written by a different version/ ],
		[ 'extension version', sub { patch_dump(12, "0.0\0") },
			qr/discarding saved statistics in ".*": written by a different version/ ],
		[ 'bad magic number', sub { patch_dump(0, pack('L', 0x12345678)) },
			qr/ignoring invalid data in file ".*$P\.stat"/ ],
		[ 'a flipped byte (checksum)', sub {
			my $size = -s $DUMP;
			open my $fh, '+<:raw', $DUMP or die;
			seek($fh, int($size / 2), 0);
			read($fh, my $c, 1);
			seek($fh, int($size / 2), 0);
			print $fh chr(ord($c) ^ 0x01);
			close $fh;
		}, qr/ignoring invalid data in file ".*$P\.stat"/ ],
		[ 'a truncated file', sub { truncate($DUMP, (-s $DUMP) - 10) or die },
			qr/(ignoring invalid data in|could not read) file ".*$P\.stat"/ ],
		[ 'a short garbage file', sub {
			open my $fh, '>:raw', $DUMP or die;
			print $fh 'garbage';
			close $fh;
		}, qr/(ignoring invalid data in|could not read) file ".*$P\.stat"/ ],
		[ 'trailing garbage', sub {
			open my $fh, '>>:raw', $DUMP or die;
			print $fh 'x';
			close $fh;
		}, qr/ignoring invalid data in file ".*$P\.stat"/ ]);
	for my $c (@cases)
	{
		my ($what, $patch, $re) = @$c;
		sql("SELECT ${P}_reset()");
		rec(5001);
		rec(5002);
		my $log = restart('fast', sub {
			ok(-f $DUMP, "$what: dump written");
			$patch->();
		});
		like($log, $re, "$what: LOG message");
		unlike($log, qr/PANIC|terminated by signal|FATAL/, "$what: no crash");
		is(entries(), 0, "$what: the store starts empty");
		ok(!-e $DUMP, "$what: the bad file is removed");
		is(rec(5003), 'inserted', "$what: recording works");
	}
}

# --------------------------- a tags_hash that does not match its tags
# Defense in depth (SEC-7): the loader recomputes every record's tags_hash
# from its tags. The test driver stores an entry under a wrong tags_hash, so
# the clean shutdown writes it into a dump whose checksum is valid.
{
	sql("SELECT ${P}_reset()");
	rec(5101);
	is(rec(q{5102, ARRAY['k', 'v']}), 'inserted', 'an entry with its true tags_hash');
	my $true_hash = sql(q{SELECT tags_hash FROM pssc_store_test_entries() WHERE queryid = 5102 LIMIT 1});
	my $bad_hash = ($true_hash + 1) % 4294967296;
	is(rec(qq{5103, ARRAY['k', 'v'], NULL, 1.0, true, $bad_hash}), 'inserted',
		'an entry with the same tags under another tags_hash');
	my $log = restart('fast', sub { ok(-f $DUMP, 'tags_hash: dump written'); });
	like($log, qr/ignoring invalid data in file ".*$P\.stat"/,
		'tags_hash: a tags_hash that does not match the tags is rejected with a LOG message');
	unlike($log, qr/loaded \d+ of \d+ saved entries/, 'tags_hash: nothing is loaded');
	unlike($log, qr/PANIC|terminated by signal|FATAL/, 'tags_hash: no crash');
	is(entries(), 0, 'tags_hash: the store starts empty');
	ok(!-e $DUMP, 'tags_hash: the bad file is removed');

	# Control: the same dump with only true hashes loads.
	sql("SELECT ${P}_reset()");
	rec(5101);
	rec(q{5102, ARRAY['k', 'v']});
	$log = restart('fast');
	like($log, qr/loaded 2 of 2 saved entries/, 'tags_hash: true hashes load');
	is(entries(), 2, 'tags_hash: both entries are back');
}

# ----------------------------------------------------------- save = off
{
	sql("SELECT ${P}_reset()");
	$node->append_conf('postgresql.conf', "$P.save = off");
	$node->reload;
	$node->poll_query_until('postgres', "SELECT current_setting('$P.save') = 'off'")
	  or die 'reload';
	rec(6001);
	my $log = restart('fast', sub { ok(!-e $DUMP, 'save = off: nothing is written'); });
	is(entries(), 0, 'save = off: the store starts empty');

	# A file saved with save = on is discarded by a start with save = off.
	$node->append_conf('postgresql.conf', "$P.save = on");
	$node->reload;
	$node->poll_query_until('postgres', "SELECT current_setting('$P.save') = 'on'")
	  or die 'reload';
	rec(6002);
	$log = restart('fast', sub {
		ok(-f $DUMP, 'save = on (by reload): written');
		$node->append_conf('postgresql.conf', "$P.save = off");
	});
	like($log, qr/discarding saved statistics in ".*": $P\.save is off/,
		'LOG: a start with save = off discards the file');
	ok(!-e $DUMP, 'and removes it');
	is(entries(), 0, 'nothing loaded');
	$node->append_conf('postgresql.conf', "$P.save = on");
	$node->reload;
	$node->poll_query_until('postgres', "SELECT current_setting('$P.save') = 'on'")
	  or die 'reload';
}

# ------------------------------------------------ crashes save nothing
{
	sql("SELECT ${P}_reset()");
	rec(7001);
	my $log = restart('immediate', sub {
		ok(!-e $DUMP, 'immediate shutdown: nothing is written');
	});
	like($log, qr/not saving statistics: the server did not shut down cleanly/,
		'LOG: an immediate shutdown is not a clean one');
	is(entries(), 0, 'immediate shutdown: the store starts empty');

	# Loaded, then a crash (a child killed): the consumed file is gone, so
	# nothing stale is replayed after the crash restart.
	rec(7002);
	restart();
	is(queryids(), '7002', 'loaded after a clean restart');
	my $pos = -s $node->logfile;
	my $ckpt = sql(q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'});
	kill 'KILL', $ckpt or die "kill $ckpt: $!";
	# wait until the postmaster has reinitialized, then until it accepts
	# connections again
	for (my $t = 0;; $t++)
	{
		$log = substr(slurp_file($node->logfile), $pos);
		last if $log =~ /all server processes terminated; reinitializing/;
		die "no crash restart" if $t > 1800;
		select(undef, undef, undef, 0.1);
	}
	$node->poll_query_until('postgres', "SELECT 1", '1') or die 'crash restart';
	isnt(sql(q{SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer'}), $ckpt,
		'a new checkpointer');
	$log = substr(slurp_file($node->logfile), $pos);
	like($log, qr/terminated by signal 9/, 'the checkpointer was killed (crash restart)');
	like($log, qr/not saving statistics: the server did not shut down cleanly/,
		'LOG: nothing saved at the crash');
	ok(!-e $DUMP, 'no file');
	is(entries(), 0, 'after the crash restart the store is empty');
}

$node->stop;

# --------------- entries that expired in real time during the downtime
{
	my $short = PostgreSQL::Test::Cluster->new('persist_short');
	$short->init;
	$short->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
huge_pages = off
$P.bucket_interval = '1s'
$P.bucket_count = 2
});
	$short->start;
	$short->safe_psql('postgres', "CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");
	$short->safe_psql('postgres', 'SELECT pssc_store_test_record(8001)');
	my $pos = -s $short->logfile;
	$short->stop;
	ok(-f $short->data_dir . "/pg_stat/$P.stat", 'short buckets: saved');
	sleep 3;					# more than the 2 s window
	$short->start;
	my $log = substr(slurp_file($short->logfile), $pos);
	like($log, qr/loaded 0 of 1 saved entries/, 'an entry that expired during the downtime is dropped');
	is($short->safe_psql('postgres', "SELECT entries || ' ' || reclaimed_entries FROM ${P}_info()"),
		'0 1', 'and counted as reclaimed');
	$short->stop;
}

done_testing();
