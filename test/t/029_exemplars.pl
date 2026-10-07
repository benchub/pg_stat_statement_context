# Exemplars for excluded high-cardinality keys (DESIGN.md §6.13; backlog
# 20261005-091225-33): pg_stat_statement_context.exemplar_keys (postmaster)
# lists keys whose most recent value is stored per entry, whether or not the
# key is also a grouping tag (it is captured after step 4, before the
# allowlist/denylist of step 5), and shown in the exemplars column of the
# 1.1 views; exemplar_memory (postmaster) caps their total shared memory, and
# a value longer than the per-value room derived from it is dropped (the
# entry keeps its previous exemplar) and counted in
# _info().exemplar_values_dropped.
#
# Covers: the GUCs (defaults: off, nothing allocated; postmaster context;
# validation); distinct values of a denylisted exemplar key make no new
# entries, and the column shows the latest value; keys not listed never
# show; a key both grouped and listed; the per-value room and total memory
# (_info().exemplar_value_bytes, exemplar_shmem_bytes); an overlong value is
# dropped and counted; tags_override beats a comment, as for tags; utility
# statements; visibility (NULL for other roles' rows without
# pg_read_all_stats, and with showtags = false); _reset(); exemplars are not
# saved across a restart; CREATE EXTENSION VERSION '1.0' then ALTER
# EXTENSION UPDATE TO '1.1'.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('exemplars');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = 'traceparent, tracestate, request_id'
$P.max_entries = 100
EOC
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }
sub info { return sql("SELECT $_[0] FROM ${P}_info()"); }

sql("CREATE EXTENSION $P; CREATE TABLE t(i int)");
is(sql("SELECT extversion FROM pg_extension WHERE extname = '$P'"), '1.1',
	'CREATE EXTENSION installs 1.1');

# ---------------------------------------------------------------------------
# GUCs: off by default
# ---------------------------------------------------------------------------
is( sql("SELECT string_agg(name || '=' || boot_val || '/' || coalesce(unit, '') || '/' || context,"
		  . " ' ' ORDER BY name COLLATE \"C\") FROM pg_settings WHERE name LIKE '$P.exemplar%'"),
	"$P.exemplar_keys=//postmaster $P.exemplar_memory=2048/kB/postmaster",
	'GUCs: names, defaults and contexts');
is(info('exemplar_shmem_bytes || \'/\' || exemplar_value_bytes || \'/\' || exemplar_values_dropped'),
	'0/0/0', 'off by default: no exemplar memory');
sql("SELECT ${P}_reset()");
sql("SELECT * FROM t /*controller='c0',traceparent='tp-0'*/");
is(sql("SELECT exemplars FROM ${P}_totals WHERE tags->>'controller' = 'c0'"), '{}',
	'off: no exemplar is stored');

foreach my $bad ('*', 'a, *', join(',', map { "k$_" } 1 .. 9), 'a b', ('k' x 64))
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.exemplar_keys = '$bad'");
	ok($ret != 0 && $err =~ /invalid value for parameter/, "exemplar_keys rejected: '$bad'")
	  or diag($err);
}
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.exemplar_memory = -1");
	ok($ret != 0, 'exemplar_memory: negative rejected');
}

# ---------------------------------------------------------------------------
# Configured: traceparent (denylisted) and sid (a grouping tag too)
# ---------------------------------------------------------------------------
# 16 kB over 100 entries: 163 bytes, 160 aligned, 80 per key, 78 per value.
sql("ALTER SYSTEM SET $P.exemplar_keys = ' traceparent , sid, traceparent'");
sql("ALTER SYSTEM SET $P.exemplar_memory = '16kB'");
$node->restart;
is(sql("SHOW $P.exemplar_keys"), ' traceparent , sid, traceparent', 'exemplar_keys set');
is(info('exemplar_value_bytes'), 78, 'per-value room: 16 kB / 100 entries / 2 keys - 2');
is(info('exemplar_shmem_bytes'), 16000, 'total exemplar memory: 100 entries x 160 bytes');
cmp_ok(info('exemplar_shmem_bytes'), '<=', 16 * 1024, 'within exemplar_memory');

sub ex
{
	my ($ctl, $from) = @_;
	$from //= "${P}_totals";
	return sql("SELECT count(*) || ':' || coalesce(string_agg(coalesce(exemplars::text, 'NULL'), ','), '')"
		  . " FROM $from WHERE tags->>'controller' = '$ctl'");
}

sql("SELECT ${P}_reset()");
my $tp1 = '00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01';
my $tp2 = '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01';
my $tp3 = '00-11111111111111111111111111111111-2222222222222222-01';
sql("SELECT * FROM t /*controller='a',traceparent='$tp1',request_id='r1'*/");
is(ex('a'), qq{1:{"traceparent": "$tp1"}}, 'first value stored');
sql("SELECT * FROM t /*controller='a',traceparent='$tp2',request_id='r2'*/");
sql("SELECT * FROM t /*controller='a',traceparent='$tp3',request_id='r3'*/");
is(ex('a'), qq{1:{"traceparent": "$tp3"}},
	'distinct traceparents: one entry, the latest value shown');
