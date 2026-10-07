# Per-key cardinality caps (DESIGN.md §6.11 step 8; backlog
# 20261005-091225-32): pg_stat_statement_context.cardinality_cap bounds the
# number of distinct values of each allowed key, counted per (role,
# database) by default (cardinality_cap_scope; 030_cap_scope.pl) but not per
# bucket or queryid, since the last _reset(); values beyond the cap collapse
# to JSON null before the entry's key is built. cardinality_cap_overrides
# sets per-key caps ("key:N, ..."; N = 0 exempts a key) that take
# precedence over the global default, and cardinality_cap_slots sizes the
# shared tracking table (postmaster), which fails closed (null) when full.
#
# Covers: the GUCs and their validation; flooding a key leaves at most cap
# distinct strings plus null (serially and from concurrent sessions);
# overrides beat the default; null is JSON null and no client input
# produces it (the string 'null' and '' stay strings); _info().capped_tags
# and cap_table_full; a full table fails closed; _reset() clears the sets;
# _extract() reports what would collapse without admitting anything; caps
# apply to tags_override and appname tags and after normalize/truncation;
# admitted values stay strings when the cap is lowered; cap_shmem_bytes;
# a _reset() that wraps the table generation around is serialized with other
# resets and admits nothing while it clears the table.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('caps');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = ''
$P.extractors = 'sqlcommenter, marginalia, appname(format=sqlcommenter)'
$P.max_tag_value_len = 8
$P.track_utility = off
# restarts must start from an empty store (counters included); persistence
# is covered by test/t/028_persist.pl
$P.save = off
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

sql("CREATE EXTENSION $P; CREATE TABLE t(i int)");

my $sentinel = 1000;
sub config
{
	my (%c) = @_;
	$sentinel++;
	sql(join('', map {
			defined $c{$_} ? "ALTER SYSTEM SET $P.$_ = " . sqlq($c{$_}) . ";\n"
			  : "ALTER SYSTEM RESET $P.$_;\n"
		} sort keys %c)
		  . "ALTER SYSTEM SET $P.scan_window = $sentinel;");
	$node->reload;
	$node->poll_query_until('postgres',
		"SELECT setting::int = $sentinel FROM pg_settings WHERE name = '$P.scan_window'")
	  or die "reload not processed (sentinel $sentinel)";
}

# One statement per value of key, in one multi-statement query string.
sub flood
{
	my ($key, @values) = @_;
	sql(join('', map { "SELECT * FROM t /*$key:$_*/;\n" } @values));
}

# "strings nulls" for key over the whole view: distinct string values, and
# whether a null value was recorded.
sub distinct
{
	my ($key) = @_;
	return sql("SELECT count(DISTINCT tags->>'$key') FILTER (WHERE jsonb_typeof(tags->'$key') = 'string')"
		  . " || ' ' || count(*) FILTER (WHERE jsonb_typeof(tags->'$key') = 'null')"
		  . " FROM ${P}_totals WHERE tags ? '$key'");
}

sub info { return sql("SELECT $_[0] FROM ${P}_info()"); }

# ---------------------------------------------------------------------------
# GUCs
# ---------------------------------------------------------------------------
is( sql("SELECT string_agg(name || '=' || setting || '/' || context, ' ' ORDER BY name COLLATE \"C\")"
		  . " FROM pg_settings WHERE name LIKE '$P.cardinality%'"),
	"$P.cardinality_cap=0/sighup $P.cardinality_cap_overrides=/sighup "
	  . "$P.cardinality_cap_scope=role/postmaster "
	  . "$P.cardinality_cap_slots=16384/postmaster",
	'GUCs: names, defaults (caps off) and contexts');

foreach my $bad ('route', 'route:', ':5', 'route:x', 'route:-1', 'route:5x',
	'a b:5', '*:5', 'route:5,route:6', 'route:5,', 'route:1000001',
	('k' x 64) . ':5')
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.cardinality_cap_overrides = " . sqlq($bad));
	ok($ret != 0 && $err =~ /invalid value for parameter/,
		"overrides rejected: '$bad'")
	  or diag($err);
}
foreach my $good ('', 'route:500', ' route : 5 , job:0 ', 'a:b:3, x:2')
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.cardinality_cap_overrides = " . sqlq($good));
	is($ret, 0, "overrides accepted: '$good'") or diag($err);
}
sql("ALTER SYSTEM RESET $P.cardinality_cap_overrides");
my ($ret, $out, $err) = $node->psql('postgres', "ALTER SYSTEM SET $P.cardinality_cap = -1");
ok($ret != 0, 'cardinality_cap: negative rejected');
($ret, $out, $err) = $node->psql('postgres', "SET $P.cardinality_cap = 5");
ok($ret != 0 && $err =~ /cannot be changed now/, 'cardinality_cap: sighup, not SET');

