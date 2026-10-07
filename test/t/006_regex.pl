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
use Time::HiRes qw(usleep time);

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
# SQL run before the ALTER SYSTEM statements, in the same session.
our $alter_pre = '';
sub alter_and_reload
{
	$sentinel++;
	$node->safe_psql('postgres',
		$alter_pre
		  . join('', map { "ALTER SYSTEM $_;\n" } @_)
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
	# without the compile time limit, which would expire first
	[ 'sleep', "SELECT pssc_extract_test_regex_compile_limit(0); SET statement_timeout = '300ms'",
		qr/canceling statement due to statement timeout/ ],
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
# Compile time limit (backlog 20261006-021334-1): a lazy compile that outlasts
# PSSC_REGEX_COMPILE_LIMIT_MS (100 ms) is aborted and counted as a compile
# failure, without failing the statement or leaving a cancel pending, on
# either engine behavior (PG16+ raise the pending cancel, simulated by
# 'sleep'; PG14/15 return REG_CANCEL, simulated by 'regsleep'). Both are
# busy (CPU-bound) past the limit: the test module puts the deadline off
# until the attempt has used the whole limit in CPU time, so that on a
# loaded host they are not taken for a stall and retried. A genuine
# cancel or statement_timeout that arrives after the limit expired still
# cancels the statement.
# ---------------------------------------------------------------------------
my $LIMIT_MS = 100;
my $BOUND_S = 10;	# generous: without the limit these take 50-60 s
for my $action (qw(sleep regsleep))
{
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', 1)");
	my $t0 = time;
	my $r = sex($s, $Q);
	my $dt = time - $t0;
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 1',
		"compile over the time limit ($action): extractor disabled, counted, statement succeeds");
	cmp_ok($dt, '<', $BOUND_S, "compile over the time limit ($action): aborted (${dt}s)");
	cmp_ok($dt, '>=', 0.9 * $LIMIT_MS / 1000, "compile over the time limit ($action): not before the limit");
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "compile over the time limit ($action): one attempt");
	is(rstats($s), '1|0|1|1', "compile over the time limit ($action): one compiled, one failed");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42',
		"compile over the time limit ($action): no cancel left pending");
	$r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 0',
		"compile over the time limit ($action): stays disabled, not retried or recounted");
	session_close($s);
}
# The busy injections spend the limit in CPU time before letting it expire
# (see above). They must still honor a genuine cancel, statement_timeout or
# termination right away, and give up (with a WARNING) after a few seconds
# of wall-clock time if the CPU time doesn't come (a starved host). A 30 s
# limit makes the CPU budget far longer than either.
my $INJ_WALL_S = 5;
sub busy_injection_run
{
	my ($action, $pre, $interrupt) = @_;
	my ($out, $err) = ('', '');
	my $app = 'pssc_victim';
	my $script = $node->basedir . '/busy_injection.sql';
	open(my $fh, '>', $script) or die "could not write $script: $!";
	print $fh "SELECT pssc_extract_test_regex_compile_limit(30000);\n"
	  . "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', 1);\n"
	  . "$pre;\n" . ex_sql($Q) . ";\n";
	close($fh);
	my $t0 = time;
	my $h = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-f', $script, '-d', $node->connstr('u8') . " application_name=$app" ],
		'<', \undef, '>', \$out, '2>', \$err, IPC::Run::timeout(120));
	if (defined $interrupt)
	{
		$node->poll_query_until('postgres',
			"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = '$app' "
			  . "AND state = 'active' AND query LIKE '%pssc_extract_test(%'")
		  or die "victim did not start";
		usleep(300_000);
		$node->safe_psql('postgres',
			"SELECT $interrupt(pid) FROM pg_stat_activity WHERE application_name = '$app'");
	}
	$h->finish;
	return ($out, $err, time - $t0);
}
for my $action (qw(sleep regsleep))
{
	for my $c ([ 'pg_cancel_backend', 'SELECT 1', qr/canceling statement due to user request/ ],
		[ undef, "SET statement_timeout = '500ms'", qr/canceling statement due to statement timeout/ ],
		[ 'pg_terminate_backend', 'SELECT 1', qr/terminating connection due to administrator command/ ])
	{
		my ($interrupt, $pre, $re) = @$c;
		my $what = "busy injection ($action), " . ($interrupt // 'statement_timeout');
		my ($out, $err, $dt) = busy_injection_run($action, $pre, $interrupt);
		like($err, $re, "$what: honored");
		unlike($err, qr/gave up/, "$what: before the injection gave up");
		cmp_ok($dt, '<', $INJ_WALL_S - 1, "$what: promptly (${dt}s)");
	}
	my ($out, $err, $dt) = busy_injection_run($action, 'SELECT 1', undef);
	like($err, qr/WARNING:  pssc_extract_test: busy injection gave up after $INJ_WALL_S s/,
		"busy injection ($action), CPU budget not reached in time: gives up with a WARNING");
	cmp_ok($dt, '<', $BOUND_S, "busy injection ($action), CPU budget not reached in time: bounded (${dt}s)");
}
# The limit is wall-clock, but a compile that was descheduled (a stall:
# little CPU time used, e.g. a loaded host or VM) is retried, up to
# $ATTEMPTS attempts, so a normal pattern is not disabled by a stall. 'stall'
# / 'regstall' sleep instead of spinning; the injection fires once (then the
# real engine compiles) or on every attempt.
my $ATTEMPTS = 3;
for my $action (qw(stall regstall))
{
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', 1)");
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o,service=s 0',
		"compile stalled past the time limit ($action): retried, compiled, not counted");
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "compile stalled past the time limit ($action): stalled once");
	is(rstats($s), '2|0|2|0', "compile stalled past the time limit ($action): both compiled");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', "compile stalled past the time limit ($action): no cancel left pending");
	session_close($s);

	$s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', -1)");
	my $t0 = time;
	$r = sex($s, $Q);
	my $dt = time - $t0;
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 1',
		"compile stalled on every attempt ($action): disabled, counted, statement succeeds");
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), $ATTEMPTS,
		"compile stalled on every attempt ($action): $ATTEMPTS attempts");
	cmp_ok($dt, '>=', 0.9 * $ATTEMPTS * $LIMIT_MS / 1000, "compile stalled on every attempt ($action): each up to the limit (${dt}s)");
	cmp_ok($dt, '<', $BOUND_S, "compile stalled on every attempt ($action): bounded (${dt}s)");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', "compile stalled on every attempt ($action): no cancel left pending");
	session_close($s);
}
{
	my $val = sqlq(q{regex(pattern='zz=(\w+)', keys=zz)});
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'stall', 1)");
	my (undef, $err) = sq_err($s, "SELECT pssc_extract_test_set_local('$P.extractors', $val)");
	is($err, '', 'check hook, compile stalled once: accepted');
	is(sq($s, "SHOW $P.extractors"), q{regex(pattern='zz=(\w+)', keys=zz)}, 'check hook, compile stalled once: set');
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'stall', -1)");
	(undef, $err) = sq_err($s, "SELECT pssc_extract_test_set_local('$P.extractors', " . sqlq(q{regex(pattern='yy=(\w+)', keys=yy)}) . ')');
	like($err, qr/DETAIL:  Compiling the pattern of extractor "regex" took longer than $LIMIT_MS ms\./,
		'check hook, compile stalled on every attempt: rejected');
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), $ATTEMPTS, "check hook, compile stalled on every attempt: $ATTEMPTS attempts");
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'none')");
	session_close($s);
}
for my $c ([ 'lateint', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'lateregint', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'latewait', "SET statement_timeout = '500ms'", qr/canceling statement due to statement timeout/ ])
{
	my ($action, $pre, $re) = @$c;
	my $s = session_open();
	sq($s, $pre);
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', 1)");
	my $t0 = time;
	my (undef, $err) = sq_err($s, ex_sql($Q));
	my $dt = time - $t0;
	like($err, $re, "cancel after the compile time limit expired ($action): the statement is canceled");
	cmp_ok($dt, '<', $BOUND_S, "cancel after the compile time limit expired ($action): promptly (${dt}s)");
	sq($s, 'RESET statement_timeout');
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o,service=s 0',
		"cancel after the compile time limit expired ($action): not disabled, compiled on next use");
	is(rstats($s), '2|0|2|0', "cancel after the compile time limit expired ($action): not counted as failed");
	session_close($s);
}
# Interrupts that don't arrive as SIGINT, pending when the limit's own cancel
# is consumed, must still be delivered: a recovery conflict (SIGUSR1; it sets
# QueryCancelPending itself on PG14-16 and only its own flags on PG17+) and,
# on PG17+, transaction_timeout (processed after the cancel).
for my $action (qw(lateconflict lateregconflict))
{
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, '$action', 1)");
	my $t0 = time;
	my (undef, $err) = sq_err($s, ex_sql($Q));
	my (undef, $err2) = sq_err($s, 'SELECT pg_sleep(0.2), 42');
	my $dt = time - $t0;
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "recovery conflict after the compile time limit ($action): injected");
	like("$err$err2", qr/canceling statement due to conflict with recovery/,
		"recovery conflict after the compile time limit ($action): not lost");
	cmp_ok($dt, '<', $BOUND_S, "recovery conflict after the compile time limit ($action): promptly (${dt}s)");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', "recovery conflict after the compile time limit ($action): nothing left pending");
	session_close($s);
}
SKIP:
{
	skip 'transaction_timeout needs PostgreSQL 17+', 2
	  unless $node->safe_psql('postgres', "SELECT count(*) FROM pg_settings WHERE name = 'transaction_timeout'");
	my ($out, $err) = ('', '');
	$node->psql('u8',
		"SELECT pssc_extract_test_regex_inject('compile', 0, 'latewait', 1);\n"
		  . "SET transaction_timeout = '500ms';\nBEGIN;\n" . ex_sql($Q) . ";\n"
		  . "SELECT pg_sleep(2), 42;\nCOMMIT;\n",
		stdout => \$out, stderr => \$err, on_error_stop => 0);
	like($err, qr/terminating connection due to transaction timeout/,
		'transaction_timeout after the compile time limit: not lost');
	unlike($out, qr/\|42/, 'transaction_timeout after the compile time limit: the transaction does not go on');
}
{
	# normalize rules share the compile path
	alter_and_reload("SET $P.normalize = " . sqlq(q{service: 's' => 'S'}));
	my $s = session_open();
	is(sex($s, $Q)->{tags}, 'a=x,operation=o,service=S', 'normalize rule applies');
	session_close($s);
	$s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('norm_compile', 0, 'sleep', 1)");
	my $t0 = time;
	my $r = sex($s, $Q);
	my $dt = time - $t0;
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o 1',
		'normalize rule over the compile time limit: disabled, counted, its key dropped, statement succeeds');
	cmp_ok($dt, '<', $BOUND_S, "normalize rule over the compile time limit: aborted (${dt}s)");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', 'normalize rule over the compile time limit: no cancel left pending');
	session_close($s);
	alter_and_reload("RESET $P.normalize");
}

