# The SQL interface against its documentation (DESIGN.md §7, docs/sql-interface.md;
# backlog 20261005-091225-24), with pg_stat_statements loaded:
#   - the example queries of docs/sql-interface.md and DESIGN.md §7 (the
#     join to pg_stat_statements, the apportioning estimate, statements by
#     bucket, per-application totals, the GRANT examples) are extracted
#     from the documents and run against a known workload, so this test
#     fails if an example stops working or stops giving sensible results;
#   - the column names and types of the views, of the set-returning
#     function and of _info() equal DESIGN.md §7 and the docs' tables;
#   - tags recorded through the hooks in UTF8, LATIN1 and SQL_ASCII
#     databases, read from a UTF8 and from a SQL_ASCII database, including
#     the docs' SQL_ASCII escaping example.
# Visibility rules, showtags, merge_buckets, the REVOKE on _reset() and
# store-level encoding conversion are covered in 015_stats.pl and
# 016_info.pl; only the documented GRANTs are checked here.
use strict;
use warnings;

use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

my $P = 'pg_stat_statement_context';
my $root = dirname(__FILE__) . '/../..';

my $node = PostgreSQL::Test::Cluster->new('sql_surface');
$node->init;
$node->start;
if (!defined pgss_suffix($node))
{
	plan skip_all => 'pg_stat_statements not installed';
}
# Small shared_buffers and a table several times larger, so that every
# scan reads blocks (shared_blks_read > 0 for the apportioning example).
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_stat_statements, $P'
shared_buffers = 1MB
max_connections = 20
max_parallel_workers_per_gather = 0
});
$node->restart;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sql("CREATE EXTENSION pg_stat_statements; CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test");
sql(q{CREATE TABLE t AS SELECT g AS id, repeat('x', 200) AS pad FROM generate_series(1, 30000) g});

# ------------------------------------------------------ the documents
my $docs = slurp_file("$root/docs/sql-interface.md");
my $design = slurp_file("$root/DESIGN.md");
my ($design7) = $design =~ /^## 7\. (.*?)^## 8\. /ms
  or die 'DESIGN.md: section 7 not found';

# The one ```sql block of $text that matches $re.
sub sql_block
{
	my ($text, $re, $what) = @_;
	my @hits = grep { $_ =~ $re } ($text =~ /^```sql\n(.*?)^```$/msg);
	die "$what: expected one sql block matching $re, found " . scalar(@hits)
	  unless @hits == 1;
	return $hits[0];
}
my $join = sql_block($docs, qr/LEFT JOIN pg_stat_statements s USING/, 'docs join');
my $design_join = sql_block($design7, qr/LEFT JOIN pg_stat_statements s USING/, 'DESIGN join');
my $apportion = sql_block($docs, qr/est_shared_blks_read/, 'docs apportioning');
my $by_bucket = sql_block($docs, qr/^SELECT bucket_start, tags->>'controller'/m, 'docs by bucket');
my $toplevel = sql_block($docs, qr/^SELECT tags->>'controller' AS controller, sum\(calls\)/m,
	'docs toplevel totals');
my $grant_stats = sql_block($docs, qr/GRANT pg_read_all_stats/, 'docs GRANT pg_read_all_stats');
my $grant_reset = sql_block($docs, qr/GRANT EXECUTE ON FUNCTION ${P}_reset\(\)/,
	'docs GRANT reset');

# Rows of a query as arrays of fields.
sub rows
{
	my ($q, $db, @params) = @_;
	my $out = $node->safe_psql($db // 'postgres', $q, @params);
	return [ map { [ split /\|/, $_, -1 ] } grep { $_ ne '' } split /\n/, $out ];
}

# Pin the shared debug clock in the middle of bucket $b.
sub pin
{
	sql(qq{SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($_[0])
	         + (interval_us / 2 || ' microseconds')::interval) FROM pssc_store_test_buckets()});
}

