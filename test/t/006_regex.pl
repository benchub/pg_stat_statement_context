# Regex extractor runtime (DESIGN.md §4.2, §6.10, §6.11; backlog
# 20261005-091225-10): src/regex_runtime.c compiles each regex extractor
# lazily per backend (REG_ADVANCED, C collation) after a config-generation
# change, frees the previous generation's regexes, matches comment bodies
# only (as pg_wchar, mapping captures back to bytes in the database
# encoding), reports capture group n as key n, and never fails the statement:
# a compile failure disables that extractor for the backend (counted in
# regex_compile_failures), a match error yields nothing, but query cancel is
# still honored. Driven through the TEST-ONLY module
# test/modules/pssc_extract_test (make install-test-modules), whose fault
# injection (pssc_regex_test_hook) stands in for engine failures.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('regex');
$node->init;
# enabled = off: the executor hooks (src/executor.c) would also run
# extraction for every statement of the test sessions, compiling and freeing
# regexes (and counting) on their own; here only the test module drives the
# regex runtime, so its lazy compile/free counts are exact.
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = '$P'\n$P.enabled = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pssc_extract_test');
for my $db ([ 'u8', 'UTF8' ], [ 'l1', 'LATIN1' ], [ 'sa', 'SQL_ASCII' ], [ 'ej', 'EUC_JP' ])
{
	$node->safe_psql('postgres',
		"CREATE DATABASE $db->[0] TEMPLATE template0 ENCODING '$db->[1]' "
		  . "LC_COLLATE 'C' LC_CTYPE 'C'");
	$node->safe_psql($db->[0], 'CREATE EXTENSION pssc_extract_test');
}

# SQL string literal (standard_conforming_strings: only quotes are doubled).
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

# Raw bytes as a bytea literal.
sub bq { return "'\\x" . unpack('H*', $_[0]) . "'::bytea"; }

# Hex of raw bytes (perl strings here are bytes: no "use utf8").
sub hx { return unpack('H*', $_[0]); }

# A persistent psql session (one backend for its whole life); see 003_guc.pl.
my @sessions;
my $session_marker = 0;
sub session_open
{
	my ($db) = @_;
	my %s = (in => '', out => '', err => '');
	$s{h} = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d', $node->connstr($db // 'u8') ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err},
		IPC::Run::timeout(600));
	push @sessions, \%s;
	return \%s;
}
# Run $q in session $s; returns (stdout, stderr).
sub sq_err
{
	my ($s, $q) = @_;
	my $m = '__pssc_done_' . ++$session_marker . '__';
	$s->{out} = '';
	$s->{err} = '';
	$s->{in} .= "$q;\n\\echo $m\n";
	$s->{h}->pump until $s->{out} =~ /^\Q$m\E$/m;
	(my $r = $s->{out}) =~ s/^\Q$m\E\n\z//m;
	chomp $r;
	return ($r, $s->{err});
}
sub sq
{
	my ($s, $q) = @_;
	my ($r, $err) = sq_err($s, $q);
	die "session error for <$q>: $err" if $err ne '';
	return $r;
}
sub session_close
{
	my ($s) = @_;
	$s->{in} .= "\\q\n";
	$s->{h}->finish;
	@sessions = grep { $_ != $s } @sessions;
}