# The real engine on pathological patterns (found by the SQL fuzzer):
# exponential NFA work from a bounded repetition of a group with
# empty-matching branches. $SLOW compiles in seconds, $HUGE runs for about a
# minute and then fails as "too complex".
my $SLOW = q{((?:(?:$)|\Zda|(?<!1)|\S){0,15})};
my $HUGE = q{((?:(?:$)|\Zda|(?<!1)|\S){0,255})};
{
	my $s = session_open();
	is(sq($s, "SELECT pssc_extract_test_regex_compile_limit(0)"), $LIMIT_MS,
		"the compile time limit is $LIMIT_MS ms");
	my $t0 = time;
	sq($s, "SELECT pssc_extract_test_set_local('$P.extractors', "
		  . sqlq("regex(pattern='$SLOW', keys=slow), sqlcommenter(position=any, merge=on)") . ')');
	my $t_full = time - $t0;
	cmp_ok($t_full, '>', 0.5, "slow pattern: takes long to compile without the limit (${t_full}s)");
	sq($s, "SELECT pssc_extract_test_regex_compile_limit($LIMIT_MS)");
	$t0 = time;
	my $r = sex($s, sqlq(q{SELECT 1 /* x */ /*a='x'*/}));
	my $dt = time - $t0;
	is("$r->{tags} $r->{regex_fail}", 'a=x 1',
		'slow pattern, real engine: lazy compile aborted at the limit, counted, statement succeeds');
	cmp_ok($dt, '<', $t_full / 2, "slow pattern, real engine: aborted early (${dt}s vs ${t_full}s)");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', 'slow pattern, real engine: no cancel left pending');
	session_close($s);
}
# The check hooks reject patterns that outlast the limit, promptly.
for my $c ([ 'extractors', "regex(pattern='$HUGE', keys=a)", 'extractor "regex"' ],
	[ 'extractors', "regex(pattern='$SLOW', keys=a)", 'extractor "regex"' ],
	[ 'normalize', "a: '$SLOW' => 'x'", 'rule 1' ])
{
	my ($name, $val, $what) = @$c;
	my $t0 = time;
	my ($ret, undef, $err) = $node->psql('postgres', "ALTER SYSTEM SET $P.$name = " . sqlq($val));
	my $dt = time - $t0;
	isnt($ret, 0, "check hook: $name with a pathological pattern rejected");
	like($err, qr/DETAIL:  Compiling the pattern of \Q$what\E took longer than $LIMIT_MS ms\./,
		"check hook: $name: the detail names the compile time limit");
	cmp_ok($dt, '<', $BOUND_S, "check hook: $name: rejected promptly (${dt}s)");
}
for my $action (qw(sleep regsleep))
{
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, '$action', 1)");
	my $t0 = time;
	my (undef, $err) = sq_err($s, "ALTER SYSTEM SET $P.extractors = " . sqlq(q{regex(pattern='zz=(\w+)', keys=zz)}));
	my $dt = time - $t0;
	like($err, qr/DETAIL:  Compiling the pattern of extractor "regex" took longer than $LIMIT_MS ms\./,
		"check hook, compile over the time limit ($action): rejected");
	cmp_ok($dt, '<', $BOUND_S, "check hook, compile over the time limit ($action): promptly (${dt}s)");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', "check hook ($action): no cancel left pending");
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'none')");
	session_close($s);
}
# A retried attempt leaves nothing behind (backlog 20261006-080948-1): an
# attempt stopped by the limit after the real engine allocated (PG16+ throw
# out of pg_regcomp() before its cleanup, PG14/15 return REG_CANCEL) and
# then retried must not keep the abandoned allocations. 'expire' makes the
# limit (60 s here) expire into the compile of $MID, at half the time a
# clean compile took on this host (0.5-1 s), so that the abandoned attempt
# has allocated about half of what the compile needs whatever the host's
# speed; the attempt used little CPU time, so it is retried and compiles.
# The backend's memory growth from the lazy compile (and the check hook's
# test compile before it) must match a clean compile's. That the expiry
# landed mid-compile is checked directly: the hook runs once per attempt,
# so it ran twice only if the first attempt was stopped and retried. A
# trivial pattern, whose compile ends long before the expiry, shows that it
# then runs once.
{
	my $s = session_open();
	sq($s, 'SELECT pssc_extract_test_regex_compile_limit(60000)');
	sq($s, "SELECT pssc_extract_test_set_local('$P.extractors', " . sqlq(q{regex(pattern='zz=(\w+)', keys=zz)}) . ')');
	sq($s, "SELECT pssc_extract_test_regex_inject('compile', 0, 'expire', 1)");
	sex($s, sqlq(q{SELECT 1 /* zz=a */}));
	is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, 'expiry after the compile finished: injected once');
	is(sq($s, 'SELECT pssc_extract_test_regex_attempts()'), 1, 'expiry after the compile finished: one attempt, not retried');
	is(rstats($s), '1|0|1|0', 'expiry after the compile finished: one regex live');
	is(sq($s, 'SELECT pg_sleep(0.4), 42'), '|42', 'expiry after the compile finished: no cancel left pending');
	session_close($s);
}
my $MID = q{((?:(?:$)|\Zda|(?<!1)|\S){0,14})};
for my $c ([ 'extractor', "regex(pattern='$MID', keys=slow), sqlcommenter(position=any, merge=on)", undef, 'compile' ],
	[ 'normalize rule', 'sqlcommenter(position=any)', "a: '$MID' => 'y'", 'norm_compile' ])
{
	my ($what, $ext, $norm, $phase) = @$c;
	my %res;
	for my $expire (0, 1)
	{
		my $s = session_open();
		my $mem = sub {
			my ($m, $cb) = split /\|/, sq($s, 'SELECT malloc_used, context_bytes FROM pssc_extract_test_mem()');
			return ($m eq '' ? 0 : $m) + $cb;
		};
		my $m0 = $mem->();
		sq($s, 'SELECT pssc_extract_test_regex_compile_limit(60000)');
		if ($expire)
		{
			sq($s, 'SELECT pssc_extract_test_regex_expire_ms(' . int(1000 * $res{0}[1] / 2 + 1) . ')');
			sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'expire', 1)");
		}
		my (undef, $err) = sq_err($s, "SELECT pssc_extract_test_set_local('$P.extractors', " . sqlq($ext) . ')');
		if (defined $norm)
		{
			my (undef, $err2) = sq_err($s, "SELECT pssc_extract_test_set_local('$P.normalize', " . sqlq($norm) . ')');
			$err .= $err2;
		}
		is($err, '', "$what, check hook" . ($expire ? ', attempt expired mid-compile' : '') . ': accepted');
		if ($expire)
		{
			is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "$what, check hook: expired once");
			is(sq($s, 'SELECT pssc_extract_test_regex_attempts()'), 2,
				"$what, check hook: the attempt was stopped mid-compile and retried");
		}
		sq($s, "SELECT pssc_extract_test_regex_inject('$phase', 0, 'expire', 1)") if $expire;
		my $t0 = time;
		my $r = sex($s, sqlq(q{SELECT 1 /* x */ /*a='x'*/}));
		my $dt = time - $t0;
		my $grew = $mem->() - $m0;
		my $name = "$what, " . ($expire ? 'attempt expired mid-compile, retried' : 'clean compile');
		is($r->{regex_fail}, 0, "$name: compiled, not counted");
		is(rstats($s), '1|0|1|0', "$name: one regex live");
		if ($expire)
		{
			is(sq($s, 'SELECT pssc_extract_test_regex_injected()'), 1, "$name: expired once");
			is(sq($s, 'SELECT pssc_extract_test_regex_attempts()'), 2, "$name: stopped mid-compile and retried");
		}
		note "$name: ${dt}s, backend grew by $grew bytes";
		$res{$expire} = [ $grew, $dt ];
		session_close($s);
	}
	cmp_ok($res{1}[0] - $res{0}[0], '<', $res{0}[0] / 4,
		"$what: the interrupted attempt's allocations were released ($res{1}[0] vs $res{0}[0] bytes)");
}
unlike($node->safe_psql('postgres', "SELECT pg_read_file('postgresql.auto.conf')"), qr/zz=|\{0,15\}|\{0,255\}/,
	'rejected patterns were not written by ALTER SYSTEM');