# --------------------------------------------------- example queries
# One statement (one queryid in both extensions) run by two contexts: three
# calls tagged users#show in bucket b, one tagged orders#index in b+1.
my $q_text = 'SELECT count(*) FROM t WHERE id < $1';
sql("SELECT pg_stat_statements_reset(); SELECT ${P}_reset()");
my $b = sql('SELECT reader_bucket + 10 FROM pssc_store_test_buckets()');
pin($b);
sql(q{SELECT count(*) FROM t WHERE id < 10 /*controller='users',action='show'*/;} x 3);
pin($b + 1);
sql(q{SELECT count(*) FROM t WHERE id < 100000 /*controller='orders',action='index'*/});

{
	my $r = rows($join);
	is(scalar(@$r), 2, 'docs join: one row per context');
	my %by = map { ("$_->[0]#$_->[1]" => $_) } @$r;
	is(join(' ', map { "$_:$by{$_}[2]" } sort keys %by), 'orders#index:1 users#show:3',
		'docs join: controller, action and calls per context');
	ok($by{'users#show'}[3] > 0 && $by{'orders#index'}[3] > 0, 'docs join: ms > 0');
	is(scalar(grep { index($_->[4], $q_text) == 0 } @$r), 2,
		"docs join: both contexts get pgss's text of the shared statement");
	is($r->[0][4], $r->[1][4], 'docs join: one pgss entry for both contexts');
	is_deeply(rows($design_join), $r, 'DESIGN.md §7 join: same result as the docs');

	# Sensible: the contexts split pgss's totals of the statement exactly.
	is(sql(qq{SELECT sum(c.calls) = s.calls
	                 AND abs(sum(c.total_exec_time) - s.total_exec_time) <= 1e-9 * s.total_exec_time
	            FROM ${P}_totals c JOIN pg_stat_statements s USING (userid, dbid, queryid, toplevel)
	           GROUP BY s.calls, s.total_exec_time}), 't',
		'the contexts sum to pgss calls and total_exec_time');

	# Apportioning: the estimates are non-negative and add up to pgss's
	# shared_blks_read of the statement.
	my $a = rows($apportion);
	is(scalar(@$a), 2, 'docs apportioning: one row per context');
	ok((grep { $_->[2] ne '' && $_->[2] >= 0 } @$a) == 2, 'docs apportioning: estimates >= 0');
	my $blks = sql(qq{SELECT shared_blks_read FROM pg_stat_statements
	                   WHERE query LIKE 'SELECT count(*) FROM t WHERE id < \$1%'});
	ok($blks > 0, "the statement read $blks shared blocks");
	my $sum = 0;
	$sum += $_->[2] for @$a;
	ok(abs($sum - $blks) <= 1e-6 * $blks, "docs apportioning: estimates sum to $blks ($sum)");
	my $expr = 's.shared_blks_read * c.total_exec_time / nullif(s.total_exec_time, 0)';
	(my $flat = $apportion) =~ s/\s+/ /g;
	ok(index($design7, "`$expr`") >= 0 && index($flat, $expr) >= 0,
		'DESIGN.md §7 and the docs use the same estimate');

	# Statements by bucket, per-application totals.
	my $starts = sql(qq{SELECT pssc_store_test_bucket_start($b) || '|' || pssc_store_test_bucket_start($b + 1)});
	my ($s0, $s1) = split /\|/, $starts;
	is(join(' ', map { "$_->[0]/$_->[1]/$_->[2]" } @{ rows($by_bucket) }),
		"$s0/users/3 $s1/orders/1", 'docs by-bucket query: one row per bucket and controller');
	is(join(' ', sort map { "$_->[0]:$_->[1]" } @{ rows($toplevel) }), 'orders:1 users:3',
		'docs toplevel totals');

	# The LEFT JOIN keeps rows pgss no longer has.
	sql('SELECT pg_stat_statements_reset()');
	$r = rows($join);
	is(join(' ', sort map { "$_->[0]:$_->[2]:" . ($_->[4] eq '' ? 'NULL' : 'text') } @$r),
		'orders:1:NULL users:3:NULL', 'docs join after a pgss reset: rows kept, query NULL');
	sql('SELECT pssc_store_test_set_clock_offset(0)');
}