# ALTER SYSTEM statements, then reload and wait until every backend we hold
# (and new ones) has processed it (scan_window moves to a new sentinel).
my $sentinel = 1000;
sub alter_and_reload
{
	$sentinel++;
	$node->safe_psql('postgres',
		join('', map { "ALTER SYSTEM $_;\n" } @_)
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
sub config
{
	my (%c) = @_;
	alter_and_reload(map {
		defined $c{$_} ? "SET $P.$_ = " . sqlq($c{$_}) : "RESET $P.$_"
	} qw(extractors tags exclude_tags));
}

# The SELECT list that renders one pssc_extract_test() result.
my @COLS = qw(tags serialized ntags oom invalid dropped regex_fail);
my $RCOLS = "concat_ws(E'\\t', array_to_string(tags, ','), encode(serialized, 'hex'), "
  . "ntags, oom, invalid_tags, dropped_tags, regex_compile_failures)";
sub parse_row
{
	my ($line) = @_;
	my @v = split /\t/, $line, -1;
	die "unexpected output <$line>" unless @v == @COLS;
	my %r;
	@r{@COLS} = @v;
	return \%r;
}
sub ex_sql { my ($q) = @_; return "SELECT $RCOLS FROM pssc_extract_test($q)"; }

# Run the pipeline on $q (a SQL expression, text or bytea) in a new backend;
# $o{pre} is run first in the same backend.
sub ex
{
	my ($q, %o) = @_;
	my $out = $node->safe_psql($o{db} // 'u8', ($o{pre} // '') . ex_sql($q));
	my @l = split /\n/, $out;
	return parse_row($l[-1]);
}
sub tags { return ex(@_)->{tags}; }
sub tq { return tags(sqlq($_[0]), @_[ 1 .. $#_ ]); }
# Same, in a persistent session.
sub sex { my ($s, $q) = @_; return parse_row(sq($s, ex_sql($q))); }
sub stq { my ($s, $q) = @_; return sex($s, sqlq($q))->{tags}; }
sub rstats { return sq($_[0], 'SELECT compiles, frees, live, failed FROM pssc_extract_test_regex_stats()'); }

# ---------------------------------------------------------------------------
# The DESIGN.md §4.2 example, verbatim in postgresql.conf
# ---------------------------------------------------------------------------
$node->append_conf('postgresql.conf', <<'EOC');
# Custom house format: /* svc=billing op=charge */
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=service|operation)'
pg_stat_statement_context.tags       = 'service,operation'
EOC
$node->reload;
$node->poll_query_until('postgres', "SELECT current_setting('$P.tags') = 'service,operation'")
  or die 'reload of the §4.2 example not processed';
is($node->safe_psql('postgres', "SHOW $P.extractors"),
	q{regex(pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)}, '§4.2 example: setting');
{
	my $r = ex(sqlq(q{SELECT * FROM t /* svc=billing op=charge */}), db => 'postgres');
	is($r->{tags}, 'operation=charge,service=billing', '§4.2 example: service and operation extracted');
	is($r->{serialized}, hx("operation\0charge\0service\0billing\0"), '§4.2 example: serialized');
	is("$r->{invalid} $r->{dropped} $r->{regex_fail}", '0 0 0', '§4.2 example: no counters');
}
is(tq(q{SELECT 'svc=billing op=charge'}), '', 'only comment text is matched, never the query');
is(tq(q{SELECT 1 /* svc=a */ op=b}), '', 'a match may not span a comment boundary');
is(tq(qq{SELECT 1 -- svc=a op=b\n}), 'operation=b,service=a', 'line comments are comment text too');
is(tq(q{/* svc=a op=b */ SELECT 1}), 'operation=b,service=a', 'default position any: leading comment');
is(tq(q{SELECT /* svc=a op=b */ 1}), 'operation=b,service=a', 'default position any: inner comment');
is(tq(q{SELECT 1 /* svc=a  op=b */}), 'operation=b,service=a', '\s+ matches several spaces');
is(tq(q{SELECT 1 /* svc=a op=-b */}), '', 'no match: no tags');
is(tq(q{SELECT 1 /* svc=a op=b */}, db => 'postgres', pre => 'SELECT pssc_extract_test_no_regex();'), '',
	'the tags come from the regex runtime (none without it)');

# ---------------------------------------------------------------------------
# position
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)', keys=service, position=prepend)});
is(tq(q{/* svc=lead */ SELECT 1 /* svc=tail */}), 'service=lead', 'prepend: leading comment');
is(tq(q{SELECT 1 /* svc=tail */}), '', 'prepend: trailing comment ignored');
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)', keys=service, position=append)});
is(tq(q{/* svc=lead */ SELECT 1 /* svc=tail */}), 'service=tail', 'append: trailing comment');
is(tq(q{/* svc=lead */ SELECT 1}), '', 'append: leading comment ignored');

# ---------------------------------------------------------------------------
# Match semantics: every non-overlapping match, first occurrence of a key
# wins; unmatched optional groups give no pair
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation)});
is(tq(q{SELECT 1 /* svc=a op=b svc=c op=d */}), 'operation=b,service=a',
	'repeated full matches: the first wins');
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='(?:svc=(\w+)|op=(\w+))', keys=service|operation)});
is(tq(q{SELECT 1 /* op=x junk svc=y */}), 'operation=x,service=y',
	'all matches in a comment are used: alternation yields both keys');