is($node->safe_psql('postgres', "SHOW $P.extractors"),
	q{regex(pattern='svc=(\w+)', keys=service), regex(pattern='op=(\w+)', keys=operation, merge=on), sqlcommenter(position=any, merge=on)},
	'the configuration is unchanged');

# Re-reading the configuration file (backlog 20261006-092320-1): time alone
# never rejects a value there. A backend's re-check after a reload whose
# test compile hits the limit on every attempt must still apply the value
# the postmaster accepted, or it would silently (DEBUG3) keep its old
# configuration while other backends switch. 'sleep' uses the limit's
# CPU time before it runs out, as a host preempting the VM looks from inside
# the guest (charged to the process, so not retried as a stall). Also in a
# session idle in a transaction block, which processes the reload there.
for my $c ([ 'extractors', 'sleep', 0 ], [ 'extractors', 'regsleep', 0 ],
	[ 'extractors', 'sleep', 1 ], [ 'normalize', 'sleep', 0 ])
{
	my ($name, $action, $in_xact) = @$c;
	my $what = "reload re-check in a backend, $name, compile over the limit ($action"
	  . ($in_xact ? ', idle in transaction' : '') . ')';
	my $orig = $node->safe_psql('u8', "SHOW $P.extractors");
	my ($val, $tags) =
	  $name eq 'extractors'
	  ? ("$orig, regex(pattern='op=(\\w+)', keys=op2, merge=on)", 'a=x,op2=o,operation=o,service=s')
	  : (q{service: 's' => 'S'}, 'a=x,operation=o,service=S');
	my $s = session_open();
	sq($s, 'BEGIN') if $in_xact;
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, '$action', -1)");
	my $t0 = time;
	alter_and_reload("SET $P.$name = " . sqlq($val));
	my $dt = time - $t0;
	cmp_ok(sq($s, 'SELECT pssc_extract_test_regex_injected()'), '>=', 1, "$what: the re-check compile hit the limit");
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'none')");
	is(sq($s, "SHOW $P.$name"), $val, "$what: value applied, as by the postmaster");
	is(sq($s, 'SELECT pg_sleep(0.2), 42'), '|42', "$what: no cancel left pending");
	sq($s, 'COMMIT') if $in_xact;
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", "$tags 0", "$what: compiled lazily and used");
	cmp_ok($dt, '<', $BOUND_S, "$what: bounded (${dt}s)");
	session_close($s);
	alter_and_reload($name eq 'extractors' ? "SET $P.$name = " . sqlq($orig) : "RESET $P.$name");
}
# A query in session $s forced into a parallel worker. The worker restores
# the leader's settings, which runs the check hooks again there; it never
# extracts, so it must neither fail the query nor spend a compile on them.
my $force_parallel =
  $node->safe_psql('postgres', 'SHOW server_version_num') >= 160000
  ? 'debug_parallel_query' : 'force_parallel_mode';
