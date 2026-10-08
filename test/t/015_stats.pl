# The stats SRF pg_stat_statement_context(showtags, merge_buckets) and its
# views pg_stat_statement_context / pg_stat_statement_context_totals
# (DESIGN.md §5.2, §6.11, §7; backlog 20261005-091225-20): the exact §7
# columns and attributes, one row per live slot with bucket_start = epoch +
# bucket_id * interval, merged sums, expired slots hidden without any writes
# to the store, the pg_read_all_stats visibility rule (also with showtags =
# false), and tag output from other databases' encodings: converted, escaped
# for SQL_ASCII, and escaped instead of an error when a tag set cannot be
# converted. Entries are seeded deterministically through the TEST-ONLY
# module test/modules/pssc_store_test and its pinned debug clock.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('stats');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
$P.bucket_count = 4
});
$node->start;

# u8 reads the store (UTF8 server encoding); l1 and sa record tags in their
# own encodings, and l1 also reads.
$node->safe_psql('postgres',
	"CREATE DATABASE $_->[0] TEMPLATE template0 ENCODING '$_->[1]' "
	  . "LC_COLLATE 'C' LC_CTYPE 'C'")
  for ([ 'u8', 'UTF8' ], [ 'l1', 'LATIN1' ], [ 'sa', 'SQL_ASCII' ]);
for my $db (qw(u8 l1 sa))
{
	$node->safe_psql($db, "CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");
}