# -------------------------------------------------------- documented GRANTs
{
	sql('CREATE ROLE alice LOGIN; CREATE ROLE monitoring LOGIN');
	$node->safe_psql('postgres', q{SELECT 1 /*controller='secret'*/}, extra_params => [ '-U', 'alice' ]);
	my $mon = sub {
		my ($q) = @_;
		my ($ret, $out, $err) = $node->psql('postgres', $q, extra_params => [ '-U', 'monitoring' ]);
		return $ret == 0 ? $out : "ERROR: $err";
	};
	my $alice_row = qq{SELECT coalesce(tags::text, 'NULL') || ' ' || (queryid IS NULL)
	                     FROM $P WHERE userid = 'alice'::regrole};
	is($mon->($alice_row), 'NULL true', "before the GRANT: alice's tags and queryid hidden");
	sql($grant_stats);
	is($mon->($alice_row), '{"controller": "secret"} false',
		'docs GRANT pg_read_all_stats: monitoring sees other roles\' tags');

	like($mon->("SELECT ${P}_reset()"), qr/permission denied for function ${P}_reset/,
		'before the GRANT: monitoring cannot reset');
	sql($grant_reset);
	is($mon->("SELECT ${P}_reset(); SELECT count(*) FROM $P"), "\n0",
		'docs GRANT EXECUTE: monitoring can reset');
}

