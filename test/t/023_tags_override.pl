# pg_stat_statement_context.tags_override (backlog item
# 20261005-091225-30): session/transaction tags set with SET or SET LOCAL,
# for prepared statements and drivers that cannot add comments, recorded
# through real statements.
#
# Covers: SET LOCAL (the override applies inside the transaction, not after
# COMMIT), a session SET, the connection-time value (PGOPTIONS), SQL
# PREPARE/EXECUTE and extended protocol (Parse/Bind/Execute over the wire)
# prepared before the SET, the union with comment tags and the override
# winning a conflict, untagged = skip (override-only statements are tagged),
# a value rejected at SET time, and nested statements (inherit keeps the
# outer statement's tags, scan reads the override again, including a
# function's SET clause and a SET LOCAL inside a function).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);

my $P = 'pg_stat_statement_context';
my $G = "$P.tags_override";

my $node = PostgreSQL::Test::Cluster->new('tags_override');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = ''
$P.extractors = 'sqlcommenter, marginalia'
$P.untagged = 'record'
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

sql("CREATE EXTENSION $P; CREATE TABLE t_ov(i int); CREATE TABLE t_nest(i int)");
sql("CREATE ROLE wire LOGIN; GRANT SELECT ON t_ov TO wire");
sql("ALTER ROLE wire SET $P.track_utility = off");

# Tagged rows of the view: "tags calls" lines, sorted.
sub recorded
{
	return sql("SELECT tags::text || ' ' || sum(calls) FROM $P "
		  . "WHERE tags <> '{}' GROUP BY tags::text ORDER BY 1");
}
# Untagged calls of the statement under test ($Q, see $QID below; the
# helpers' own queries are tracked too).
my $QID;
sub untagged
{
	return sql("SELECT coalesce(sum(calls), 0) FROM $P "
		  . "WHERE tags = '{}' AND queryid = $QID");
}

sub reset_all { sql("SELECT ${P}_reset()"); }

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

# Run psql on $script (stdin) with utility statements not tracked (so only
# the statements under test are recorded) plus any extra -c options.
# Returns stdout; an error is a test failure.
sub run_psql
{
	my ($script, @opts) = @_;
	my ($out, $err) = ('', '');
	local $ENV{PGOPTIONS} = join(' ', "-c $P.track_utility=off", map { "-c $_" } @opts);
	IPC::Run::run(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres') ],
		'<', \$script, '>', \$out, '2>', \$err, IPC::Run::timeout(180))
	  or fail("psql failed: $err");
	chomp $out;
	return $out;
}

my $Q = "SELECT count(*) FROM t_ov WHERE i > 0";

# The queryid of $Q (comments do not change it).
reset_all();
run_psql("$Q /*qid:x*/;\n");
$QID = sql("SELECT queryid FROM $P WHERE tags = '{\"qid\": \"x\"}'");
like($QID, qr/^-?\d+$/, 'queryid of the statement under test');

# ---------------------------------------------------------------------------
# SET LOCAL: inside the transaction only
# ---------------------------------------------------------------------------
reset_all();
run_psql(<<"EOS");
BEGIN;
SET LOCAL $G = 'controller=''users'',action=''show''';
$Q;
$Q;
COMMIT;
$Q;
EOS
is(recorded(), q{{"action": "show", "controller": "users"} 2},
	'SET LOCAL: statements in the transaction get the override tags');
is(untagged(), '1', 'SET LOCAL: the override no longer applies after COMMIT');

reset_all();
run_psql(<<"EOS");
BEGIN;
SET LOCAL $G = 'controller=''rb''';
$Q;
ROLLBACK;
$Q;
EOS
is(recorded(), q{{"controller": "rb"} 1}, 'SET LOCAL then ROLLBACK: recorded inside');
is(untagged(), '1', 'SET LOCAL then ROLLBACK: gone after');

# Session SET, then RESET.
reset_all();
run_psql(<<"EOS");
SET $G = 'controller=''sess''';
$Q;
$Q;
RESET $G;
$Q;
EOS
is(recorded(), q{{"controller": "sess"} 2}, 'SET: later statements use the override');
is(untagged(), '1', 'RESET: later statements are untagged');

# Set at connection start (a driver's options / PGOPTIONS).
reset_all();
run_psql("$Q;\n", "$G=controller='opt'");
is(recorded(), q{{"controller": "opt"} 1}, 'value set at connection start');