sub parallel_query_ok
{
	my ($s, $what, $bound) = @_;
	$bound //= $BOUND_S;
	my $t0 = time;
	my ($out, $err) = sq_err($s,
		"SET $force_parallel = on; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;\n"
		  . 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM generate_series(1, 10)');
	my $dt = time - $t0;
	sq($s, "RESET $force_parallel; RESET parallel_setup_cost; RESET parallel_tuple_cost");
	is($err, '', "$what: parallel query succeeds");
	like($out, qr/Workers Launched: [1-9]/, "$what: in a parallel worker");
	cmp_ok($dt, '<', $bound, "$what: parallel query bounded (${dt}s, limit ${bound}s)");
}
# A pathological pattern written into postgresql.conf by hand: the
# postmaster (no timer: it compiles to completion) accepts it and logs that
# it is too slow; backends that already run agree (their bounded re-check
# runs out of time and accepts it), new ones inherit it, and each disables
# the extractor at its lazy compile, as for any compile over the limit.
{
	my $orig = $node->safe_psql('u8', "SHOW $P.extractors");
	my $val = "regex(pattern='$SLOW', keys=slow), sqlcommenter(position=any, merge=on)";
	my $s = session_open();
	my $logpos = -s $node->logfile;
	(my $conf = $val) =~ s/\\/\\\\/g;
	$conf =~ s/'/''/g;
	$node->append_conf('postgresql.conf', "$P.extractors = '$conf'\n");
	my $t0 = time;
	alter_and_reload("RESET $P.extractors");
	my $dt = time - $t0;
	my $log = substr(slurp_file($node->logfile), $logpos);
	unlike($log, qr/invalid value for parameter "\Q$P\E\.extractors"/,
		'slow pattern in postgresql.conf: not rejected by the postmaster');
	like($log, qr/LOG:  compiling the pattern of extractor "regex" took \d+ ms, longer than the $LIMIT_MS ms limit\n.*DETAIL:  \S/,
		'slow pattern in postgresql.conf: the postmaster logs the compile time');
	my ($slow_ms) = $log =~ /compiling the pattern of extractor "regex" took (\d+) ms/;
	$slow_ms //= 0;
	is($node->safe_psql('u8', "SHOW $P.extractors"), $val, 'slow pattern in postgresql.conf: new backends have it');
	is(sq($s, "SHOW $P.extractors"), $val, 'slow pattern in postgresql.conf: a running backend has it too');
	note "slow pattern in postgresql.conf: reload took ${dt}s";
	# Setting it with a statement is still rejected for time ...
	my ($out, $err) = sq_err($s, "ALTER SYSTEM SET $P.extractors = " . sqlq($val));
	like($err, qr/DETAIL:  Compiling the pattern of extractor "regex" took longer than $LIMIT_MS ms\./,
		'slow pattern in postgresql.conf: ALTER SYSTEM still rejects it for time');
	# ... but pg_file_settings, which checks the file's values with the check
	# hooks while running a statement, reads the configuration file (also
	# right after that failed ALTER SYSTEM in the same backend).
	$t0 = time;
	is(sq($s, "SELECT applied, error IS NULL FROM pg_file_settings WHERE name = '$P.extractors' ORDER BY seqno DESC LIMIT 1"),
		't|t', 'slow pattern in postgresql.conf: pg_file_settings shows it applied');
	my $dt3 = time - $t0;
	cmp_ok($dt3, '<', $BOUND_S, "slow pattern in postgresql.conf: pg_file_settings bounded (${dt3}s)");
	# the worker doesn't compile it: well below one full compile
	parallel_query_ok($s, 'slow pattern in postgresql.conf', $slow_ms / 2000);
	# _extract() does extract, so it must not run in a worker either (where
	# its lazy compile would be unbounded): it is PARALLEL RESTRICTED.
	{
		$node->safe_psql('u8', "CREATE EXTENSION $P");
		my $p = session_open();
		$t0 = time;
		my ($pout, $perr) = sq_err($p,
			"SET $force_parallel = on; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;\n"
			  . 'SELECT count(*) FROM generate_series(1, 10) g WHERE '
			  . "${P}_extract('SELECT 1 /* x */') IS NOT NULL");
		my $pdt = time - $t0;
		is("$perr|$pout", '|10', 'slow pattern in postgresql.conf: _extract() under forced parallelism succeeds');
		cmp_ok($pdt, '<', $slow_ms / 2000,
			"slow pattern in postgresql.conf: _extract() under forced parallelism bounded (${pdt}s)");
		is($node->safe_psql('u8', "SELECT proparallel FROM pg_proc WHERE proname = '${P}_extract'"),
			'r', '_extract() is PARALLEL RESTRICTED');
		session_close($p);
		$node->safe_psql('u8', "DROP EXTENSION $P");
	}
	for my $b ([ 'running', $s ], [ 'new', session_open() ])
	{
		$t0 = time;
		my $r = sex($b->[1], sqlq(q{SELECT 1 /* x */ /*a='x'*/}));
		my $dt2 = time - $t0;
		is("$r->{tags} $r->{regex_fail}", 'a=x 1',
			"slow pattern in postgresql.conf, $b->[0] backend: extractor disabled at its lazy compile, counted");
		cmp_ok($dt2, '<', $BOUND_S, "slow pattern in postgresql.conf, $b->[0] backend: bounded (${dt2}s)");
		session_close($b->[1]);
	}
	alter_and_reload("SET $P.extractors = " . sqlq($orig));
	is($node->safe_psql('u8', "SHOW $P.extractors"), $orig, 'slow pattern in postgresql.conf: overridden again');
}
# A value the postmaster rejects for a reason other than time is rejected
# by a backend whose re-check was stopped at the limit too, if it can tell
# without the compiled regex: more keys (so capture groups) than max_tags.
{
	my $orig = $node->safe_psql('u8', "SHOW $P.extractors");
	my $val = q{regex(pattern='(a)(b)(c)(d)(e)(f)(g)(h)(i)', keys=a|b|c|d|e|f|g|h|i)};
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'sleep', -1)");
	my $logpos = -s $node->logfile;
	(my $conf = $val) =~ s/'/''/g;
	$node->append_conf('postgresql.conf', "$P.extractors = '$conf'\n");
	alter_and_reload("RESET $P.extractors");
	like(substr(slurp_file($node->logfile), $logpos),
		qr/invalid value for parameter "\Q$P\E\.extractors".*\n.*DETAIL:  Pattern of extractor "regex" has 9 capture groups, more than max_tags \(8\)\./,
		'more keys than max_tags in postgresql.conf: rejected by the postmaster');
	cmp_ok(sq($s, 'SELECT pssc_extract_test_regex_injected()'), '>=', 1,
		'more keys than max_tags in postgresql.conf: the backend re-check hit the limit');
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'none')");
	is(sq($s, "SHOW $P.extractors"), $node->safe_psql('u8', "SHOW $P.extractors"),
		'more keys than max_tags in postgresql.conf: the backend agrees with the postmaster');
	session_close($s);
	alter_and_reload("SET $P.extractors = " . sqlq($orig));
}
# A value the postmaster rejects for a reason only the compiled regex shows
# (2 capture groups, 1 key) but a backend accepted unchecked: the backend
# disables it at its lazy compile, and its parallel workers, restoring the
# backend's value, accept it too rather than fail the query.
{
	my $orig = $node->safe_psql('u8', "SHOW $P.extractors");
	my $val = "$orig, regex(pattern='op=(\\w+)(x?)', keys=op2, merge=on)";
	my $what = 'capture groups not matching the keys, accepted unchecked';
	my $s = session_open();
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'sleep', -1)");
	my $logpos = -s $node->logfile;
	(my $conf = $val) =~ s/\\/\\\\/g;
	$conf =~ s/'/''/g;
	$node->append_conf('postgresql.conf', "$P.extractors = '$conf'\n");
	alter_and_reload("RESET $P.extractors");
	like(substr(slurp_file($node->logfile), $logpos),
		qr/invalid value for parameter "\Q$P\E\.extractors".*\n.*DETAIL:  Extractor "regex" has 1 key but its pattern has 2 capture groups\./,
		"$what: rejected by the postmaster");
	cmp_ok(sq($s, 'SELECT pssc_extract_test_regex_injected()'), '>=', 1, "$what: the backend re-check hit the limit");
	sq($s, "SELECT pssc_extract_test_regex_inject('check', -1, 'none')");
	is(sq($s, "SHOW $P.extractors"), $val, "$what: the backend has it");
	parallel_query_ok($s, $what);
	my $r = sex($s, $Q);
	is("$r->{tags} $r->{regex_fail}", 'a=x,operation=o,service=s 1', "$what: disabled at the lazy compile, counted");
	session_close($s);
	alter_and_reload("SET $P.extractors = " . sqlq($orig));
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

