# Extractor DSL (DESIGN.md §4.2, backlog 20261005-091225-8): every §4.2
# example parses, omitted parameters get their per-extractor defaults, each
# class of malformed input is rejected with a specific GUC detail, a bad
# reload keeps the previous parsed config, regexes are validated with the
# core engine (length, back-references, capture count vs max_tags and keys),
# and the parsed blob is pointer-free and freed when replaced. The parsed
# state is read through the TEST-ONLY module test/modules/pssc_guc_test
# (make install-test-modules), which renders it canonically from a relocated
# copy of the blob.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('extractors');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pssc_guc_test');
$node->safe_psql('postgres', 'CREATE EXTENSION pssc_extract_test');

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub show { return sql("SHOW $P.extractors"); }
sub parsed { return sql('SELECT pssc_guc_test_extractors(true)'); }
sub gen { return sql('SELECT pssc_guc_test_generation()'); }

# SQL string literal (standard_conforming_strings: only quotes are doubled).
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

# postgresql.conf string literal: the config-file lexer also processes
# backslash escapes, so backslashes are doubled too.
sub confq { my ($s) = @_; $s =~ s/\\/\\\\/g; $s =~ s/'/''/g; return "'$s'"; }

# A persistent psql session (one backend for its whole life); see 003_guc.pl.
my @sessions;
my $session_marker = 0;
sub session_open
{
	my %s = (in => '', out => '', err => '');
	$s{h} = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d', $node->connstr('postgres') ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err},
		IPC::Run::timeout(300));
	push @sessions, \%s;
	return \%s;
}
sub sq
{
	my ($s, $q) = @_;
	my $m = '__pssc_done_' . ++$session_marker . '__';
	$s->{out} = '';
	$s->{err} = '';
	$s->{in} .= "$q;\n\\echo $m\n";
	$s->{h}->pump until $s->{out} =~ /^\Q$m\E$/m;
	die "session error for <$q>: $s->{err}" if $s->{err} ne '';
	(my $r = $s->{out}) =~ s/^\Q$m\E\n\z//m;
	chomp $r;
	return $r;
}
sub session_close
{
	my ($s) = @_;
	$s->{in} .= "\\q\n";
	$s->{h}->finish;
	@sessions = grep { $_ != $s } @sessions;
}

# ALTER SYSTEM statements, then reload and wait until it has been processed
# (scan_window is moved to a new sentinel value and polled for). Statements
# in $alter_prelude run first in the same backend.
my $sentinel = 1000;
my $alter_prelude = '';
sub alter_and_reload
{
	$sentinel++;
	sql($alter_prelude . join('', map { "ALTER SYSTEM $_;\n" } @_)
		  . "ALTER SYSTEM SET $P.scan_window = $sentinel;");
	$node->reload;
	$node->poll_query_until('postgres',
		"SELECT setting::int = $sentinel FROM pg_settings WHERE name = '$P.scan_window'")
	  or die "reload not processed (sentinel $sentinel)";
	for my $s (@sessions)
	{
		my $tries = 0;
		until (sq($s, "SELECT setting FROM pg_settings WHERE name = '$P.scan_window'") eq $sentinel)
		{
			die "session did not process reload (sentinel $sentinel)" if ++$tries > 1800;
			usleep(100_000);
		}
	}
}
sub set_ext { alter_and_reload("SET $P.extractors = " . sqlq($_[0])); }

# ALTER SYSTEM SET extractors (validated by the check_hook): stderr, '' if OK.
sub ext_err
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.extractors = " . sqlq($_[0]));
	return $ret == 0 ? '' : $err;
}

my $SQLC_DEF = "sqlcommenter(position=append, merge=off, url_decode=on)";
my $MARG_DEF = "marginalia(position=append, merge=off, kv_sep=':', pair_sep=',')";

# ---------------------------------------------------------------------------
# Default and §4.2 examples
# ---------------------------------------------------------------------------
is(show(), 'sqlcommenter, marginalia', 'default extractors value');
is(parsed(), "$SQLC_DEF\n$MARG_DEF",
	"default 'sqlcommenter, marginalia': both parsed, position append");
cmp_ok(sql('SELECT pssc_guc_test_extractors_size()'), '>', 0, 'blob size reported');

