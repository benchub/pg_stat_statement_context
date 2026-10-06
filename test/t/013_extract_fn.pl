# pg_stat_statement_context_extract() leaves the shared store alone (DESIGN.md
# §7, §9; backlog 20261005-091225-11): the debug function reports its own
# per-call diagnostics, but records nothing and does not add its
# invalid/dropped/heuristic counts to the backend-local counters that the
# executor hooks flush into the shared _info() header. The positive control
# runs the same statement for real and sees the store change. A regex
# compile failure is per-backend state (the extractor stays disabled for the
# backend until the next config change, and the hooks never see a second
# failure to count), so it is reported by the call AND counted in the shared
# counter. Shared counters are read through the TEST-ONLY module
# test/modules/pssc_store_test; the regex compile failure is injected with
# test/modules/pssc_extract_test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('extract_fn');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $pkglibdir = sql(q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
my $have_pgss = -e "$pkglibdir/pg_stat_statements.so";
note("pg_stat_statements available: " . ($have_pgss ? 'yes' : 'no'));
if ($have_pgss)
{
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'pg_stat_statements, $P'\n");
	$node->restart;
	sql('CREATE EXTENSION pg_stat_statements');
}

sql("CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test; "
	  . 'CREATE EXTENSION pssc_extract_test');

# Sets one sighup GUC (value given as an SQL literal) and waits until new
# sessions see it (expected: the SHOW output).
sub set_conf
{
	my ($name, $literal, $expected) = @_;
	sql("ALTER SYSTEM SET $P.$name = $literal");
	sql('SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', "SHOW $P.$name", $expected)
	  or die "$P.$name did not become $expected";
}

set_conf('tags', q{'*'}, '*');
set_conf('exclude_tags', q{''}, '');