# ---------------------------------------------------------------------------
# Merge with comment tags
# ---------------------------------------------------------------------------
reset_all();
run_psql(<<"EOS");
SET $G = 'controller=''ovr'',job=''j''';
$Q /*action='a'*/;
$Q /*controller='cmt',action='b'*/;
$Q /*controller:mg*/;
EOS
is( recorded(),
	join("\n",
		q{{"job": "j", "action": "a", "controller": "ovr"} 1},
		q{{"job": "j", "action": "b", "controller": "ovr"} 1},
		q{{"job": "j", "controller": "ovr"} 1}),
	'disjoint keys: the union; a shared key: the override value');

# ---------------------------------------------------------------------------
# Prepared statements: the override at execution time
# ---------------------------------------------------------------------------
reset_all();
run_psql(<<"EOS");
PREPARE p AS $Q;
EXECUTE p;
SET $G = 'controller=''late''';
EXECUTE p;
EXECUTE p;
BEGIN;
SET LOCAL $G = 'controller=''txn''';
EXECUTE p;
COMMIT;
EXECUTE p;
EOS
is( recorded(),
	join("\n", q{{"controller": "late"} 3}, q{{"controller": "txn"} 1}),
	'PREPARE/EXECUTE: the override at EXECUTE time');
is(untagged(), '1', 'PREPARE/EXECUTE: before the SET, untagged');

# ---------------------------------------------------------------------------
# Extended protocol, raw messages (copied from 022_activity.pl): a statement
# parsed before the SET, bound and executed after.
# ---------------------------------------------------------------------------
sub pmsg
{
	my ($type, $body) = @_;
	return $type . pack('N', length($body) + 4) . $body;
}
sub p_parse   { return pmsg('P', "$_[0]\0$_[1]\0" . pack('n', 0)); }
sub p_bind    { return pmsg('B', "$_[0]\0$_[1]\0" . pack('nnn', 0, 0, 0)); }
sub p_execute { return pmsg('E', "$_[0]\0" . pack('N', 0)); }
sub p_sync    { return pmsg('S', ''); }
sub p_query   { return pmsg('Q', "$_[0]\0"); }

sub p_readn
{
	my ($c, $n) = @_;
	my $buf = '';
	while (length($buf) < $n)
	{
		my $got = sysread($c->{sock}, $buf, $n - length($buf), length($buf));
		die "protocol read: " . (defined $got ? 'EOF' : $!) unless $got;
	}
	return $buf;
}

# Send messages, read until every ReadyForQuery they produce (one per Query
# or Sync message; one for the startup packet); returns the error messages.
sub p_run
{
	my ($c, @msgs) = @_;
	my $out = join('', @msgs);
	my $expect = grep { /^[QS]/ } @msgs;
	$expect = 1 if $expect == 0;
	local $SIG{ALRM} = sub { die "protocol timeout\n" };
	alarm($PostgreSQL::Test::Utils::timeout_default);
	while (length($out) > 0)
	{
		my $n = syswrite($c->{sock}, $out);
		die "protocol write: $!" unless defined $n;
		substr($out, 0, $n) = '';
	}
	my $err = '';
	while (1)
	{
		my ($type, $len) = unpack('a N', p_readn($c, 5));
		my $body = $len > 4 ? p_readn($c, $len - 4) : '';
		$err .= ($body =~ /(?:^|\0)M([^\0]*)/)[0] . "\n" if $type eq 'E';
		last if $type eq 'Z' && --$expect == 0;
	}
	alarm(0);
	return $err;
}

sub p_open
{
	my ($user) = @_;
	my $path = $node->host . '/.s.PGSQL.' . $node->port;
	my $sock = IO::Socket::UNIX->new(Type => SOCK_STREAM(), Peer => $path)
	  or die "connect $path: $!";
	my $c = { sock => $sock };
	my $body = pack('N', 196608) . "user\0$user\0database\0postgres\0\0";
	my $err = p_run($c, pack('N', length($body) + 4) . $body);
	die "startup: $err" if $err ne '';
	return $c;
}

sub p_ok
{
	my ($c, $what, @msgs) = @_;
	my $err = p_run($c, @msgs);
	fail("$what: $err") if $err ne '';
}