my @examples = (
	# Rails app
	[ 'marginalia(position=append)', $MARG_DEF ],
	# Polyglot shop
	[ 'sqlcommenter(position=append, rename=route:endpoint), marginalia(position=prepend, rename=controller:endpoint)',
	  "sqlcommenter(position=append, merge=off, url_decode=on, rename=route:endpoint)\n"
	  . "marginalia(position=prepend, merge=off, kv_sep=':', pair_sep=',', rename=controller:endpoint)" ],
	# Custom house format, as stored in the GUC (SQL/ALTER SYSTEM form)
	[ q{regex(pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)},
	  q{regex(position=any, merge=off, pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)} ],
	# The regex parameter illustration, with a real two-group pattern for '...'
	[ q{regex(pattern='(\w+)/(\w+)', keys='k1|k2', position=any)},
	  q{regex(position=any, merge=off, pattern='(\w+)/(\w+)', keys=k1|k2)} ],
	# sqlcommenter / marginalia parameter lists
	[ 'sqlcommenter(url_decode=off)',
	  'sqlcommenter(position=append, merge=off, url_decode=off)' ],
	[ q{marginalia(kv_sep=':', pair_sep=',')}, $MARG_DEF ],
	[ 'sqlcommenter, marginalia', "$SQLC_DEF\n$MARG_DEF" ]);
for my $ex (@examples)
{
	set_ext($ex->[0]);
	is(show(), $ex->[0], "example accepted: $ex->[0]");
	is(parsed(), $ex->[1], "example parsed: $ex->[0]");
}