sub sql { return $node->safe_psql($_[1] // 'u8', $_[0]); }
sub rec { return sql("SELECT pssc_store_test_record($_[0])", $_[1]); }

# Pin the shared debug clock in the middle of bucket $b.
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}

# Rows of $from as "b<bucket id>|queryid|tags|calls|total_exec_time", one per
# line, sorted; the bucket id is found by exact match of bucket_start.
sub rows
{
	my ($from, $db, $where) = @_;
	$where //= 'true';
	return sql(qq{SELECT string_agg(concat_ws('|',
	                 'b' || coalesce((SELECT id::text FROM generate_series(-1000, 1000) id
	                                  WHERE pssc_store_test_bucket_start(id) = r.bucket_start), '?'),
	                 coalesce(r.queryid::text, 'NULL'), coalesce(r.tags::text, 'NULL'),
	                 r.calls, r.total_exec_time), E'\n'
	               ORDER BY r.queryid, r.tags::text, r.bucket_start, r.calls)
	             FROM $from r WHERE $where}, $db);
}

# Every byte of the store's state for all entries (written or not, expired
# or not), to show that reading changes nothing.
sub store_state
{
	return sql(q{SELECT string_agg(format('%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s',
	               dbid, userid, queryid, toplevel, tags, encoding, last_bucket, usage,
	               slot, bucket_id, calls, total_exec_time), ' '
	               ORDER BY queryid, tags::text, slot) FROM pssc_store_test_entries()});
}

# ------------------------------------------------------- catalog: §7 columns
{
	my $cols = 'bucket_start:timestamp with time zone,userid:oid,dbid:oid,'
	  . 'queryid:bigint,toplevel:boolean,tags:jsonb,calls:bigint,'
	  . 'total_exec_time:double precision,calls_total:bigint,'
	  . 'exec_time_total:double precision,stats_since:timestamp with time zone,exemplars:jsonb';
	for my $v ($P, "${P}_totals", "${P}_last_bucket")
	{
		is(sql(qq{SELECT string_agg(attname || ':' || format_type(atttypid, atttypmod), ','
		                            ORDER BY attnum)
		          FROM pg_attribute WHERE attrelid = '$v'::regclass AND attnum > 0
		            AND NOT attisdropped}),
			$cols, "view $v has exactly the §7 columns");
	}
	like(sql(qq{SELECT pg_get_viewdef('$P'::regclass)}),
		qr/\Q$P\E\(true, false\)/, 'view reads the SRF with (true, false)');
	like(sql(qq{SELECT pg_get_viewdef('${P}_totals'::regclass)}),
		qr/\Q$P\E\(true, true\)/, 'totals view reads the SRF with (true, true)');
	is(sql(qq{SELECT pg_get_function_arguments(p.oid) || ' -> ' || pg_get_function_result(p.oid)
	          || ' ' || concat_ws(' ', provolatile, proparallel, proisstrict, proretset)
	          FROM pg_proc p WHERE proname = '$P'}),
		'showtags boolean DEFAULT true, merge_buckets boolean DEFAULT false, '
		  . 'OUT bucket_start timestamp with time zone, OUT userid oid, OUT dbid oid, '
		  . 'OUT queryid bigint, OUT toplevel boolean, OUT tags jsonb, OUT calls bigint, '
		  . 'OUT total_exec_time double precision, OUT calls_total bigint, '
		  . 'OUT exec_time_total double precision, '
		  . 'OUT stats_since timestamp with time zone, OUT exemplars jsonb '
		  . '-> SETOF record v s t t',
		'SRF signature; VOLATILE PARALLEL SAFE STRICT like pg_stat_statements');
}

# ------------------------------------------- rows, bucket_start, merging
my $b0 = 100;
my ($b1, $b3) = ($b0 + 1, $b0 + 3);
my $u = sql('SELECT oid FROM pg_roles WHERE rolname = current_user');
my $d = sql(q{SELECT oid FROM pg_database WHERE datname = 'u8'});
{
	sql('SELECT pssc_store_test_reset()');
	pin($b0);
	rec(q{1, ARRAY['a', '1'], NULL, 1.5}) for 1 .. 2;
	rec(q{1, ARRAY['a', '2'], NULL, 2});
	rec(q{1, ARRAY['a', '1'], NULL, 0.25, false});
	pin($b1);
	rec(q{1, ARRAY['a', '1'], NULL, 3});
	rec(q{2, '{}', NULL, 4});
	pin($b3);
	rec(q{1, ARRAY['a', '1'], NULL, 5}) for 1 .. 3;

	is(rows($P), join("\n",
			"b$b0|1|{\"a\": \"1\"}|1|0.25",
			"b$b0|1|{\"a\": \"1\"}|2|3",
			"b$b1|1|{\"a\": \"1\"}|1|3",
			"b$b3|1|{\"a\": \"1\"}|3|15",
			"b$b0|1|{\"a\": \"2\"}|1|2",
			"b$b1|2|{}|1|4"),
		'view: one row per written slot, bucket_start = epoch + id * interval');
	is(sql(qq{SELECT string_agg(DISTINCT format('%s %s', userid, dbid), ',') FROM $P}),
		"$u $d", 'userid and dbid of the recording session');
	is(sql(qq{SELECT string_agg(format('%s:%s', toplevel, calls), ',' ORDER BY toplevel, calls)
	          FROM $P WHERE bucket_start = pssc_store_test_bucket_start($b0)
	            AND tags = '{"a": "1"}'}),
		'f:1,t:2', 'toplevel is part of the key');

	is(rows("${P}_totals"), join("\n",
			"b$b0|1|{\"a\": \"1\"}|1|0.25",
			"b$b0|1|{\"a\": \"1\"}|6|21",
			"b$b0|1|{\"a\": \"2\"}|1|2",
			"b$b1|2|{}|1|4"),
		'totals: one row per entry, sums of its live slots (2+1+3 calls, '
		  . '3+3+15 ms), bucket_start = oldest live slot');
	is(rows("$P(true, true)"), rows("${P}_totals"), 'SRF(true, true) = totals view');
	is(rows("$P()"), rows($P), 'SRF() defaults to (true, false)');
	is(sql(qq{SELECT count(*) FILTER (WHERE tags IS NULL) || '/' || count(*)
	          FROM $P(false, false)}), '6/6', 'showtags = false: tags NULL in every row');
	is(sql(qq{SELECT count(*) FILTER (WHERE tags IS NULL AND queryid IS NOT NULL) || '/' || count(*)
	          FROM $P(false, true)}), '4/4', 'showtags = false, merged: tags NULL, queryid shown');
	is(sql(qq{SELECT sum(calls) || ' ' || sum(total_exec_time) FROM $P(false, false)}),
		'9 27.25', 'showtags = false: same counters');

	# Expiry: with bucket_count = 4, at b0+4 the live window is [b0+1, b0+4].
	pin($b0 + 4);
	my $before = store_state();
	like($before, qr{/$b0/2/3 }, 'expired slot b0 is still stored before reading');
	is(rows($P), join("\n",
			"b$b1|1|{\"a\": \"1\"}|1|3",
			"b$b3|1|{\"a\": \"1\"}|3|15",
			"b$b1|2|{}|1|4"),
		'view hides expired slots; dead entries yield nothing');
	is(rows("${P}_totals"), join("\n",
			"b$b1|1|{\"a\": \"1\"}|4|18",
			"b$b1|2|{}|1|4"),
		'totals sum only live slots; bucket_start = oldest live slot');
	is(store_state(), $before, 'reading the views wrote nothing to the store');
	sql('SELECT pssc_store_test_check_invariants()');

	pin($b0 + 5);
	is(rows($P), "b$b3|1|{\"a\": \"1\"}|3|15", 'window [b0+2, b0+5]: one live slot left');
	is(rows("${P}_totals"), "b$b3|1|{\"a\": \"1\"}|3|15", 'totals follow');
	pin($b0 + 8);
	is(sql("SELECT count(*) FROM $P") . sql("SELECT count(*) FROM ${P}_totals"), '00',
		'every slot expired: no rows');
	is(sql('SELECT count(DISTINCT (queryid, tags, toplevel)) FROM pssc_store_test_entries()'), 4,
		'the expired entries are still in the store');
}

# ------------------------------------------------------------- visibility
{
	sql('SELECT pssc_store_test_reset()');
	sql(q{CREATE ROLE alice; CREATE ROLE bob;
	      CREATE ROLE stats IN ROLE pg_read_all_stats;
	      CREATE ROLE noinh NOINHERIT IN ROLE pg_read_all_stats});
	my $alice = sql(q{SELECT oid FROM pg_roles WHERE rolname = 'alice'});
	my $bob = sql(q{SELECT oid FROM pg_roles WHERE rolname = 'bob'});
	pin(200);
	rec(qq{11, ARRAY['x', '1'], NULL, 1, true, NULL, NULL, $alice});
	rec(qq{12, ARRAY['y', '2'], NULL, 2, true, NULL, NULL, $bob});

	my $q = sub {
		my ($role, $from) = @_;
		return sql(qq{SET ROLE $role; SELECT string_agg(format('%s:%s:%s:%s', userid::regrole,
		                coalesce(queryid::text, 'NULL'), coalesce(tags::text, 'NULL'), calls),
		                ' ' ORDER BY userid::regrole::text) FROM $from});
	};
	for my $from ($P, "${P}_totals")
	{
		is($q->('alice', $from), 'alice:11:{"x": "1"}:1 bob:NULL:NULL:1',
			"$from: unprivileged role sees its own row, NULL queryid/tags for others");
		is($q->('stats', $from), 'alice:11:{"x": "1"}:1 bob:12:{"y": "2"}:1',
			"$from: pg_read_all_stats member sees everything");
		is($q->('noinh', $from), 'alice:NULL:NULL:1 bob:NULL:NULL:1',
			"$from: NOINHERIT member lacks the privileges of pg_read_all_stats");
	}
	for my $args ('false, false', 'false, true', 'true, true')
	{
		is($q->('alice', "$P($args)"), 'alice:11:NULL:1 bob:NULL:NULL:1',
			"SRF($args) as alice: tags hidden or off, other queryid hidden")
		  if $args =~ /^false/;
		is($q->('bob', "$P($args)"),
			($args =~ /^false/ ? 'alice:NULL:NULL:1 bob:12:NULL:1'
			  : 'alice:NULL:NULL:1 bob:12:{"y": "2"}:1'),
			"SRF($args) as bob: other role's queryid and tags stay NULL");
	}
	is($q->($node->safe_psql('u8', 'SELECT current_user'), $P),
		'alice:11:{"x": "1"}:1 bob:12:{"y": "2"}:1', 'superuser sees everything');
}

# ------------------------------------------------------------ tag encodings
{
	sql('SELECT pssc_store_test_reset()');
	pin(300);
	# LATIN1: chr(233) is the byte 0xe9 (é).
	is(rec(q{21, ARRAY['cl' || chr(233), chr(233) || 't' || chr(233), 'b', 'a\b']}, 'l1'),
		'inserted', 'record LATIN1 tags with non-ASCII bytes');
	# SQL_ASCII: chr(n) is the byte n.
	is(rec(q{22, ARRAY['k' || chr(233), 'a\b' || chr(255)]}, 'sa'),
		'inserted', 'record SQL_ASCII tags with non-ASCII bytes and a backslash');
	# UTF8: chr() is a code point; € (U+20AC) has no LATIN1 equivalent.
	is(rec(q{23, ARRAY['e', chr(233), 'k', 'x' || chr(8364) || '\']}, 'u8'),
		'inserted', 'record UTF8 tags, one not representable in LATIN1');
	is(rec(q{24, ARRAY['e', chr(233)]}, 'u8'), 'inserted', 'record UTF8 tags representable in LATIN1');

	my $tags = sub {
		my ($qid, $db) = @_;
		return sql("SELECT tags FROM $P WHERE queryid = $qid", $db);
	};
	is(sql(qq{SELECT tags = jsonb_build_object('cl' || chr(233), chr(233) || 't' || chr(233),
	                                           'b', 'a\\b')
	          FROM $P WHERE queryid = 21}), 't',
		'LATIN1 tags read from a UTF8 database are converted (and \ is not escaped)');
	is(sql(qq{SELECT octet_length(tags->>('cl' || chr(233))) FROM $P WHERE queryid = 21}),
		5, 'été (3 LATIN1 bytes) became 5 UTF8 bytes');
	is($tags->(22, 'u8'), '{"k\\\\xe9": "a\\\\\\\\b\\\\xff"}',
		'SQL_ASCII tags read from a UTF8 database: bytes >= 0x80 as \xHH, \ as \\');
	is($tags->(22, 'l1'), $tags->(22, 'u8'), 'SQL_ASCII tags are escaped the same way in LATIN1');
	is(sql(qq{SELECT tags = jsonb_build_object('e', chr(233), 'k', 'x' || chr(8364) || '\\')
	          FROM $P WHERE queryid = 23}), 't', 'UTF8 tags read from UTF8: unchanged');

	# Read from the LATIN1 database: 23 cannot be converted, 24 can.
	my ($ret, $out, $err) = $node->psql('l1',
		"SELECT count(*) FROM $P; SELECT count(*) FROM ${P}_totals");
	is("$ret|$out|$err", "0|4\n4|", 'unconvertible tags do not make the SRF fail');
	is($tags->(23, 'l1'), '{"e": "\\\\xc3\\\\xa9", "k": "x\\\\xe2\\\\x82\\\\xac\\\\\\\\"}',
		'unconvertible UTF8 tag set read from LATIN1: the whole set is escaped as for SQL_ASCII');
	is(sql("SELECT tags FROM ${P}_totals WHERE queryid = 23", 'l1'), $tags->(23, 'l1'),
		'same fallback in the totals view');
	is(sql(qq{SELECT tags = jsonb_build_object('e', chr(233)) AND octet_length(tags->>'e') = 1
	          FROM $P WHERE queryid = 24}, 'l1'), 't',
		'convertible UTF8 tag set read from LATIN1 is converted');
	is(sql(qq{SELECT tags = jsonb_build_object('cl' || chr(233), chr(233) || 't' || chr(233),
	                                           'b', 'a\\b')
	          FROM $P WHERE queryid = 21}, 'l1'), 't', 'LATIN1 tags read from LATIN1: unchanged');
}

$node->stop;
done_testing();