{
	my @exec = (p_bind('', 'ps'), p_execute(''), p_sync());
	reset_all();
	my $c = p_open('wire');
	p_ok($c, 'parse', p_parse('ps', $Q), p_sync());
	p_ok($c, 'exec before', @exec);
	p_ok($c, 'set', p_query("SET $G = 'controller=''wire'''"));
	p_ok($c, 'exec after', @exec);
	p_ok($c, 'exec after', @exec);
	p_ok($c, 'begin', p_query("BEGIN; SET LOCAL $G = 'controller=''wtxn'''"));
	p_ok($c, 'exec in txn', @exec);
	p_ok($c, 'commit', p_query('COMMIT'));
	p_ok($c, 'exec after commit', @exec);
	# SET sent with the extended protocol too
	p_ok($c, 'set extended',
		p_parse('', "SET $G = 'controller=''wext'''"), p_bind('', ''),
		p_execute(''), p_sync());
	p_ok($c, 'exec after extended set', @exec);
	# a rejected value leaves the override as it was
	my $err = p_run($c, p_query("SET $G = 'controller=wbad'"));
	like($err, qr/invalid value for parameter "\Q$G\E"/,
		'extended protocol session: a malformed value is rejected');
	p_ok($c, 'exec after rejected set', @exec);
	syswrite($c->{sock}, pmsg('X', ''));
	close($c->{sock});
	is( recorded(),
		join("\n",
			q{{"controller": "wext"} 2},
			q{{"controller": "wire"} 3},
			q{{"controller": "wtxn"} 1}),
		'extended protocol: a statement parsed before the SET uses the override at Bind/Execute');
	is(untagged(), '1', 'extended protocol: before the SET, untagged');
}

# ---------------------------------------------------------------------------
# Rejected at SET time
# ---------------------------------------------------------------------------
{
	my ($ret, $out, $err) = $node->psql('postgres', "SET $G = 'a=''%zz'''");
	isnt($ret, 0, 'a bad % escape is rejected');
	like($err, qr/invalid value for parameter "\Q$G\E"/, 'error names the parameter');
}

# ---------------------------------------------------------------------------
# untagged = skip
# ---------------------------------------------------------------------------
config(untagged => 'skip');
reset_all();
run_psql(<<"EOS");
$Q;
SET $G = 'controller=''only''';
$Q;
EOS
is(untagged(), '0', 'untagged = skip: nothing untagged recorded');
is(recorded(), q{{"controller": "only"} 1},
	'untagged = skip: override-only statements are tagged');
config(untagged => undef);

# ---------------------------------------------------------------------------
# Nested statements
# ---------------------------------------------------------------------------
sql(qq{CREATE FUNCTION f_cfg() RETURNS bigint LANGUAGE sql
SET $G = 'controller=''fn'''
AS \$\$ SELECT count(*) FROM t_nest \$\$});
sql(qq{CREATE FUNCTION f_local() RETURNS bigint LANGUAGE plpgsql AS \$\$
DECLARE n bigint;
BEGIN
  PERFORM set_config('$G', 'controller=''local''', true);
  SELECT count(*) INTO n FROM t_nest;
  RETURN n;
END \$\$});
for my $mode ('inherit', 'scan')
{
	reset_all();
	run_psql("SET $G = 'controller=''outer''';\nSELECT f_cfg();\n",
		"$P.track=all", "$P.nested_tags=$mode");
	my $got = recorded();
	if ($mode eq 'inherit')
	{
		like($got, qr/^\{"controller": "outer"\} \d+$/,
			'inherit: nested statements keep the outer tags (function SET clause ignored)');
	}
	else
	{
		like($got, qr/^\{"controller": "fn"\} 1$/m,
			'scan: a nested statement uses the function SET clause');
		like($got, qr/^\{"controller": "outer"\} 1$/m,
			'scan: the outer statement keeps the value at its start');
	}
}

# SET LOCAL inside a function: nested statements under scan, and later
# top-level statements of the transaction.
reset_all();
run_psql(<<"EOS", "$P.track=all", "$P.nested_tags=scan");
SET $G = 'controller=''outer''';
BEGIN;
SELECT f_local();
$Q;
COMMIT;
$Q;
EOS
my $got = recorded();
like($got, qr/^\{"controller": "local"\} 2$/m,
	'SET LOCAL in a function: the nested statement after it and the next top-level statement');
like($got, qr/^\{"controller": "outer"\} \d+$/m,
	'SET LOCAL in a function: the calling statement and after COMMIT keep the session value');
is(sql("SELECT sum(calls) FROM $P WHERE tags = '{\"controller\": \"outer\"}' "
	  . "AND toplevel"), '2', 'SET LOCAL in a function: two outer top-level statements');

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