# ------------------------------------------------ columns against §7 and docs
{
	my %type = (timestamptz => 'timestamp with time zone', bool => 'boolean',
		float8 => 'double precision', int => 'integer');
	my $norm = sub {
		my ($s) = @_;
		$s =~ s/\s+/ /g;
		$s =~ s/^ | $//g;
		$s =~ s/\b(timestamptz|bool|float8|int)\b/$type{$1}/g;
		return $s;
	};
	my $args = sub {
		return sql("SELECT pg_get_function_arguments('$_[0]'::regprocedure)");
	};

	my ($srf) = $design7 =~ /CREATE FUNCTION $P\(\n(.*?)\)\nRETURNS SETOF record/s
	  or die 'DESIGN.md §7: SRF definition not found';
	is($args->("$P(boolean, boolean)"), $norm->($srf),
		'SRF arguments and OUT columns: names, types and defaults as in DESIGN.md §7');
	my ($info) = $design7 =~ /CREATE FUNCTION ${P}_info\(\n(.*?)\) \.\.\.;/s
	  or die 'DESIGN.md §7: _info() definition not found';
	is($args->("${P}_info()"), $norm->($info), '_info() OUT columns as in DESIGN.md §7');

	my $view_cols = sub {
		return sql(qq{SELECT string_agg('OUT ' || attname || ' ' || format_type(atttypid, atttypmod), ', '
		                                ORDER BY attnum)
		                FROM pg_attribute WHERE attrelid = '$_[0]'::regclass AND attnum > 0});
	};
	(my $srf_out = $norm->($srf)) =~ s/^.*?(?=OUT )//;
	is($view_cols->($P), $srf_out, 'view pg_stat_statement_context: the SRF columns');
	is($view_cols->("${P}_totals"), $srf_out, 'view _totals: the SRF columns');
	is($view_cols->("${P}_last_bucket"), $srf_out, 'view _last_bucket: the SRF columns');
	my ($lb) = $design7 =~ /CREATE FUNCTION ${P}_last_bucket\(\n(.*?)\)\nRETURNS SETOF record/s
	  or die 'DESIGN.md §7: _last_bucket() definition not found';
	is($args->("${P}_last_bucket(boolean)"), $norm->($lb),
		'_last_bucket() arguments and OUT columns as in DESIGN.md §7');
	is($norm->($lb) =~ s/^.*?(?=OUT )//r, $srf_out, '_last_bucket() has the SRF\'s OUT columns');

	# The docs' column tables.
	my $table = sub {
		my ($from, $to) = @_;
		my ($sec) = $docs =~ /\Q$from\E(.*?)\Q$to\E/s or die "docs: section $from not found";
		my @c = $sec =~ /^\| `(\w+)` \| `(\w+)` \|/mg;
		my @out;
		push @out, 'OUT ' . shift(@c) . ' ' . shift(@c) while @c;
		return $norm->(join(', ', @out));
	};
	is($table->('Both have the same columns:', 'An entry is one'), $srf_out,
		'docs: the views\' column table matches the catalog');
	is($table->("## `${P}_info()`", "## `${P}_reset()`"), $args->("${P}_info()"),
		'docs: the _info() column table matches the catalog');
	my ($sig, $cols) = $docs =~ /^$P\((showtags.*?)\)\n\s+RETURNS SETOF \((.*?)\)\n/ms
	  or die 'docs: SRF signature not found';
	(my $names = $srf_out) =~ s/OUT (\w+) [^,]+/$1/g;
	is($norm->($sig) . ' -> ' . $norm->($cols),
		($args->("$P(boolean, boolean)") =~ s/, OUT.*//r) . " -> $names",
		'docs: the SRF signature block matches the catalog');
}

# ------------------------------------------------------- encodings (hooks)
{
	sql("CREATE DATABASE $_->[0] TEMPLATE template0 ENCODING '$_->[1]' LC_COLLATE 'C' LC_CTYPE 'C'")
	  for ([ 'u8', 'UTF8' ], [ 'l1', 'LATIN1' ], [ 'sa', 'SQL_ASCII' ]);
	sql("CREATE EXTENSION $P", $_) for qw(u8 l1 sa);
	sql("SELECT ${P}_reset()");

	# The docs' example: the bytes of "café" written in UTF-8 in a SQL_ASCII
	# database, and how they are shown.
	my ($cafe, $shown, $json) = $docs =~
	  /the bytes `(caf[^`]+)` written in UTF-8 in a\s+`SQL_ASCII` database are shown as `([^`]+)`\..*?appears as `([^`]+)`/s
	  or die 'docs: SQL_ASCII example not found';
	is(unpack('H*', $cafe), '636166c3a9', 'docs example: café in UTF-8 bytes');

	# One tagged statement per database through the hooks, with é written
	# in the database's encoding: LATIN1 e9; SQL_ASCII the docs' UTF-8
	# bytes; UTF8 c3a9.
	sql(q{SELECT format('SELECT 1 /*controller=''caf%s''*/', chr(233)) \gexec}, 'l1');
	sql(qq{SELECT 1 /*controller='$cafe'*/}, 'sa');
	sql(q{SELECT format('SELECT 1 /*controller=''caf%s''*/', chr(233)) \gexec}, 'u8');

	my $hex = sub {
		my ($origin, $reader) = @_;
		return sql(qq{SELECT encode(textsend(tags->>'controller'), 'hex') FROM $P
		               WHERE dbid = (SELECT oid FROM pg_database WHERE datname = '$origin')},
			$reader);
	};
	my $h = sub { unpack('H*', $_[0]) };
	is($hex->('l1', 'u8'), '636166c3a9', 'LATIN1 origin read from UTF8: converted');
	is($hex->('u8', 'l1'), '636166e9', 'UTF8 origin read from LATIN1: converted');
	is($hex->('sa', 'u8'), $h->($shown), "SQL_ASCII origin read from UTF8: escaped as the docs show ($shown)");
	is(sql(qq{SELECT tags->'controller' FROM $P
	           WHERE dbid = (SELECT oid FROM pg_database WHERE datname = 'sa')}, 'u8'),
		$json, "and in jsonb text output as the docs show ($json)");
	is($hex->('sa', 'l1'), $h->($shown), 'SQL_ASCII origin read from LATIN1: escaped the same way');
	is($hex->('sa', 'sa'), $h->($shown), 'SQL_ASCII origin read from SQL_ASCII: escaped the same way');
	is($hex->('u8', 'sa'), '636166c3a9', 'UTF8 origin read from SQL_ASCII: shown unconverted');
	is($hex->('l1', 'sa'), '636166e9', 'LATIN1 origin read from SQL_ASCII: shown unconverted');
	is(sql("SELECT count(*) FROM ${P}_totals", 'sa'), 3, 'all three read from SQL_ASCII');
	is(sql("SELECT invalid_tags FROM ${P}_info()"), 0, 'no tag rejected');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|TRAP|terminated by signal/, 'server log has no crash');

done_testing();