is(sql("SELECT calls FROM ${P}_totals WHERE tags->>'controller' = 'a'"), 3,
	'all three calls on that entry');
is(sql("SELECT tags::text FROM ${P}_totals WHERE tags->>'controller' = 'a'"),
	'{"controller": "a"}', 'traceparent and request_id are not grouping tags');
is(sql("SELECT count(*) FROM ${P}_totals WHERE exemplars ? 'request_id'"), 0,
	'request_id (denylisted, not listed) is never an exemplar');

sql("SELECT * FROM t /*controller='b',sid='s1'*/");
is(sql("SELECT tags::text || ' ' || exemplars::text FROM ${P}_totals WHERE tags->>'controller' = 'b'"),
	q{{"sid": "s1", "controller": "b"} {"sid": "s1"}}, 'a grouped key is an exemplar too');
sql("SELECT * FROM t /*controller='b',sid='s1',traceparent='$tp1'*/");
sql("SELECT * FROM t /*controller='b',sid='s1'*/");
is(sql("SELECT tags::text || ' ' || exemplars::text FROM ${P}_totals WHERE tags->>'controller' = 'b'"),
	qq{{"sid": "s1", "controller": "b"} {"sid": "s1", "traceparent": "$tp1"}},
	'slots fill independently; a statement without the key keeps its last value');
is(ex('a', $P), qq{1:{"traceparent": "$tp3"}}, 'the bucket view shows it too');

# Overlong: dropped, counted, the previous value kept.
my $long = 'x' x 79;
is(info('exemplar_values_dropped'), 0, 'nothing dropped yet');
sql("SELECT * FROM t /*controller='a',traceparent='$long'*/");
is(ex('a'), qq{1:{"traceparent": "$tp3"}}, 'a value over the room is dropped');
is(info('exemplar_values_dropped'), 1, 'and counted in exemplar_values_dropped');
my $fit = 'y' x 78;
sql("SELECT * FROM t /*controller='a',traceparent='$fit'*/");
is(ex('a'), qq{1:{"traceparent": "$fit"}}, 'a value of exactly the room is stored');
is(info('exemplar_values_dropped'), 1, 'not counted');

# tags_override beats the comment, like for tags.
sql("SET $P.tags_override = " . q{$$traceparent='ov-tp'$$;}
	  . " SELECT * FROM t /*controller='a',traceparent='$tp1'*/");
is(ex('a'), q{1:{"traceparent": "ov-tp"}}, 'tags_override wins over the comment');

# A utility statement.
sql("ANALYZE t /*controller='u',traceparent='$tp2'*/");
is(ex('u'), qq{1:{"traceparent": "$tp2"}}, 'utility statements store exemplars');

# Nested statements inherit the outer statement's exemplars (nested_tags =
# inherit, the default), like its tags.
sql(q{CREATE FUNCTION f_nest() RETURNS bigint LANGUAGE plpgsql AS $$
      BEGIN RETURN (SELECT count(*) FROM t); END $$});
sql("SET $P.track = 'all'; SELECT f_nest() /*controller='n',traceparent='$tp1'*/");
is(sql("SELECT string_agg(toplevel || '=' || exemplars::text, ' ' ORDER BY toplevel)"
	  . " FROM ${P}_totals WHERE tags->>'controller' = 'n'"),
	qq{false={"traceparent": "$tp1"} true={"traceparent": "$tp1"}},
	'nested statements inherit the exemplars');

# ---------------------------------------------------------------------------
# Visibility (§6.11)
# ---------------------------------------------------------------------------
sql(q{CREATE ROLE alice; CREATE ROLE bob; CREATE ROLE stats IN ROLE pg_read_all_stats;
      GRANT SELECT ON t TO alice, bob, stats});
sql("SET ROLE alice; SELECT * FROM t /*controller='v',traceparent='tp-alice'*/");
sql("SET ROLE bob; SELECT * FROM t /*controller='v',traceparent='tp-bob'*/");
my $vis = sub {
	my ($role, $from) = @_;
	return sql(qq{SET ROLE $role; SELECT string_agg(userid::regrole || ':'
	                || coalesce(exemplars::text, 'NULL'), ' ' ORDER BY userid::regrole::text)
	              FROM $from WHERE userid IN ('alice'::regrole, 'bob'::regrole)});
};
is($vis->('alice', "${P}_totals"),
	'alice:{"traceparent": "tp-alice"} bob:NULL',
	'unprivileged: own exemplars shown, other roles\' NULL');
is($vis->('alice', $P), 'alice:{"traceparent": "tp-alice"} bob:NULL', 'same in the bucket view');
is($vis->('stats', "${P}_totals"),
	'alice:{"traceparent": "tp-alice"} bob:{"traceparent": "tp-bob"}',
	'pg_read_all_stats member sees every exemplar');
is($vis->('alice', "$P(false, true)"), 'alice:NULL bob:NULL',
	'showtags = false: exemplars NULL in every row');