# ---------------------------------------------------------------------------
# Off by default: every value is kept
# ---------------------------------------------------------------------------
sql("SELECT ${P}_reset()");
flood('route', map { "r$_" } 1 .. 30);
is(distinct('route'), '30 0', 'cap off: 30 distinct values, no null');
is(info('capped_tags || \' \' || cap_table_full'), '0 0', 'cap off: nothing capped');

# ---------------------------------------------------------------------------
# Flood one key: at most cap strings plus null
# ---------------------------------------------------------------------------
config(cardinality_cap => 5);
sql("SELECT ${P}_reset()");
flood('route', map { "v$_" } 1 .. 40);
is(distinct('route'), '5 1', 'flood: 5 distinct strings plus null');
is(sql("SELECT string_agg(DISTINCT tags->>'route', ',') FROM ${P}_totals WHERE jsonb_typeof(tags->'route') = 'string'"),
	'v1,v2,v3,v4,v5', 'flood: the first 5 values are kept');
is(info('capped_tags || \' \' || cap_table_full'), '35 0',
	'_info(): 35 collapses counted');
is(sql("SELECT sum(calls) FROM ${P}_totals WHERE tags = '{\"route\": null}'"), '35',
	'one null entry with the 35 overflow calls');
is(sql("SELECT tags::text FROM ${P}_totals WHERE jsonb_typeof(tags->'route') = 'null'"),
	'{"route": null}', 'null is JSON null in tags');
# admitted values stay; the cap is per key, not global
flood('route', 'v3', 'v41');
flood('action', map { "a$_" } 1 .. 7);
is(distinct('route'), '5 1', 'flood: admitted values stay, new ones collapse');
is(distinct('action'), '5 1', 'cap per key: action has its own 5');
is(sql("SELECT sum(calls) FROM ${P}_totals WHERE tags->>'route' = 'v3'"), '2',
	'admitted value v3 recorded again as a string');

# client input never produces null: 'null' and '' are strings
sql("SELECT ${P}_reset()");
sql(q{SELECT * FROM t /*k='null'*/; SELECT * FROM t /*k=''*/; SELECT * FROM t /*k:null*/;});
is(sql("SELECT string_agg(tags::text, ' ' ORDER BY tags::text COLLATE \"C\") FROM ${P}_totals WHERE tags ? 'k'"),
	'{"k": ""} {"k": "null"}', "'null' and '' stay strings");
is(sql("SELECT count(*) FROM ${P}_totals WHERE tags ? 'k' AND jsonb_typeof(tags->'k') = 'null'"),
	'0', 'no null from client input');

# ---------------------------------------------------------------------------
# _reset() clears the sets
# ---------------------------------------------------------------------------
sql("SELECT ${P}_reset()");
flood('route', map { "w$_" } 1 .. 8);
is(distinct('route'), '5 1', 'reset: new values admitted again');
is(sql("SELECT string_agg(DISTINCT tags->>'route', ',') FROM ${P}_totals WHERE jsonb_typeof(tags->'route') = 'string'"),
	'w1,w2,w3,w4,w5', 'reset: the earlier values no longer count');
is(info('capped_tags'), '3', 'reset: capped_tags restarted');

# lowering the cap keeps admitted values; raising it admits more
config(cardinality_cap => 2);
flood('route', 'w4', 'x1');
is(sql("SELECT count(*) FROM ${P}_totals WHERE tags->>'route' = 'w4'"), '1',
	'lowered cap: admitted w4 still a string');
config(cardinality_cap => 7);
flood('route', 'x2', 'x3', 'x4');
is(distinct('route'), '7 1', 'raised cap: 2 more admitted');

# ---------------------------------------------------------------------------
# Overrides take precedence over the default
# ---------------------------------------------------------------------------
config(cardinality_cap => 3, cardinality_cap_overrides => 'route:6, job:0');
sql("SELECT ${P}_reset()");
flood('route', map { "r$_" } 1 .. 20);
flood('job', map { "j$_" } 1 .. 20);
flood('action', map { "a$_" } 1 .. 20);
is(distinct('route'), '6 1', 'override route:6 beats the default 3');
is(distinct('job'), '20 0', 'override job:0 exempts the key');
is(distinct('action'), '3 1', 'other keys use the default');
config(cardinality_cap => 0, cardinality_cap_overrides => 'route:2');
sql("SELECT ${P}_reset()");
flood('route', map { "r$_" } 1 .. 10);
flood('action', map { "a$_" } 1 .. 10);
is(distinct('route'), '2 1', 'override alone (default off) caps its key');
is(distinct('action'), '10 0', 'default off: other keys uncapped');

