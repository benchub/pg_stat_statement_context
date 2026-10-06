# appname(format=...) extractors (DESIGN.md §4.2; backlog
# 20261005-091225-38): tags derived from application_name, for clients that
# cannot add comments, recorded through real statements.
#
# Covers: a driver-like client that only sets application_name (no comment
# at all), simple and extended protocol (pgbench -M extended / prepared),
# SQL PREPARE/EXECUTE (the value at execution time is used), a comment tag
# winning a key conflict, SET application_name in the middle of a session,
# untagged = skip (a non-tagging application_name is not recorded, a tagging
# one is), and nested statements (inherit uses the outer statement's tags,
# scan reads application_name again).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('appname');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = ''
$P.extractors = 'sqlcommenter, marginalia, appname(format=sqlcommenter)'
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

sql("CREATE EXTENSION $P; CREATE TABLE t_app(i int); CREATE TABLE t_nest(i int)");
sql(q{CREATE FUNCTION f_set() RETURNS int LANGUAGE plpgsql AS $$
DECLARE n int;
BEGIN
  PERFORM set_config('application_name', 'controller=''inner''', false);
  SELECT count(*) INTO n FROM t_nest;
  RETURN n;
END $$});

# Tagged rows of the view: "tags calls" lines, sorted.
sub recorded
{
	return sql("SELECT tags::text || ' ' || sum(calls) FROM $P "
		  . "WHERE tags <> '{}' GROUP BY tags::text ORDER BY 1");
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

# Run psql on $script (stdin) as a client whose application_name is $app,
# with utility statements not tracked (so only the statements under test
# are recorded) plus any extra -c options.
sub as_app
{
	my ($app, $script, @opts) = @_;
	my ($out, $err) = ('', '');
	local $ENV{PGAPPNAME} = $app;
	local $ENV{PGOPTIONS} = join(' ', "-c $P.track_utility=off", map { "-c $_" } @opts);
	IPC::Run::run(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres') ],
		'<', \$script, '>', \$out, '2>', \$err, IPC::Run::timeout(180))
	  or die "psql failed: $err";
	chomp $out;
	return $out;
}

my $APP = q{controller='billing',action='charge'};

# ---------------------------------------------------------------------------
# A driver that can only set application_name: no comment at all
# ---------------------------------------------------------------------------
reset_all();
is(as_app($APP, "SHOW application_name;\n"), $APP, 'client application_name as set');
reset_all();
as_app($APP, "SELECT count(*) FROM t_app WHERE i > 0;\n" x 3);
is(recorded(), q{{"action": "charge", "controller": "billing"} 3},
	'no comment: recorded with the tags of application_name');

# A comment tag wins a key conflict; other keys are merged.
reset_all();
as_app($APP, "SELECT count(*) FROM t_app WHERE i > 0 /*controller='web'*/;\n");
is(recorded(), q{{"action": "charge", "controller": "web"} 1},
	'conflict: the comment value is stored, application_name supplies the rest');

# A plain application_name tags nothing.
reset_all();
as_app('psql', "SELECT count(*) FROM t_app;\n");
is(recorded(), '', 'plain application_name: untagged');

# ---------------------------------------------------------------------------
# Extended protocol
# ---------------------------------------------------------------------------
{
	my $script = $node->basedir . '/pgbench_app.sql';
	open my $fh, '>', $script or die $!;
	print $fh "SELECT count(*) FROM t_app WHERE i > 0;\n";
	close $fh;
	for my $mode ('extended', 'prepared')
	{
		reset_all();
		my ($stdout, $stderr);
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		local $ENV{PGAPPNAME} = q{controller='pgb'};
		IPC::Run::run(
			[ 'pgbench', '-n', '-M', $mode, '-t', '5', '-f', $script, 'postgres' ],
			'>', \$stdout, '2>', \$stderr)
		  or die "pgbench failed: $stderr";
		like($stdout, qr{number of transactions actually processed: 5/5},
			"pgbench -M $mode ran");
		is(recorded(), q{{"controller": "pgb"} 5},
			"pgbench -M $mode: recorded with the tags of application_name");
	}
}

# SQL PREPARE / EXECUTE: application_name at execution, not at PREPARE.
reset_all();
as_app(q{controller='early'}, <<'EOS');
PREPARE p AS SELECT count(*) FROM t_app WHERE i > 0;
SET application_name = 'controller=''late''';
EXECUTE p;
EXECUTE p;
EOS
is(recorded(), q{{"controller": "late"} 2},
	'PREPARE/EXECUTE: the application_name at EXECUTE time');

# SET application_name in the middle of a session.
reset_all();
as_app(q{controller='a1'}, <<'EOS');
SELECT count(*) FROM t_app;
SET application_name = 'controller=''a2''';
SELECT count(*) FROM t_app;
SELECT count(*) FROM t_app;
EOS
is(recorded(), qq{{"controller": "a1"} 1\n{"controller": "a2"} 2},
	'SET application_name: later statements use the new value');

# ---------------------------------------------------------------------------
# Nested statements
# ---------------------------------------------------------------------------
reset_all();
as_app(q{controller='outer'}, "SELECT f_set();\n", "$P.track=all",
	"$P.nested_tags=inherit");
my $got = recorded();
like($got, qr/^\{"controller": "outer"\} \d+$/,
	'nested_tags = inherit: nested statements keep the outer tags');
reset_all();
as_app(q{controller='outer'}, "SELECT f_set();\n", "$P.track=all",
	"$P.nested_tags=scan");
$got = recorded();
like($got, qr/^\{"controller": "inner"\} 1$/m,
	'nested_tags = scan: a nested statement reads application_name again');
like($got, qr/^\{"controller": "outer"\} \d+$/m,
	'nested_tags = scan: the outer statement keeps its own');

# ---------------------------------------------------------------------------
# untagged = skip
# ---------------------------------------------------------------------------
config(untagged => 'skip');
reset_all();
as_app('psql', "SELECT count(*) FROM t_app;\n");
as_app($APP, "SELECT count(*) FROM t_app;\n");
is(sql("SELECT count(*) FROM $P WHERE tags = '{}'"), '0',
	'untagged = skip: nothing untagged recorded');
is(recorded(), q{{"action": "charge", "controller": "billing"} 1},
	'untagged = skip: application_name tags make a statement tagged');
config(untagged => undef);

# ---------------------------------------------------------------------------
# Comment extractors only: application_name is ignored
# ---------------------------------------------------------------------------
config(extractors => 'sqlcommenter, marginalia');
reset_all();
as_app($APP, "SELECT count(*) FROM t_app;\n");
is(recorded(), '', 'without an appname extractor application_name is ignored');
config(extractors => undef);

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