is(tq(q{SELECT 1 /* svc=1 svc=2 op=3 op=4 */}), 'operation=3,service=1',
	'first occurrence of each key wins within a comment');
is(tq(q{SELECT 1 /* svc=1 */ /* svc=2 op=3 */}), 'operation=3,service=1',
	'and across comments; later comments fill missing keys');
{
	my $r = ex(sqlq(q{SELECT 1 /* svc=a */}));
	is("$r->{tags} $r->{ntags}", 'service=a 1', 'a group that did not participate gives no pair');
}
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)(?:\s+op=(\w+))?', keys=service|operation)});
{
	my $r = ex(sqlq(q{SELECT 1 /* svc=a */}));
	is("$r->{tags} $r->{ntags}", 'service=a 1', 'unmatched optional group: no pair (not an empty value)');
	$r = ex(sqlq(q{SELECT 1 /* svc=a op=b */}));
	is("$r->{tags} $r->{ntags}", 'operation=b,service=a 2', 'optional group matched');
}
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w*);', keys=service)});
{
	my $r = ex(sqlq(q{SELECT 1 /* svc=; */}));
	is("$r->{serialized} $r->{ntags}", hx("service\0\0") . ' 1',
		'a group that matched the empty string gives an empty value');
}
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='(?:op=(\w+))?', keys=operation)});
is(tq(q{SELECT 1 /* aa op=z */}), 'operation=z',
	'empty matches advance one character at a time (and terminate)');
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='(?:^a(\w)|^(\w)z)', keys=k1|k2)});
is(tq(q{SELECT 1 /*cz*/}), 'k2=c', '^ anchors at the start of the comment body');
# after the match "ab", the search from "cz" must not treat its start as ^
# (as regexp_matches(..., 'g') does not)
is(tq(q{SELECT 1 /*abcz*/}), 'k1=b', 'and later searches do not treat their start as ^');
is(tq(q{SELECT 1 /*xx cz*/}), '', '^ does not match after the start of the body');
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='(?i)ID=(\d+)', keys=id)});
is(tq(q{SELECT 1 /* id=42x */}), 'id=42', 'advanced regex syntax (embedded options, \d)');
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='k=([a-z]+)', keys=k)});
is(tq(q{SELECT 1 /* K=ABC k=abc */}), 'k=abc', 'case-sensitive by default (C collation)');

# A regex extractor in a chain with keys allowlist, rename and merge.
config(tags => 'service,op,action', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)\s+op=(\w+)', keys=service|operation, rename=operation:op), }
	  . q{sqlcommenter(position=any, merge=on)});
is(tq(q{SELECT 1 /* svc=a op=b */ /*action='x',service='s'*/}), 'action=x,op=b,service=a',
	'regex pairs go through rename and the allowlist; merge adds; regex wins a duplicate key');