# ---------------------------------------------------------------------------
# All sources, after normalize and truncation
# ---------------------------------------------------------------------------
config(cardinality_cap => 2, cardinality_cap_overrides => undef,
	normalize => q{route: '/\d+' => '/:id'});
sql("SELECT ${P}_reset()");
flood('route', '/u/1', '/u/22', '/p/3', '/q/4');
is(sql("SELECT string_agg(x, ',' ORDER BY x COLLATE \"C\") FROM (SELECT coalesce(tags->>'route', 'NULL') || '=' || calls AS x FROM ${P}_totals WHERE tags ? 'route') s"),
	'/p/:id=1,/u/:id=2,NULL=1', 'caps apply after normalize');
# max_tag_value_len = 8: two values with the same 8-byte prefix are one
flood('long', 'abcdefgh1', 'abcdefgh2', 'zzzzzzzz', 'yyyyyyyy');
is(distinct('long'), '2 1', 'caps apply after truncation');
sql("SELECT ${P}_reset()");
sql("SELECT * FROM t /*s='1'*/");
$node->safe_psql('postgres',
	"SET $P.tags_override = 's=''2'''; SELECT * FROM t;");
$node->safe_psql('postgres',
	"SET application_name = 's=''3'''; SELECT * FROM t /*s='1'*/;");
$node->safe_psql('postgres',
	"SET application_name = 's=''4'''; SELECT * FROM t /*z='1'*/;");
is(sql("SELECT string_agg(tags::text || '=' || calls, ' ' ORDER BY tags::text COLLATE \"C\") FROM ${P}_totals WHERE tags ? 's'"),
	'{"s": "1"}=2 {"s": "2"}=1 {"s": null, "z": "1"}=1',
	'tags_override and appname values are capped too');
config(normalize => undef);

# ---------------------------------------------------------------------------
# _extract() peeks: reports what would collapse, admits nothing
# ---------------------------------------------------------------------------
sql("SELECT ${P}_reset()");
is(sql("SELECT string_agg(${P}_extract('SELECT 1 /*e:p' || i || '*/')->>'capped_tags', ',') FROM generate_series(1, 5) i"),
	'0,0,0,0,0', '_extract(): values under the cap are not collapsed');
flood('e', 'e1', 'e2', 'e3');
is(distinct('e'), '2 1', '_extract() did not use up the cap');
is(sql("SELECT ${P}_extract('SELECT 1 /*e:new*/')->>'tags'"),
	'{"e": null}', '_extract(): a new value at the cap shows as null');
is(sql("SELECT ${P}_extract('SELECT 1 /*e:new*/')->>'capped_tags'"), '1',
	'_extract(): capped_tags');
is(sql("SELECT ${P}_extract('SELECT 1 /*e:e1*/')->>'tags'"), '{"e": "e1"}',
	'_extract(): an admitted value stays');
is(info('capped_tags'), '1', '_extract() does not count in _info()');

# ---------------------------------------------------------------------------
# Concurrent sessions: still exactly cap distinct strings
# ---------------------------------------------------------------------------
config(cardinality_cap => 20);
sql("SELECT ${P}_reset()");
my @h;
foreach my $s (1 .. 4)
{
	my $script = join('', map { "SELECT * FROM t /*c:" . (($_ * 7 + $s * 13) % 300) . "*/;\n" } 1 .. 300);
	my ($in, $o, $e) = ($script, '', '');
	push @h, IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres') ],
		'<', \$in, '>', \$o, '2>', \$e);
}
$_->finish foreach @h;
is(distinct('c'), '20 1', 'concurrent flood: exactly 20 distinct strings plus null');
is(sql("SELECT sum(calls) FROM ${P}_totals WHERE tags ? 'c'"), '1200', 'concurrent flood: every call recorded');
is(info('capped_tags'), '1200' - sql("SELECT sum(calls) FROM ${P}_totals WHERE jsonb_typeof(tags->'c') = 'string'"),
	'concurrent flood: capped_tags = calls recorded as null');