# ---------------------------------------------------------------------------
# Per-extractor defaults, every parameter, quoting and whitespace
# ---------------------------------------------------------------------------
my @ok = (
	[ 'sqlcommenter', $SQLC_DEF, 'sqlcommenter alone: append' ],
	[ 'marginalia', $MARG_DEF, 'marginalia alone: append' ],
	[ q{regex(pattern='(a)', keys=k)},
	  q{regex(position=any, merge=off, pattern='(a)', keys=k)}, 'regex: any' ],
	[ q{sqlcommenter(position=prepend, keys='a | b|c', rename='a:x| b : y ', merge=on, url_decode=off)},
	  'sqlcommenter(position=prepend, merge=on, url_decode=off, keys=a|b|c, rename=a:x|b:y)',
	  'sqlcommenter: every parameter' ],
	[ q{marginalia(kv_sep='=', pair_sep=' ', position=any, keys=application, merge=true)},
	  q{marginalia(position=any, merge=on, kv_sep='=', pair_sep=' ', keys=application)},
	  'marginalia: every parameter, whitespace pair_sep' ],
	[ q{regex(pattern='op=(\w+)', keys=operation, position=append, rename=operation:op, merge=off)},
	  q{regex(position=append, merge=off, pattern='op=(\w+)', keys=operation, rename=operation:op)},
	  'regex: every parameter' ],
	[ q{  marginalia ( kv_sep = '=>' , pair_sep = ';' )  ,sqlcommenter  },
	  "marginalia(position=append, merge=off, kv_sep='=>', pair_sep=';')\n$SQLC_DEF",
	  'whitespace around tokens' ],
	[ q{SQLCommenter(Position=PREPEND, Merge=ON), MARGINALIA},
	  "sqlcommenter(position=prepend, merge=on, url_decode=on)\n$MARG_DEF",
	  'names and keywords are case-insensitive' ],
	[ q{regex(pattern='it''s (\w+)', keys=k)},
	  q{regex(position=any, merge=off, pattern='it''s (\w+)', keys=k)},
	  "quoted value with '' escape" ],
	[ q{regex(pattern='(?:a|b)(c)', keys=k)},
	  q{regex(position=any, merge=off, pattern='(?:a|b)(c)', keys=k)},
	  'non-capturing groups are not counted' ],
	[ q{marginalia(kv_sep=':', pair_sep='''')},
	  q{marginalia(position=append, merge=off, kv_sep=':', pair_sep='''')},
	  'a separator that is a quote' ],
	# kv_sep may start with (or be) whitespace; the pair parser supports it
	[ q{marginalia(kv_sep=' :')},
	  q{marginalia(position=append, merge=off, kv_sep=' :', pair_sep=',')},
	  'kv_sep starting with a space' ],
	[ "marginalia(kv_sep='\t =', pair_sep=';')",
	  "marginalia(position=append, merge=off, kv_sep='\t =', pair_sep=';')",
	  'kv_sep starting with a tab' ],
	[ q{marginalia(kv_sep='  ')},
	  q{marginalia(position=append, merge=off, kv_sep='  ', pair_sep=',')},
	  'whitespace-only kv_sep' ],
	[ q{sqlcommenter(keys='Key:1|k.2')},
	  'sqlcommenter(position=append, merge=off, url_decode=on, keys=Key:1|k.2)',
	  'keys are case-sensitive and kept as written' ],
	[ q{regex(pattern='a=(\w+)|b=(\w+)', keys=k|k)},
	  q{regex(position=any, merge=off, pattern='a=(\w+)|b=(\w+)', keys=k|k)},
	  'regex: several groups may share a key' ],
	[ 'sqlcommenter, sqlcommenter(position=prepend, merge=on)',
	  "$SQLC_DEF\nsqlcommenter(position=prepend, merge=on, url_decode=on)",
	  'the same extractor twice' ],
	# appname(format=...) (item 20261005-091225-38): the format's defaults, no position
	[ 'appname(format=sqlcommenter)', 'appname(format=sqlcommenter, merge=off, url_decode=on)',
	  'appname sqlcommenter: defaults' ],
	[ 'appname(format=marginalia)', q{appname(format=marginalia, merge=off, kv_sep=':', pair_sep=',')},
	  'appname marginalia: defaults' ],
	[ q{appname(format=regex, pattern='^(\w+)/(\w+)$', keys=app|ver)},
	  q{appname(format=regex, merge=off, pattern='^(\w+)/(\w+)$', keys=app|ver)},
	  'appname regex' ],
	[ q{appname(kv_sep='=', merge=on, pair_sep=';', format=marginalia, keys=a|b, rename=a:c)},
	  q{appname(format=marginalia, merge=on, kv_sep='=', pair_sep=';', keys=a|b, rename=a:c)},
	  'appname: every parameter, format last' ],
	[ q{AppName(url_decode=off, Format=SQLCOMMENTER)},
	  'appname(format=sqlcommenter, merge=off, url_decode=off)',
	  'appname: case-insensitive, format value too' ],
	[ 'sqlcommenter, appname(format=marginalia), marginalia',
	  "$SQLC_DEF
appname(format=marginalia, merge=off, kv_sep=':', pair_sep=',')
$MARG_DEF",
	  'appname between comment extractors' ],
	[ '', '', 'empty value: no extractors' ],
	[ "  \t ", '', 'blank value: no extractors' ]);
for my $c (@ok)
{
	set_ext($c->[0]);
	is(parsed(), $c->[1], "parsed: $c->[2]");
	is(sql('SELECT pssc_guc_test_extractors(false)'), $c->[1],
		"parsed in place: $c->[2]");
}

# Maximum lengths: a 1024-byte pattern, 16 extractors, 63-byte keys.
my $pat1024 = '(a)' . ('x' x 1021);
set_ext("regex(pattern='$pat1024', keys=k)");
is(parsed(), "regex(position=any, merge=off, pattern='$pat1024', keys=k)",
	'1024-byte pattern accepted');
set_ext(join(',', ('marginalia') x 16));
is(parsed(), join("\n", ($MARG_DEF) x 16), '16 extractors accepted');
my $k63 = 'k' x 63;
set_ext("sqlcommenter(keys=$k63, rename=$k63:$k63)");
is(parsed(), "sqlcommenter(position=append, merge=off, url_decode=on, keys=$k63, rename=$k63:$k63)",
	'63-byte keys accepted');
my $sep8 = '<' x 8;
set_ext("marginalia(kv_sep='$sep8', pair_sep='>')");
is(parsed(), "marginalia(position=append, merge=off, kv_sep='$sep8', pair_sep='>')",
	'8-byte separator accepted');

# ---------------------------------------------------------------------------
# Generation: equivalent spellings do not bump it, real changes do
# ---------------------------------------------------------------------------
set_ext('sqlcommenter, marginalia');
my $g0 = gen();
set_ext(q{sqlcommenter( position = append ,url_decode=on),marginalia(kv_sep=':', merge=off)});
is(gen(), $g0, 'equivalent extractors: generation not bumped');
set_ext('sqlcommenter(position=prepend), marginalia');
cmp_ok(gen(), '>', $g0, 'changed extractors: generation bumped');

# ---------------------------------------------------------------------------
# Malformed input: each class rejected with a specific detail
# ---------------------------------------------------------------------------
set_ext('sqlcommenter, marginalia');
my $nine = join('', map { "($_)" } 'a' .. 'i');
my @bad = (
	# unknown names
	[ 'sqlcomenter', qr/Unknown extractor "sqlcomenter"\./, 'unknown extractor' ],
	[ 'appnames(format=sqlcommenter)', qr/Unknown extractor "appnames"\./, 'unknown extractor (appnames)' ],
	# appname
	[ 'appname', qr/Extractor "appname" requires parameter "format"\./, 'appname without parameters' ],
	[ 'appname(keys=a)', qr/Extractor "appname" requires parameter "format"\./, 'appname without format' ],
	[ 'appname(format=json)',
	  qr/Invalid value "json" for parameter "format" of extractor "appname": expected sqlcommenter, marginalia or regex\./,
	  'appname: invalid format' ],
	[ 'appname(format=appname)',
	  qr/Invalid value "appname" for parameter "format" of extractor "appname": expected sqlcommenter, marginalia or regex\./,
	  'appname: format=appname' ],
	[ 'appname(format=marginalia, position=any)',
	  qr/Unknown parameter "position" for extractor "appname"\./, 'appname takes no position' ],
	[ 'marginalia(format=marginalia)',
	  qr/Unknown parameter "format" for extractor "marginalia"\./, 'format is appname-only' ],
	[ 'appname(format=marginalia, foo=1)',
	  qr/Unknown parameter "foo" for extractor "appname"\./, 'appname: unknown parameter' ],
	[ 'appname(format=marginalia, format=regex)',
	  qr/Parameter "format" of extractor "appname" is given more than once\./, 'appname: duplicate format' ],
	[ 'appname(kv_sep=:, format=sqlcommenter)',
	  qr/Parameter "kv_sep" of extractor "appname" is not allowed with format=sqlcommenter\./,
	  'appname: kv_sep needs format=marginalia' ],
	[ 'appname(format=sqlcommenter, pair_sep=;)',
	  qr/Parameter "pair_sep" of extractor "appname" is not allowed with format=sqlcommenter\./,
	  'appname: pair_sep needs format=marginalia' ],
	[ 'appname(format=marginalia, url_decode=on)',
	  qr/Parameter "url_decode" of extractor "appname" is not allowed with format=marginalia\./,
	  'appname: url_decode needs format=sqlcommenter' ],
	[ q{appname(format=marginalia, pattern='(a)')},
	  qr/Parameter "pattern" of extractor "appname" is not allowed with format=marginalia\./,
	  'appname: pattern needs format=regex' ],
	[ q{appname(format=regex, pattern='(a)', keys=k, kv_sep=':')},
	  qr/Parameter "kv_sep" of extractor "appname" is not allowed with format=regex\./,
	  'appname: kv_sep not for regex' ],
	[ q{appname(format=marginalia, kv_sep=',')},
	  qr/Parameter "kv_sep" \(","\) of extractor "appname" contains its pair_sep \(","\), so it can never match\./,
	  'appname marginalia: kv_sep containing pair_sep' ],
	[ 'appname(format=regex, keys=k)',
	  qr/Extractor "appname" requires parameter "pattern"\./, 'appname regex without pattern' ],
	[ q{appname(format=regex, pattern='(a)')},
	  qr/Extractor "appname" requires parameter "keys"\./, 'appname regex without keys' ],
	[ q{appname(format=regex, pattern='(a)(b)', keys=k)},
	  qr/Extractor "appname" has 1 key but its pattern has 2 capture groups\./,
	  'appname regex: key count' ],
	[ q{appname(format=regex, pattern='(a)\1', keys=k)},
	  qr/Pattern of extractor "appname" uses back-references, which are not allowed\./,
	  'appname regex: back-reference' ],
	[ q{appname(format=regex, pattern='(a', keys=k)},
	  qr/Pattern of extractor "appname" is invalid: parentheses \(\) not balanced\./,
	  'appname regex: compile failure' ],
	[ q{appname(format=marginalia, keys='bad key')},
	  qr/Key "bad key" in parameter "keys" of extractor "appname"/, 'appname: invalid key' ],
	[ join(',', ('appname(format=marginalia)') x 17),
	  qr/The list has more than 16 extractors\./, 'appname counts toward the 16 extractors' ],
	[ 'marginalia(url_decode=on)', qr/Unknown parameter "url_decode" for extractor "marginalia"\./,
	  'url_decode is sqlcommenter-only' ],
	[ 'sqlcommenter(kv_sep=:)', qr/Unknown parameter "kv_sep" for extractor "sqlcommenter"\./,
	  'kv_sep is marginalia-only' ],
	[ q{marginalia(pattern='(a)')}, qr/Unknown parameter "pattern" for extractor "marginalia"\./,
	  'pattern is regex-only' ],
	[ q{regex(pattern='(a)', keys=k, url_decode=on)},
	  qr/Unknown parameter "url_decode" for extractor "regex"\./, 'url_decode not for regex' ],
	[ 'sqlcommenter(foo=1)', qr/Unknown parameter "foo" for extractor "sqlcommenter"\./,
	  'unknown parameter' ],
	# duplicates
	[ 'sqlcommenter(position=append, position=prepend)',
	  qr/Parameter "position" of extractor "sqlcommenter" is given more than once\./,
	  'duplicate parameter' ],
	[ 'marginalia(KV_SEP=:, kv_sep==)',
	  qr/Parameter "kv_sep" of extractor "marginalia" is given more than once\./,
	  'duplicate parameter, different case' ],
	# invalid values
	[ 'sqlcommenter(position=middle)',
	  qr/Invalid value "middle" for parameter "position" of extractor "sqlcommenter": expected append, prepend or any\./,
	  'invalid position' ],
	[ 'marginalia(merge=maybe)',
	  qr/Invalid value "maybe" for parameter "merge" of extractor "marginalia": expected a Boolean value\./,
	  'invalid merge' ],
	[ 'sqlcommenter(url_decode=2)',
	  qr/Invalid value "2" for parameter "url_decode" of extractor "sqlcommenter": expected a Boolean value\./,
	  'invalid url_decode' ],
	[ 'sqlcommenter(position=)', qr/Parameter "position" of extractor "sqlcommenter" has an empty value\./,
	  'empty unquoted value' ],
	[ q{sqlcommenter(keys='')}, qr/Parameter "keys" of extractor "sqlcommenter" has an empty value\./,
	  'empty quoted value' ],
	[ q{sqlcommenter(keys='a||b')}, qr/Parameter "keys" of extractor "sqlcommenter" contains an empty key\./,
	  'empty key in keys' ],
	[ q{sqlcommenter(keys='a b')}, qr/Key "a b" in parameter "keys" of extractor "sqlcommenter" contains whitespace\./,
	  'key with whitespace' ],
	[ "sqlcommenter(keys=${k63}x)",
	  qr/Key "k{63}\.\.\." in parameter "keys" of extractor "sqlcommenter" is longer than 63 bytes\./,
	  '64-byte key' ],
	[ 'sqlcommenter(rename=foo)',
	  qr/Entry "foo" in parameter "rename" of extractor "sqlcommenter" is not of the form old:new\./,
	  'rename without colon' ],
	[ 'sqlcommenter(rename=a:b:c)',
	  qr/Entry "a:b:c" in parameter "rename" of extractor "sqlcommenter" is not of the form old:new\./,
	  'rename with two colons' ],
	[ 'sqlcommenter(rename=:b)', qr/Parameter "rename" of extractor "sqlcommenter" contains an empty key\./,
	  'rename with empty old key' ],
	[ "sqlcommenter(rename=a:${k63}x)",
	  qr/Key "k{63}\.\.\." in parameter "rename" of extractor "sqlcommenter" is longer than 63 bytes\./,
	  'rename to a 64-byte key' ],
	[ 'sqlcommenter(rename=a:b|a:c)',
	  qr/Key "a" is renamed more than once in parameter "rename" of extractor "sqlcommenter"\./,
	  'rename of the same key twice' ],
	[ 'sqlcommenter(keys=' . join('|', ('a') x 1025) . ')',
	  qr/Parameter "keys" of extractor "sqlcommenter" has more than 1024 entries\./,
	  'too many keys' ],
	# separators
	[ "marginalia(kv_sep='$sep8<')",
	  qr/Parameter "kv_sep" of extractor "marginalia" is longer than 8 bytes\./, '9-byte kv_sep' ],
	[ "marginalia(pair_sep='$sep8<')",
	  qr/Parameter "pair_sep" of extractor "marginalia" is longer than 8 bytes\./, '9-byte pair_sep' ],
	[ q{marginalia(kv_sep=',')},
	  qr/Parameter "kv_sep" \(","\) of extractor "marginalia" contains its pair_sep \(","\), so it can never match\./,
	  'kv_sep equal to the default pair_sep' ],
	[ q{marginalia(kv_sep=';', pair_sep=';')},
	  qr/Parameter "kv_sep" \(";"\) of extractor "marginalia" contains its pair_sep \(";"\)/,
	  'kv_sep equal to pair_sep' ],
	[ q{marginalia(kv_sep='=>', pair_sep='>')},
	  qr/Parameter "kv_sep" \("=>"\) of extractor "marginalia" contains its pair_sep \(">"\)/,
	  'kv_sep containing pair_sep' ],
	# bad quoting and syntax
	[ q{regex(pattern='(a), keys=k)},
	  qr/Unterminated quoted value for parameter "pattern" of extractor "regex"\./, 'unterminated quote' ],
	[ q{sqlcommenter(keys='a'b)},
	  qr/Unexpected "b" after the quoted value of parameter "keys" of extractor "sqlcommenter"\./,
	  'text after a quoted value' ],
	[ q{sqlcommenter(keys=a'b')},
	  qr/Value of parameter "keys" of extractor "sqlcommenter" contains a quote; quote the whole value\./,
	  'quote inside an unquoted value' ],
	[ 'regex(pattern=(a), keys=k)',
	  qr/Value of parameter "pattern" of extractor "regex" contains "\("; quote the whole value\./,
	  'parenthesis in an unquoted value' ],
	[ 'sqlcommenter(keys=a b)',
	  qr/Unexpected "b" after the value of parameter "keys" of extractor "sqlcommenter"\./,
	  'whitespace in an unquoted value' ],
	[ 'sqlcommenter(position=append',
	  qr/Missing "\)" after the parameters of extractor "sqlcommenter"\./, 'missing )' ],
	[ 'sqlcommenter(position)',
	  qr/Expected "=" after parameter "position" of extractor "sqlcommenter"\./, 'missing =' ],
	[ 'sqlcommenter()', qr/Expected a parameter name after "\(" of extractor "sqlcommenter"\./,
	  'empty parameter list' ],
	[ 'sqlcommenter(position=append,)', qr/Expected a parameter name after "," of extractor "sqlcommenter"\./,
	  'trailing comma in parameter list' ],
	[ 'sqlcommenter(position=append) x', qr/Unexpected "x" after extractor "sqlcommenter"\./,
	  'junk after an extractor' ],
	[ 'sqlcommenter,', qr/Empty entry in the extractor list\./, 'trailing comma' ],
	[ ',marginalia', qr/Empty entry in the extractor list\./, 'leading comma' ],
	[ "'sqlcommenter'", qr/Expected an extractor name at "'sqlcommenter'"\./, 'quoted name' ],
	[ join(',', ('marginalia') x 17), qr/The list has more than 16 extractors\./, '17 extractors' ],
	# regex
	[ 'regex(keys=k)', qr/Extractor "regex" requires parameter "pattern"\./, 'regex without pattern' ],
	[ q{regex(pattern='(a)')}, qr/Extractor "regex" requires parameter "keys"\./, 'regex without keys' ],
	[ q{regex(pattern='(a)\1', keys=k)},
	  qr/Pattern of extractor "regex" uses back-references, which are not allowed\./, 'back-reference' ],
	[ q{regex(pattern='(a)(b)\2', keys=k|l)},
	  qr/Pattern of extractor "regex" uses back-references, which are not allowed\./, 'back-reference \2' ],
	[ "regex(pattern='$nine', keys=" . join('|', 'a' .. 'i') . ')',
	  qr/Pattern of extractor "regex" has 9 capture groups, more than max_tags \(8\)\./,
	  'more capture groups than max_tags' ],
	[ q{regex(pattern='(a)(b)', keys=k)},
	  qr/Extractor "regex" has 1 key but its pattern has 2 capture groups\./, 'fewer keys than groups' ],
	[ q{regex(pattern='(a)', keys=k|l)},
	  qr/Extractor "regex" has 2 keys but its pattern has 1 capture group\./, 'more keys than groups' ],
	[ q{regex(pattern='abc', keys=k)},
	  qr/Extractor "regex" has 1 key but its pattern has 0 capture groups\./, 'pattern without groups' ],
	[ q{regex(pattern='(a', keys=k)},
	  qr/Pattern of extractor "regex" is invalid: parentheses \(\) not balanced\./, 'compile failure' ],
	[ q{regex(pattern='a{2,1}(b)', keys=k)},
	  qr/Pattern of extractor "regex" is invalid: invalid repetition count\(s\)\./, 'compile failure (bounds)' ]);
for my $c (@bad)
{
	like(ext_err($c->[0]), $c->[1], "rejected: $c->[2]");
}
my $pat1025 = $pat1024 . 'x';
like(ext_err("regex(pattern='$pat1025', keys=k)"),
	qr/Pattern of extractor "regex" is longer than 1024 bytes\./, 'rejected: 1025-byte pattern');
like(ext_err('sqlcomenter'), qr/ERROR:  invalid value for parameter "$P\.extractors": "sqlcomenter"/,
	'rejection is reported as an invalid value');
is(parsed(), "$SQLC_DEF\n$MARG_DEF", 'rejected ALTER SYSTEM values left the config alone');

# The 1 kB limit is checked before compiling: a long pattern that would not
# even compile is reported as too long.
like(ext_err("regex(pattern='(" . ('x' x 1100) . "', keys=k)"),
	qr/Pattern of extractor "regex" is longer than 1024 bytes\./, 'length checked before compiling');

# ---------------------------------------------------------------------------
# postgresql.conf form of the §4.2 regex example: the config-file lexer turns
# '' into ' and also processes backslash escapes, so \w must be written \\w
# there. Written exactly as in §4.2 (single backslashes), the backslashes are
# lost (still a valid pattern).
# ---------------------------------------------------------------------------
alter_and_reload("RESET $P.extractors");
$node->append_conf('postgresql.conf',
	q{pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\\\w+)\\\\s+op=(\\\\w+)'', keys=service|operation)'} . "\n");
alter_and_reload();
is(parsed(), q{regex(position=any, merge=off, pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)},
	'§4.2 regex example from postgresql.conf (backslashes doubled)');
$node->append_conf('postgresql.conf',
	q{pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\w+)\s+op=(\w+)'', keys=service|operation)'} . "\n");
alter_and_reload();
is(parsed(), q{regex(position=any, merge=off, pattern='svc=(w+)s+op=(w+)', keys=service|operation)},
	'§4.2 regex example from postgresql.conf as written: backslashes dropped by the conf lexer');

# ---------------------------------------------------------------------------
# A bad SIGHUP keeps the previous config, in the postmaster and in an
# existing backend
# ---------------------------------------------------------------------------
my $good = q{regex(pattern='op=(\w+)', keys=operation), marginalia(position=prepend)};
my $good_parsed = q{regex(position=any, merge=off, pattern='op=(\w+)', keys=operation)} . "\n"
  . "marginalia(position=prepend, merge=off, kv_sep=':', pair_sep=',')";
$node->append_conf('postgresql.conf', "$P.extractors = " . confq($good) . "\n");
alter_and_reload();
is(parsed(), $good_parsed, 'good config from postgresql.conf');
my $s = session_open();
my $spid = sq($s, 'SELECT pg_backend_pid()');
is(sq($s, 'SELECT pssc_guc_test_extractors(true)'), $good_parsed, 'session: good config');
my $sg = sq($s, 'SELECT pssc_guc_test_generation()');
my $g1 = gen();

for my $bad (q{regex(pattern='(a)\1', keys=k)}, 'sqlcommenter(position=middle)',
	q{marginalia(kv_sep=',')}, "regex(pattern='$nine', keys=" . join('|', 'a' .. 'i') . ')')
{
	my $logpos = -s $node->logfile;
	$node->append_conf('postgresql.conf', "$P.extractors = " . confq($bad) . "\n");
	alter_and_reload();
	$node->wait_for_log(qr/invalid value for parameter "$P\.extractors"/, $logpos);
	my $log = substr(slurp_file($node->logfile), $logpos);
	like($log, qr/invalid value for parameter "$P\.extractors": ".*"\n.*DETAIL:  \S/,
		"bad reload logged with a detail: $bad");
	is(show(), $good, "bad reload: previous value kept: $bad");
	is(parsed(), $good_parsed, "bad reload: previous parsed config kept (new backend): $bad");
	is(gen(), $g1, "bad reload: generation unchanged (new backend): $bad");
	is(sq($s, 'SELECT pssc_guc_test_extractors(true)'), $good_parsed,
		"bad reload: previous parsed config kept (existing backend): $bad");
	is(sq($s, 'SELECT pssc_guc_test_generation()'), $sg,
		"bad reload: generation unchanged (existing backend): $bad");
}

# ---------------------------------------------------------------------------
# Old blobs (and the regexes compiled to validate them) are freed: many
# reloads alternating two large configs must not grow the backend's malloc'd
# memory by anything near one blob per reload.
# ---------------------------------------------------------------------------
# Each config: 4 marginalia extractors with 500 keys each (~140 kB of blob)
# and 12 regexes with large bounded repetitions (expensive to compile).
sub big_config
{
	my ($c) = @_;
	my @e;
	for my $i (1 .. 4)
	{
		push @e, 'marginalia(keys=' . join('|', map { sprintf("$c%d_%055d", $i, $_) } 1 .. 500) . ')';
	}
	for my $i (1 .. 12)
	{
		push @e, "regex(pattern='$c$i=([a-z0-9_]{1,200})/(\\w{1,200}),(\\d{1,100})', keys=a|b|c)";
	}
	return join(', ', @e);
}
my @big = (big_config('p'), big_config('q'));
# This is about memory and generations, not compile time: ALTER SYSTEM
# test-compiles the 12 regexes without the time limit, which a host taking
# the CPU away from a VM can make them hit (that time counts as CPU time
# inside the VM). Reloads never reject a value for time (backlog
# 20261006-092320-1: a backend that did kept its old extractors).
$alter_prelude = "SELECT pssc_extract_test_regex_compile_limit(0);\n";
my $m_null = sq($s, 'SELECT pssc_guc_test_malloc_used() IS NULL');
SKIP:
{
	skip 'malloc statistics need glibc >= 2.33 (mallinfo2); leak check not run', 3
	  if $m_null eq 't';
	for my $i (1 .. 4)
	{
		alter_and_reload("SET $P.extractors = " . sqlq($big[ $i % 2 ]));
	}
	like(sq($s, 'SELECT pssc_guc_test_extractors(true)'), qr/^marginalia\(.*\nregex\(/s,
		'session: large config parsed');
	my $m1 = sq($s, 'SELECT pssc_guc_test_malloc_used()');
	my $g_before = sq($s, 'SELECT pssc_guc_test_generation()');
	my $cycles = 30;
	for my $i (1 .. $cycles)
	{
		alter_and_reload("SET $P.extractors = " . sqlq($big[ $i % 2 ]));
	}
	my $m2 = sq($s, 'SELECT pssc_guc_test_malloc_used()');
	is(sq($s, 'SELECT pssc_guc_test_generation()') - $g_before, $cycles,
		'session: every alternating reload replaced the extractors');
	my $blob = sq($s, 'SELECT pssc_guc_test_extractors_size()');
	note "extractors blob: $blob bytes; malloc growth over $cycles reloads: " . ($m2 - $m1);
	cmp_ok($m2 - $m1, '<', 1_000_000,
		"session: malloc'd memory stable over $cycles reloads (grew by " . ($m2 - $m1) . ' bytes)');
}
$alter_prelude = '';
is(sq($s, 'SELECT pg_backend_pid()'), $spid, 'session: same backend throughout');
session_close($s);

# ---------------------------------------------------------------------------
# Startup: an invalid value falls back to the default; max_tags bounds the
# capture count with the value the server was started with
# ---------------------------------------------------------------------------
sql("ALTER SYSTEM RESET $P.extractors");
$node->append_conf('postgresql.conf', "$P.extractors = 'sqlcommenter(position=middle)'\n");
my $logpos = -s $node->logfile;
$node->restart;
like(substr(slurp_file($node->logfile), $logpos),
	qr/invalid value for parameter "$P\.extractors": "sqlcommenter\(position=middle\)"/,
	'startup: invalid extractors reported');
is(parsed(), "$SQLC_DEF\n$MARG_DEF", 'startup: invalid extractors -> default used');

$node->append_conf('postgresql.conf', "$P.extractors = 'sqlcommenter'\n$P.max_tags = 2\n");
$node->restart;
like(ext_err(q{regex(pattern='(a)(b)(c)', keys=a|b|c)}),
	qr/Pattern of extractor "regex" has 3 capture groups, more than max_tags \(2\)\./,
	'max_tags = 2: three capture groups rejected');
set_ext(q{regex(pattern='(a)(b)', keys=a|b)});
is(parsed(), q{regex(position=any, merge=off, pattern='(a)(b)', keys=a|b)},
	'max_tags = 2: two capture groups accepted');
# A regex accepted before a restart that lowers max_tags is rejected at startup.
$node->append_conf('postgresql.conf', "$P.max_tags = 1\n");
$logpos = -s $node->logfile;
$node->restart;
like(substr(slurp_file($node->logfile), $logpos),
	qr/invalid value for parameter "$P\.extractors".*\n.*DETAIL:  Pattern of extractor "regex" has 2 capture groups, more than max_tags \(1\)\./,
	'max_tags lowered: stored regex rejected at startup');
is(parsed(), "$SQLC_DEF\n$MARG_DEF", 'max_tags lowered: default extractors used');

sql("ALTER SYSTEM RESET ALL");
$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|TRAP|terminated by signal|server process .* was terminated/,
	'server log has no crashes');

done_testing();