# ---------------------------------------------------------------------------
# Multibyte comments: matching is per character; captures map to the right
# bytes in UTF8, EUC_JP, LATIN1 and SQL_ASCII databases
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='v=(.{3})', keys=v), regex(pattern='w=(\S+) x=(\S+)', keys=w|x, merge=on)});
{
	my $r = ex(bq("SELECT 1 /* \xc3\xb1\xe2\x82\xac v=\xc3\xa9\xe2\x82\xac\xc3\xbcX */"));
	is($r->{serialized}, hx("v\0\xc3\xa9\xe2\x82\xac\xc3\xbc\0"),
		'UTF8: . is one character; capture after multibyte text maps to its bytes');
	$r = ex(bq("SELECT 1 /* \xe6\x97\xa5\xe6\x9c\xac w=\xc3\xbcn\xc3\xaf x=\xf0\x9f\x98\x80! tail */"));
	is($r->{serialized}, hx("w\0\xc3\xbcn\xc3\xaf\0x\0\xf0\x9f\x98\x80!\0"),
		'UTF8: two captures with 2- and 4-byte characters');
	$r = ex(bq("SELECT 1 /* v=ab\xff */"));
	is("$r->{tags} $r->{oom} $r->{invalid}", ' f 0', 'UTF8: invalid bytes in the comment: no tags, no error');
	$r = ex(bq("SELECT 1 /* \xc6\xfc v=\x8f\xa2\xaf\xc6\xfcz w=\xa4\xa2 x=\x8e\xb1 */"), db => 'ej');
	is($r->{serialized}, hx("v\0\x8f\xa2\xaf\xc6\xfcz\0w\0\xa4\xa2\0x\0\x8e\xb1\0"),
		'EUC_JP: 3-byte (SS3), 2-byte and SS2 characters');
	$r = ex(bq("SELECT 1 /* \xe9\xe9 v=\xe9\xe8xy */"), db => 'l1');
	is($r->{serialized}, hx("v\0\xe9\xe8x\0"), 'LATIN1: one byte per character');
	$r = ex(bq("SELECT 1 /* \xc3\xa9 v=\xc3\xa9\xffy */"), db => 'sa');
	is($r->{serialized}, hx("v\0\xc3\xa9\xff\0"), 'SQL_ASCII: one byte per character');
}
# A pattern that is not valid in this database's encoding cannot be compiled
# here (the check_hook validated it in another encoding): that extractor is
# disabled and counted, the others still work.
config(tags => '*', exclude_tags => '',
	extractors => qq{regex(pattern='x\xe2\x82\xac=(\\w+)', keys=e), regex(pattern='v=(\\w+)', keys=v, merge=on)});
{
	my $r = ex(bq("SELECT 1 /* x\xe2\x82\xac=1 v=2 */"));
	is("$r->{tags} $r->{regex_fail}", "e=1,v=2 0", 'UTF8: non-ASCII pattern compiles and matches');
	$r = ex(bq("SELECT 1 /* v=2 */"), db => 'ej');
	is("$r->{tags} $r->{regex_fail}", 'v=2 1',
		'EUC_JP: pattern invalid in the database encoding: disabled and counted, statement succeeds');
}

# ---------------------------------------------------------------------------
# Compile failures: only that extractor is disabled, once per generation,
# counted; the statement succeeds
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)', keys=service), }
	  . q{regex(pattern='op=(\w+)', keys=operation, merge=on), sqlcommenter(position=any, merge=on)});
my $Q = sqlq(q{SELECT 1 /* svc=s op=o */ /*a='x'*/});
for my $pa ((map { [ 'compile', $_ ] } qw(espace etoobig oom error)),
	[ 'context', 'oom' ], [ 'context', 'error' ])
{
	my ($phase, $act) = @$pa;
	# 'context': failure before the pattern's memory context exists
	my $action = $phase eq 'compile' ? $act : "$phase $act";
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('$phase', 0, '$act', -1)");
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 1',
		"compile failure ($action): only that extractor disabled, counted, statement succeeds");
	$r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 0',
		"compile failure ($action): stays disabled, not retried or recounted");
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "compile failure ($action): one attempt");
	is(rstats($s), '1|0|1|1', "compile failure ($action): one compiled, one failed");
	is(sq($s, 'SELECT 42'), 42, "compile failure ($action): session healthy");
	sq($s, "SELECT pssc_extract_test_regex_inject('$phase', -1, 'none')");
	# A new generation retries the compilation.
	alter_and_reload("SET $P.exclude_tags = 'zz'");
	$r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o,service=s 0',
		"compile failure ($action): next generation compiles it again");
	is(rstats($s), '3|1|2|0', "compile failure ($action): old regex freed, both compiled");
	session_close($s);
	alter_and_reload("SET $P.exclude_tags = ''");
}
{
	# per backend: another backend is not affected
	my $r = ex($Q, pre => q{SELECT pssc_extract_test_regex_inject('compile', 1, 'espace', -1);});
	is("$r->{tags} $r->{regex_fail}", 'a=x,service=s 1', 'compile failure of the second extractor');
	is(tags($Q), 'a=x,operation=o,service=s', 'other backends compile normally');
}