# ---------------------------------------------------------------------------
# Not saved; _reset()
# ---------------------------------------------------------------------------
$node->restart;
is(ex('a'), '1:{}', 'restart: the entry is restored without its exemplar');
sql("SELECT * FROM t /*controller='a',traceparent='$tp2'*/");
is(ex('a'), qq{1:{"traceparent": "$tp2"}}, 'and stores one again');
my $d0 = info('exemplar_values_dropped');
sql("SELECT * FROM t /*controller='a',traceparent='$long'*/");
is(info('exemplar_values_dropped'), $d0 + 1, 'dropped values are counted after a restart');
sql("SELECT ${P}_reset()");
is(info('exemplar_values_dropped'), 0, '_reset() zeroes exemplar_values_dropped');
is(ex('a'), '0:', '_reset() removes the entries');

# ---------------------------------------------------------------------------
# No room for any value (exemplar_memory = 0 with keys set): the entries get
# no slots, and every value is dropped and counted.
# ---------------------------------------------------------------------------
sql("ALTER SYSTEM SET $P.exemplar_memory = 0");
$node->restart;
is(info('exemplar_value_bytes || \'/\' || exemplar_shmem_bytes'), '0/0',
	'exemplar_memory = 0: no room, no memory');
sql("SELECT * FROM t /*controller='z',traceparent='$tp1'*/");
is(ex('z'), '1:{}', 'nothing stored');
is(info('exemplar_values_dropped'), 1, 'the value is dropped and counted');
sql("SELECT * FROM t /*controller='z',traceparent=''*/");
is(ex('z'), '1:{}', 'an empty value is not stored either');
# A tags_override result cached by _extract() (which captures nothing) must
# not be reused by recording, which captures and here drops.
sql(qq{SET $P.enabled = off;
       SET $P.tags_override = \$\$controller='z',traceparent='ov'\$\$;
       SELECT ${P}_extract('SELECT 1') IS NOT NULL;
       SET $P.enabled = on;
       SELECT * FROM t;});
is(ex('z'), '1:{}', 'override: nothing stored');
is(info('exemplar_values_dropped'), 2,
	'override value dropped and counted after _extract() cached the override');
sql("ALTER SYSTEM SET $P.exemplar_memory = '16kB'");
$node->restart;

# ---------------------------------------------------------------------------
# Upgrade 1.0 -> 1.1
# ---------------------------------------------------------------------------
sql('CREATE DATABASE up');
sql("CREATE EXTENSION $P VERSION '1.0'", 'up');
my $cols = sub {
	return sql(qq{SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
	              WHERE attrelid = '$_[0]'::regclass AND attnum > 0}, 'up');
};
my $v10 = 'bucket_start,userid,dbid,queryid,toplevel,tags,calls,total_exec_time,'
  . 'calls_total,exec_time_total,stats_since';
is($cols->($P), $v10, '1.0: the view has no exemplars column');
sql("CREATE TABLE t(i int); SELECT * FROM t /*controller='up',traceparent='tp-up'*/", 'up');
is(sql("SELECT count(*) FROM $P WHERE tags->>'controller' = 'up'", 'up'), 1,
	'1.0 functions still work with the 1.1 library');
is(sql("SELECT count(*) FROM ${P}_info()", 'up'), 1, '1.0 _info() still works');
sql("ALTER EXTENSION $P UPDATE TO '1.1'", 'up');
is(sql("SELECT extversion FROM pg_extension WHERE extname = '$P'", 'up'), '1.1', 'updated to 1.1');
for my $v ($P, "${P}_totals", "${P}_last_bucket")
{
	is($cols->($v), "$v10,exemplars", "1.1: $v has the exemplars column");
}
is(sql("SELECT exemplars FROM ${P}_totals WHERE tags->>'controller' = 'up'", 'up'),
	'{"traceparent": "tp-up"}', 'after the update the exemplar is readable');
is(sql("SELECT exemplar_shmem_bytes FROM ${P}_info()", 'up'), 16000, '1.1 _info() columns');
is(sql(q{SELECT has_table_privilege('alice', 'pg_stat_statement_context', 'SELECT')
                AND has_table_privilege('alice', 'pg_stat_statement_context_totals', 'SELECT')
                AND has_table_privilege('alice', 'pg_stat_statement_context_last_bucket', 'SELECT')}, 'up'),
	't', 'the recreated views are readable by PUBLIC');
is(sql(qq{SELECT count(*) FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid
          WHERE e.extname = '$P' AND d.deptype = 'e'
            AND d.classid = 'pg_proc'::regclass}, 'up'),
	sql(qq{SELECT count(*) FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid
           WHERE e.extname = '$P' AND d.deptype = 'e'
             AND d.classid = 'pg_proc'::regclass}),
	'an updated install has the same functions as a fresh 1.1 one');
sql("DROP EXTENSION $P", 'up');

unlike(slurp_file($node->logfile), qr/PANIC|TRAP|terminated by signal/, 'no crash');
done_testing();