# A statement longer than scan_window (append: heuristic tail path) whose
# trailing comment has one invalid tag (%00) and eight 64-byte values, one
# of which does not fit max_tagset_bytes = 512.
my $gen = q{'SELECT length(''' || repeat('x', 3000) || ''') /*'
  || (SELECT string_agg('k' || i || '=''' || repeat('v', 64) || '''', ',')
        FROM generate_series(1, 8) i)
  || ',bad=''%00''*/'};

my $counters = 'SELECT entries, invalid_tags, dropped_tags, heuristic_scans, '
  . 'regex_compile_failures FROM pssc_store_test_counters()';

sql('SELECT pssc_store_test_reset()');
is(sql($counters), '0|0|0|0|0', 'store empty after reset');

my $r = sql(
	"SELECT r->>'heuristic', r->>'heuristic_scans', r->>'invalid_tags', "
	  . "r->>'dropped_tags', r->>'ntags' "
	  . "FROM $P\_extract($gen) r");
is($r, 'true|1|1|1|7', 'debug call reports heuristic, invalid and dropped tags');
is(sql($counters), '0|0|0|0|0',
	'debug call recorded nothing and left the shared counters alone');

# The same backend runs later statements: nothing was left pending in it.
is( sql(
		"SELECT ($P\_extract($gen))->>'ntags'; SELECT 1; $counters"),
	"7\n1\n0|0|0|0|0",
	'nothing pending in the backend after debug calls');

# Positive control: the same statement executed for real.
sql("SELECT $gen \\gexec");
is(sql($counters), '1|1|1|1|0',
	'executing the statement records it and counts its diagnostics');
is( sql(
		'SELECT tags_len > 0, array_length(tags, 1) FROM pssc_store_test_entries()'),
	't|14', 'recorded with the seven tags the debug call reported');

# Regex compile failure: reported by the call and counted (shared).
sql('SELECT pssc_store_test_reset()');
set_conf('extractors', q{'regex(pattern=''svc=(\w+)'', keys=service)'},
	q{regex(pattern='svc=(\w+)', keys=service)});
is( sql(
		"SELECT pssc_extract_test_regex_inject('compile', 0, 'oom', -1); "
		  . "SELECT r->>'regex_compile_failures', r->'tags' "
		  . "FROM $P\_extract('SELECT 1 /* svc=s */') r; "
		  . "SELECT r->>'regex_compile_failures', r->'tags' "
		  . "FROM $P\_extract('SELECT 1 /* svc=s */') r; "
		  . "SELECT pssc_extract_test_regex_inject('compile', -1, 'none')"),
	"\n1|{}\n0|{}\n",
	'regex compile failure reported once by the call that hit it');
is(sql($counters), '0|0|0|0|1',
	'regex compile failure counted in the shared counter (backend state)');
sql('SELECT pssc_store_test_reset()');
is(sql("SELECT ($P\_extract('SELECT 1 /* svc=s */'))->'tags'"),
	'{"service": "s"}', 'a new backend compiles the regex');
is(sql($counters), '0|0|0|0|0', 'and a successful call counts nothing');

# SQL_ASCII (DESIGN.md §6.11): tags are stored as the bytes the client sent
# (only NUL is invalid there), but non-ASCII bytes are escaped on output as
# \xHH, and '\' as '\\' so that the escaping is unambiguous; keys too. The
# diagnostics describe the stored bytes.
set_conf('extractors', q{'sqlcommenter, marginalia'}, 'sqlcommenter, marginalia');
sql(q{CREATE DATABASE sqlascii TEMPLATE template0 ENCODING 'SQL_ASCII' }
	  . q{LC_COLLATE 'C' LC_CTYPE 'C'});
$node->safe_psql('sqlascii', "CREATE EXTENSION $P");
my $ascii = $node->safe_psql('sqlascii',
	"SELECT $P\_extract(\$q\$SELECT 1 /*k%E9y='%FF',back='a%5Cb',ok='v'*/\$q\$)");
like($ascii, qr/\A[\x00-\x7f]*\z/, 'SQL_ASCII output is pure ASCII');
is( $node->safe_psql('sqlascii',
		"SELECT string_agg(key || '=' || value, ',' ORDER BY key), "
		  . "r->>'ntags', r->>'tagset_bytes', r->>'invalid_tags' "
		  . "FROM $P\_extract(\$q\$SELECT 1 /*k%E9y='%FF',back='a%5Cb',ok='v'*/\$q\$) r, "
		  . "jsonb_each_text(r->'tags') GROUP BY r"),
	'back=a\\\\b,k\\xe9y=\\xff,ok=v|3|20|0',
	'SQL_ASCII escapes non-ASCII bytes and backslashes of keys and values');
is( $node->safe_psql('sqlascii',
		"SELECT ($P\_extract(E'SELECT 1 /*c\\xc3\\xa9:\\xc3\\xa9*/'))->'tags'"),
	'{"c\\\\xc3\\\\xa9": "\\\\xc3\\\\xa9"}',
	'SQL_ASCII escapes raw (marginalia) bytes, even valid UTF8 sequences');
is(sql("SELECT ($P\_extract(\$q\$SELECT 1 /*back='a%5Cb',k='%C3%A9'*/\$q\$))->'tags'"),
	qq{{"k": "\xc3\xa9", "back": "a\\\\b"}},
	'a UTF8 database outputs the bytes unescaped');

# Parity with the executor hooks (DESIGN.md §6.5): for the first statement
# of a query string, the debug call given the parser's stmt_location finds
# the same owned start as the hooks, including the scan_window budget for
# leading trivia (PG18 reports stmt_location at the first token, PG14-17 at
# the string start). The parser's location is taken from pg_stat_statements'
# text (the statement has no constants to normalize).
SKIP:
{
	skip 'pg_stat_statements not installed', 3 unless $have_pgss;

	set_conf('extractors', q{'marginalia(position=prepend)'},
		'marginalia(position=prepend)');
	for my $pad (100, 3000)
	{
		my $q = qq{'/*controller:head*/' || repeat(' ', $pad) || 'SELECT current_user'};
		sql('SELECT pssc_store_test_reset(); SELECT pg_stat_statements_reset()');
		sql("SELECT $q \\gexec");
		my $recorded = sql(
			q{SELECT coalesce(string_agg(array_to_string(tags, '='), ','), '') }
			  . 'FROM pssc_store_test_entries()');
		my $loc = sql("SELECT strpos($q, query) - 1 FROM pg_stat_statements "
			  . q{WHERE query LIKE '%current_user'});
		my $debug = sql(
			q{SELECT coalesce(string_agg(key || '=' || value, ','), '') }
			  . "FROM jsonb_each_text(($P\_extract($q, $loc))->'tags')");
		is($debug, $recorded,
			"debug call equals the hooks (leading trivia $pad, stmt_location $loc)");
		is($recorded, 'controller=head', 'recorded within scan_window')
		  if $pad == 100;
	}
}
# Lexing back to find the owned start is bounded by scan_window, as in the
# hooks: from the first token, 3019 bytes in, the comment is not owned.
is( sql(
		"SELECT ($P\_extract('/*controller:head*/' || repeat(' ', 3000) "
		  . "|| 'SELECT 1', 3019))->'tags'"),
	'{}', 'owned-start lexing is bounded by scan_window');
is( sql(
		"SELECT r->'tags', r->'stmt_start' FROM $P\_extract('/*controller:head*/' "
		  . "|| repeat(' ', 2000) || 'SELECT 1', 2019) r"),
	'{"controller": "head"}|0', 'within scan_window the leading comment is owned');

$node->stop;
done_testing();