# Interrupts during compilation are honored and are not compile failures.
for my $c ([ 'regcancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'cancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'sleep', "SET statement_timeout = '300ms'", qr/canceling statement due to statement timeout/ ],
	[ 'context cancel', 'SELECT 1', qr/canceling statement due to user request/ ])
{
	my ($action, $pre, $re) = @$c;
	my ($phase, $act) = $action =~ /^(context) (\w+)$/ ? ($1, $2) : ('compile', $action);
	my $s = session_open();
	sq($s, $pre);
	sq($s, "SELECT pssc_extract_test_regex_inject('$phase', 0, '$act', 1)");
	my (undef, $err) = sq_err($s, ex_sql($Q));
	like($err, $re, "compile interrupted ($action): the statement is canceled");
	sq($s, 'RESET statement_timeout');
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o,service=s 0',
		"compile interrupted ($action): not disabled, compiled on next use");
	is(rstats($s), '2|0|2|0', "compile interrupted ($action): not counted as failed");
	session_close($s);
}

# ---------------------------------------------------------------------------
# Match errors: no pairs from that comment, statement succeeds, extractor
# stays enabled; interrupts are honored
# ---------------------------------------------------------------------------
for my $action (qw(espace etoobig oom error))
{
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('exec', 0, '$action', 1)");
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail} $r->{oom}", 'a=x,operation=o 0 f',
		"match error ($action): no pairs from that extractor, statement succeeds");
	$r = sex($s, $Q);
	is($r->{tags}, 'a=x,operation=o,service=s', "match error ($action): extractor still enabled");
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "match error ($action): injected once");
	session_close($s);
}
for my $c ([ 'regcancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'cancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'sleep', "SET statement_timeout = '300ms'", qr/canceling statement due to statement timeout/ ])
{
	my ($action, $pre, $re) = @$c;
	my $s = session_open();
	sq($s, $pre);
	sq($s, "SELECT pssc_extract_test_regex_inject('exec', 1, '$action', 1)");
	my (undef, $err) = sq_err($s, ex_sql($Q));
	like($err, $re, "match interrupted ($action): the statement is canceled");
	sq($s, 'RESET statement_timeout');
	is(sex($s, $Q)->{tags}, 'a=x,operation=o,service=s', "match interrupted ($action): next statement fine");
	session_close($s);
}

# ---------------------------------------------------------------------------
# Lazy compilation; a reload recompiles and frees the old regexes
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='svc=(\w+)', keys=service), sqlcommenter(position=any, merge=on)});
{
	my $s = session_open();
	is(rstats($s), '0|0|0|0', 'nothing compiled before first use');
	is(stq($s, q{SELECT 1}), '', 'statement without comments');
	is(rstats($s), '0|0|0|0', 'no comment: still nothing compiled');
	is(stq($s, q{SELECT 1 /* svc=a */}), 'service=a', 'first use');
	is(rstats($s), '1|0|1|0', 'compiled on first use');
	is(stq($s, q{SELECT 1 /* svc=b */}), 'service=b', 'second use');
	is(rstats($s), '1|0|1|0', 'cached: not recompiled');
	alter_and_reload("SET $P.extractors = " . sqlq(q{regex(pattern='op=(\w+)', keys=service), sqlcommenter(position=any, merge=on)}));
	is(rstats($s), '1|0|1|0', 'reload alone compiles nothing');
	is(stq($s, q{SELECT 1 /* svc=a op=c */}), 'service=c', 'new pattern used after reload');
	is(rstats($s), '2|1|1|0', 'recompiled, old regex freed');
	alter_and_reload();
	is(stq($s, q{SELECT 1 /* op=d */}), 'service=d', 'unrelated reload');
	is(rstats($s), '2|1|1|0', 'unrelated reload: no recompilation');
	alter_and_reload("SET $P.extractors = 'sqlcommenter(position=any)'");
	is(stq($s, q{SELECT 1 /*x='1'*/}), 'x=1', 'config without regex');
	is(rstats($s), '2|2|0|0', 'regexes of a replaced config are freed even if no regex runs');
	session_close($s);
}