# ---------------------------------------------------------------------------
# A full tracking table fails closed
# ---------------------------------------------------------------------------
config(cardinality_cap => 100000);
sql("ALTER SYSTEM SET $P.cardinality_cap_slots = 256");
$node->restart;
is(sql("SHOW $P.cardinality_cap_slots"), '256', 'cardinality_cap_slots = 256');
is(info('cap_shmem_bytes'),
	sql("SELECT size FROM pg_shmem_allocations WHERE name = '$P cardinality caps'"),
	'cap_shmem_bytes: exact size of the cap table');
# 8 bytes per word (up to 16 with emulated 64-bit atomics)
ok(info('cap_shmem_bytes') > (256 + 2 * 64) * 8 && info('cap_shmem_bytes') <= (256 + 2 * 64) * 16 + 64,
	'cap_shmem_bytes follows cardinality_cap_slots (256 values, 64 key slots)');
flood('f', map { "f$_" } 1 .. 600);
my ($strings, $nulls) = split / /, distinct('f');
ok($strings > 0 && $strings <= 256 && $nulls == 1,
	"full table: $strings strings (<= 256) plus null");
is(info('cap_table_full'), 600 - $strings, 'full table: cap_table_full counts the rest');
is(info('capped_tags'), 600 - $strings, 'full table: also counted in capped_tags');
flood('f', 'f1');
is(sql("SELECT calls FROM ${P}_totals WHERE tags->>'f' = 'f1'"), '2', 'full table: admitted values still kept');
sql("SELECT ${P}_reset()");
flood('f', 'g1');
is(distinct('f'), '1 0', 'full table: _reset() frees it');
($ret, $out, $err) = $node->psql('postgres', "ALTER SYSTEM SET $P.cardinality_cap_slots = 100");
ok($ret != 0, 'cardinality_cap_slots: below the minimum rejected');

# ---------------------------------------------------------------------------
# _reset() wrapping the table generation around clears the table; a second
# _reset() waits for it, and no value is admitted in the middle of the clear
# (the clear would otherwise leave an admitted value without its count)
# ---------------------------------------------------------------------------
config(cardinality_cap => 1);
sql("CREATE EXTENSION pssc_store_test");
sql("SELECT ${P}_reset()");
flood('w', 'v1', 'v2');
sql("SELECT pssc_store_test_cap_near_wrap(); SELECT ${P}_reset()");
flood('w', 'v2', 'v1');
is(sql("SELECT x FROM (SELECT coalesce(tags->>'w', 'NULL') AS x FROM ${P}_totals WHERE tags ? 'w') s ORDER BY x COLLATE \"C\""),
	"NULL\nv2", 'wrapping _reset(): earlier values no longer count');

# a background psql session named $app running $script
sub bg_psql
{
	my ($app, $script) = @_;
	my ($o, $e) = ('', '');
	# -c, not stdin: IPC::Run feeds stdin only while the harness is pumped
	my $h = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d',
			$node->connstr('postgres') . " application_name=$app", '-c', $script ],
		'>', \$o, '2>', \$e);
	return { h => $h, out => \$o, err => \$e };
}

my $release = $node->basedir . '/cap_clear_release';
unlink $release;
sql("SELECT ${P}_reset()");
sql('SELECT pssc_store_test_cap_near_wrap()');
my $ra = bg_psql('cap_reset_a',
	"SELECT pssc_store_test_stall_next_cap_clear('$release'); SELECT ${P}_reset();");
ok( $node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = 'cap_reset_a' AND wait_event = 'PgSleep'"),
	'wrapping _reset() stalled in the middle of the clear');
my $rb = bg_psql('cap_reset_b', "SELECT ${P}_reset();");
my $bstate = '';
foreach (1 .. 100)
{
	$bstate = sql("SELECT coalesce(wait_event_type, state) FROM pg_stat_activity WHERE application_name = 'cap_reset_b'");
	last if $bstate eq 'LWLock';
	select(undef, undef, undef, 0.1);
}
is($bstate, 'LWLock', 'a second _reset() waits for the wrapping one');
flood('w', 'x1');
is(distinct('w'), '0 1', 'no value is admitted while the table is being cleared');
open(my $fh, '>', $release) or die "cannot create $release: $!";
close($fh);
$ra->{h}->finish;
$rb->{h}->finish;
is(${ $ra->{err} } . ${ $rb->{err} }, '', 'both _reset() calls succeeded');
flood('w', 'x1', 'x2', 'x1');
is(distinct('w'), '1 1', 'after the stalled wrap: still one string per cap of 1, plus null');
is(sql("SELECT calls FROM ${P}_totals WHERE tags->>'w' = 'x1'"), '2', 'after the stalled wrap: x1 admitted');
is(info('capped_tags'), '1', 'after the stalled wrap: x2 collapsed');

$node->stop;
done_testing();