# The malloc probe used below (and above) must see malloc'd memory where the
# platform has a source for it: glibc >= 2.33 (mallinfo2) and macOS
# (malloc_zone_statistics). PG14/15's regex engine mallocs directly, so
# without it the leak check below would see nothing there.
SKIP:
{
	my $rt = $^O eq 'linux' ? `getconf GNU_LIBC_VERSION 2>/dev/null` : '';
	my $want = $^O eq 'darwin'
	  || ($rt =~ /^glibc (\d+)\.(\d+)/ && ($1 > 2 || ($1 == 2 && $2 >= 33)));
	skip "no malloc statistics source known for $^O" . ($rt ne '' ? " ($rt)" : ''), 3
	  unless $want;
	my $s = session_open();
	my $mu = sub { sq($s, 'SELECT malloc_used FROM pssc_extract_test_mem()') };
	my $m0 = $mu->();
	isnt($m0, '', "malloc probe: malloc_used available on $^O");
	my $n = 8 * 1024 * 1024;
	sq($s, "SELECT pssc_extract_test_malloc_hold($n)");
	my $m1 = $mu->();
	sq($s, 'SELECT pssc_extract_test_malloc_hold(0)');
	my $m2 = $mu->();
	cmp_ok(($m1 || 0) - ($m0 || 0), '>=', $n, "malloc probe: sees a $n-byte malloc (grew by "
		  . (($m1 || 0) - ($m0 || 0)) . ' bytes)');
	cmp_ok(($m1 || 0) - ($m2 || 0), '>=', $n, 'malloc probe: sees it freed');
	session_close($s);
}

# Reloads alternating two configs of 16 expensive regexes must not grow the
# backend's memory (malloc'd on PG14/15, palloc'd on PG16+). Each pattern
# takes several ms of CPU time to compile; this is not a test of the compile
# time limit, which on an overloaded VM (host steal counts as CPU time: a
# 3 ms compile was charged 54 ms) could make ALTER SYSTEM reject them or the
# session disable them, so it is raised to 60 s for ALTER SYSTEM and the
# session (still a timed compile).
sub big_regex_config
{
	my ($c) = @_;
	return join(', ', map {
		"regex(pattern='$c$_=([a-z0-9_]{1,150})/(\\w{1,150}),(\\d{1,80})', keys=a$_|b$_|c$_, merge=on)"
	} 1 .. 16);
}
{
	my @big = (big_regex_config('p'), big_regex_config('q'));
	local $alter_pre = "SELECT pssc_extract_test_regex_compile_limit(60000);\n";
	my $s = session_open();
	sq($s, 'SELECT pssc_extract_test_regex_compile_limit(60000)');
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