# Reloads alternating two configs of 16 expensive regexes must not grow the
# backend's memory (malloc'd on PG14/15, palloc'd on PG16+).
sub big_regex_config
{
	my ($c) = @_;
	return join(', ', map {
		"regex(pattern='$c$_=([a-z0-9_]{1,150})/(\\w{1,150}),(\\d{1,80})', keys=a$_|b$_|c$_, merge=on)"
	} 1 .. 16);
}
{
	my @big = (big_regex_config('p'), big_regex_config('q'));
	my $s = session_open();
	my $mem = sub {
		my ($m, $c) = split /\|/, sq($s, 'SELECT malloc_used, context_bytes FROM pssc_extract_test_mem()');
		return ($m eq '' ? 0 : $m) + $c;
	};
	my $probe = sqlq(q{SELECT 1 /* p3=abc/def,12 q4=x/y,1 */});
	alter_and_reload("SET $P.tags = '*'", "SET $P.extractors = " . sqlq($big[0]));
	my $m0 = $mem->();
	is(sex($s, $probe)->{tags}, 'a3=abc,b3=def,c3=12', 'large config: matches');
	my $m_compiled = $mem->();
	my $size = $m_compiled - $m0;
	note "16 compiled regexes: $size bytes";
	for my $i (1 .. 4)
	{
		alter_and_reload("SET $P.extractors = " . sqlq($big[ $i % 2 ]));
		sex($s, $probe);
	}
	my $m1 = $mem->();
	my $cycles = 30;
	for my $i (1 .. $cycles)
	{
		alter_and_reload("SET $P.extractors = " . sqlq($big[ $i % 2 ]));
		my $r = sex($s, $probe);
		die "cycle $i: unexpected tags $r->{tags}"
		  unless $r->{tags} eq ($i % 2 ? 'a4=x,b4=y,c4=1' : 'a3=abc,b3=def,c3=12');
	}
	my $m2 = $mem->();
	my ($compiles, $frees, $live) = split /\|/, rstats($s);
	is($compiles - $frees, $live, 'every compiled regex but the live ones was freed');
	is($live, 16, 'one generation of regexes live');
	cmp_ok($size * $cycles, '>', 5_000_000,
		"a leak of the compiled regexes would show ($size bytes per generation)");
	cmp_ok($m2 - $m1, '<', 1_000_000,
		"memory stable over $cycles recompiling reloads (grew by " . ($m2 - $m1) . ' bytes)');
	session_close($s);
}

# ---------------------------------------------------------------------------
# Hostile input never errors, with the real runtime and patterns that match
# empty strings, in every encoding
# ---------------------------------------------------------------------------
config(tags => '*', exclude_tags => '',
	extractors => q{regex(pattern='(\w*)', keys=r1), regex(pattern='(.)(.)?(.)?', keys=r2|r3|r4, merge=on), }
	  . q{regex(pattern='^\s*(\S+)', keys=r5, merge=on), regex(pattern='(?:a=(\S*)|b=([^,]+))', keys=r6|r7, merge=on), }
	  . q{sqlcommenter(position=any, merge=on)});
{
	my @frag = ('/*', '*/', '--', "\n", "'", '=', ',', ' ', ';', 'a=', 'b=', "\0", "\xff",
		"\xc3\xa9", "\xe2\x82", "\x8f\xa2\xaf", "\xa4\xa2", "\x8e", 'k' x 70, 'v' x 300, 'SELECT 1', 'a', 'b');
	my $arr = 'ARRAY[' . join(',', map { bq($_) } @frag) . ']';
	my $nfrag = scalar @frag;
	for my $db (qw(u8 l1 sa ej))
	{
		my $out = $node->safe_psql($db, qq{
			SELECT setseed(0.42);
			SELECT count(*), max(r.nbytes) <= 512, bool_or(r.ntags > 0), bool_or(r.oom),
			       sum(r.regex_compile_failures)
			FROM generate_series(1, 2000) g,
			     LATERAL (SELECT string_agg(($arr)[1 + floor(random() * $nfrag)::int], ''::bytea) AS q
			              FROM generate_series(1, 1 + (g % 200)) i
			              WHERE g > 0) s,
			     LATERAL pssc_extract_test(s.q) r});
		my @l = split /\n/, $out;
		is($l[-1], '2000|t|t|f|0', "$db: 2000 random inputs with the real regex runtime: no error");
	}
	my $out = $node->safe_psql('u8', q{
		SELECT ntags > 0, oom FROM pssc_extract_test('SELECT 1 /*' || repeat('é a= b=x, ', 60) || '*/')});
	is($out, 't|f', 'long multibyte comment');
}

$node->stop;
unlike(slurp_file($node->logfile), qr/PANIC|TRAP|terminated by signal|server process .* was terminated/,
	'server log has no crashes');

done_testing();
