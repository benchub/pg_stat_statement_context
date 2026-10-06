# Tag value normalization (DESIGN.md §6.11 step 6; backlog
# 20261005-091225-41): pg_stat_statement_context.normalize rewrites the
# values of one key with regex-replace rules after rename and the
# allowlist/denylist and before truncation, so that the normalized value is
# what is recorded (and what the entry's key is built from).
#
# Covers, through real statements and the views: the documented
# postgresql.conf example; rename -> allowlist -> normalize -> truncation
# order; values collapsing into one entry; a rejected value at reload keeps
# the previous rules (logged); the rules' lazy per-backend compilation (once
# per generation, freed on change); failures fail closed (the tag is
# dropped): an injected engine error while replacing, and a compile failure
# (counted in _info().regex_compile_failures, rule disabled for the
# backend); query cancel and statement_timeout are honored while
# normalizing. Fault injection uses the TEST-ONLY module
# test/modules/pssc_extract_test.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('normalize');
$node->init;
# max_tag_value_len = 16 makes truncation visible; the normalize line is
# the docs/configuration.md example (quotes and backslashes doubled in
# postgresql.conf).
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.max_tag_value_len = 16
$P.tags = 'route, action, controller'
$P.exclude_tags = ''
EOC
$node->append_conf('postgresql.conf', <<'EOC');
pg_stat_statement_context.normalize = 'route: ''/users/\\d+'' => ''/users/:id'', route: ''/posts/\\d+'' => ''/posts/:id'''
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

sql("CREATE EXTENSION $P; CREATE EXTENSION pssc_extract_test; CREATE TABLE t(i int)");

is(sql("SHOW $P.normalize"),
	q{route: '/users/\d+' => '/users/:id', route: '/posts/\d+' => '/posts/:id'},
	'postgresql.conf example: setting');

# Tagged rows of the view: "tags calls" lines, sorted.
sub recorded
{
	return sql("SELECT tags::text || ' ' || sum(calls) FROM $P "
		  . "WHERE tags <> '{}' GROUP BY tags::text ORDER BY 1");
}

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

# ---------------------------------------------------------------------------
# Values collapse into one entry
# ---------------------------------------------------------------------------
sql("SELECT ${P}_reset()");
sql(q{SELECT * FROM t /*route:/users/1*/; SELECT * FROM t /*route:/users/22*/; }
	  . q{SELECT * FROM t /*route='%2Fusers%2F333'*/; SELECT * FROM t /*route:/posts/4*/; }
	  . q{SELECT * FROM t /*route:/about*/});
is(recorded(),
	qq{{"route": "/about"} 1\n{"route": "/posts/:id"} 1\n{"route": "/users/:id"} 3},
	'normalized values are recorded; different ids share one entry');

# ---------------------------------------------------------------------------
# Order: rename -> allowlist -> normalize -> truncation (16 bytes)
# ---------------------------------------------------------------------------
config(extractors => 'marginalia(rename=path:route|route:old)',
	tags => 'route, action',
	normalize => q{route: '\d+' => '#', path: '.*' => 'path rule', old: '.*' => 'old rule', }
	  . q{action: '^(/[a-z]+)/\d+' => '\1/#', controller: '.*' => 'disallowed'});
sql("SELECT ${P}_reset()");
sql(q{SELECT * FROM t /*path:/u/1,route:/r/2*/});
is(recorded(), qq{{"route": "/u/#"} 1},
	'rules see the renamed key: path -> route is normalized, route -> old is not kept');
sql("SELECT ${P}_reset()");
sql(q{SELECT * FROM t /*controller:c1,action:/a/1*/});
is(recorded(), qq{{"action": "/a/#"} 1}, 'a rule for a key not allowed does not add it');
sql("SELECT ${P}_reset()");
# /accounts/123456789/settings (28 bytes) -> /accounts/#/settings -> 16 bytes.
# Truncating first would give /accounts/123456 -> /accounts/#.
sql(q{SELECT * FROM t /*action:/accounts/123456789/settings*/});
is(recorded(), qq{{"action": "/accounts/#/sett"} 1}, 'normalization runs before truncation');
sql("SELECT ${P}_reset()");
sql(q{SELECT * FROM t /*path:aaaaaaaaaaaaaaaaaaaaaaa1*/});
is(recorded(), qq{{"route": "aaaaaaaaaaaaaaaa"} 1}, 'and its output is truncated');

# ---------------------------------------------------------------------------
# A rejected value at reload keeps the previous rules
# ---------------------------------------------------------------------------
# Back to the postgresql.conf rules (a value set by ALTER SYSTEM would
# override postgresql.conf, whose later lines are then not even checked).
config(extractors => 'sqlcommenter, marginalia', tags => 'route, action', normalize => undef);
{
	my $log_offset = -s $node->logfile;
	$node->append_conf('postgresql.conf',
		qq{$P.normalize = 'route: ''(a)\\\\1'' => ''x'''\n});
	$node->reload;
	ok($node->wait_for_log(qr/invalid value for parameter "\Q$P\E\.normalize"/, $log_offset),
		'invalid rules in postgresql.conf: reload logs the error');
	is(sql("SHOW $P.normalize"),
		q{route: '/users/\d+' => '/users/:id', route: '/posts/\d+' => '/posts/:id'},
		'the previous rules stay');
	sql("SELECT ${P}_reset()");
	sql(q{SELECT * FROM t /*route:/users/12*/});
	is(recorded(), qq{{"route": "/users/:id"} 1}, 'and still apply');
	$node->append_conf('postgresql.conf', "$P.normalize = ''\n");
	my ($ret, $out, $err) = $node->psql('postgres',
		"ALTER SYSTEM SET $P.normalize = 'route: ''(a)\\1'' => ''x'''");
	isnt($ret, 0, 'ALTER SYSTEM rejects invalid rules');
	like($err, qr/invalid value for parameter "\Q$P\E\.normalize".*\n.*back-reference/i,
		'with a detail');
}

# ---------------------------------------------------------------------------
# Lazy per-backend compilation, freed when the rules change
# ---------------------------------------------------------------------------
config(normalize => q{route: '\d+' => 'N', route: 'N' => 'M', action: 'x' => 'y'});
{
	my $stats = 'SELECT compiles, frees, live, failed FROM pssc_extract_test_regex_stats()';
	is( sql("$stats; SELECT * FROM t /*route:/r/1*/; $stats; SELECT * FROM t /*route:/r/2*/; $stats"),
		"0|0|0|0\n2|0|2|0\n2|0|2|0",
		'the rules of a key compile on first use, once per backend');
}

# A persistent session, to observe a reload from the same backend.
my %s = (in => '', out => '', err => '');
$s{h} = IPC::Run::start([ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d', $node->connstr('postgres') ],
	'<', \$s{in}, '>', \$s{out}, '2>', \$s{err}, IPC::Run::timeout(600));
my $marker = 0;
sub sq_err
{
	my ($q) = @_;
	my $m = '__pssc_done_' . ++$marker . '__';
	$s{out} = '';
	$s{err} = '';
	$s{in} .= "$q;\n\\echo $m\n";
	$s{h}->pump until $s{out} =~ /^\Q$m\E$/m;
	(my $r = $s{out}) =~ s/^\Q$m\E\n\z//m;
	chomp $r;
	return ($r, $s{err});
}
sub sq
{
	my ($r, $err) = sq_err($_[0]);
	die "session error for <$_[0]>: $err" if $err ne '';
	return $r;
}
sub wait_session
{
	my $tries = 0;
	until (sq("SELECT setting FROM pg_settings WHERE name = '$P.scan_window'") eq $sentinel)
	{
		die "session did not process reload" if ++$tries > 1800;
		usleep(100_000);
	}
}
{
	my $stats = 'SELECT compiles, frees, live, failed FROM pssc_extract_test_regex_stats()';
	sq('SELECT * FROM t /*route:/r/1,action:x*/');
	is(sq($stats), '3|0|3|0', 'session: all three rules compiled');
	config(normalize => q{route: '\d+' => 'Z'});
	wait_session();
	sq("SELECT ${P}_reset()");
	sq('SELECT * FROM t /*route:/r/1*/');
	is(sq($stats), '4|3|1|0', 'after a change: the old rules are freed, the new one compiled');
	is(recorded(), qq{{"route": "/r/Z"} 1}, 'and applied');
}

# ---------------------------------------------------------------------------
# Failures: the tag is dropped (fail closed), the statement runs
# ---------------------------------------------------------------------------
config(normalize => q{route: '\d+' => 'N', action: '\d+' => 'A'});
wait_session();
for my $c ([ 'espace', 'norm_exec' ], [ 'error', 'norm_exec' ], [ 'oom', 'norm_exec' ])
{
	my ($action, $phase) = @$c;
	sq("SELECT ${P}_reset()");
	sq("SELECT pssc_extract_test_regex_inject('$phase', 0, '$action', 1)");
	is(sq('SELECT count(*) FROM t /*route:/r/1,action:/a/1*/'), '0',
		"replace failure ($action): the statement runs");
	is(sq('SELECT pssc_extract_test_regex_injected()'), 1, "replace failure ($action): injected once");
	is(recorded(), qq{{"action": "/a/A"} 1},
		"replace failure ($action): the tag of that key is dropped, others kept");
	is(sq(q{SELECT pg_stat_statement_context_extract('SELECT 1 /*route:/r/2*/')->>'normalize_failures'}),
		'0', "replace failure ($action): the next statement normalizes again");
}
sq("SELECT pssc_extract_test_regex_inject('norm_exec', 0, 'espace', 1)");
is( sq(q{SELECT pg_stat_statement_context_extract('SELECT 1 /*route:/r/1,action:/a/1*/') }
		  . q{- 'stmt_start' - 'stmt_end' - 'tagset_bytes'}),
	'{"oom": false, "tags": {"action": "/a/A"}, "ntags": 1, "footer": false, "heuristic": false, '
	  . '"capped_tags": 0, "dropped_tags": 0, "invalid_tags": 0, "heuristic_scans": 0, "normalized_tags": 1, '
	  . '"normalize_failures": 1, "regex_compile_failures": 0}',
	'the extract function reports normalize_failures');

# A compile failure disables the rule for the backend (until the rules
# change), counted once in regex_compile_failures; tags of its key are
# dropped.
sq("SELECT ${P}_reset()");
my $gen = 0;
for my $phase ('norm_compile', 'norm_context')
{
	# a new value each time: a new generation, compiled afresh
	$gen++;
	config(normalize => qq{route: '\\d+' => 'N', action: '\\d+' => 'A', job: '$gen' => ''});
	wait_session();
	sq("SELECT ${P}_reset()");
	sq("SELECT pssc_extract_test_regex_inject('$phase', 0, 'oom', -1)");
	sq('SELECT * FROM t /*route:/r/1,action:/a/1*/');
	sq('SELECT * FROM t /*route:/r/2,action:/a/2*/');
	is(sq('SELECT pssc_extract_test_regex_injected()'), 1, "$phase failure: tried once");
	is(recorded(), qq{{"action": "/a/A"} 2}, "$phase failure: tags of that key dropped");
	is(sql("SELECT regex_compile_failures FROM ${P}_info()"), 1,
		"$phase failure: counted once in _info().regex_compile_failures");
	sq("SELECT pssc_extract_test_regex_inject('norm_exec', -1, 'none')");
}

# Interrupts are honored while normalizing.
for my $c ([ 'norm_exec', 'cancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'norm_exec', 'regcancel', 'SELECT 1', qr/canceling statement due to user request/ ],
	[ 'norm_exec', 'sleep', "SET statement_timeout = '300ms'", qr/canceling statement due to statement timeout/ ],
	[ 'norm_compile', 'cancel', 'SELECT 1', qr/canceling statement due to user request/ ])
{
	my ($phase, $action, $pre, $re) = @$c;
	$gen++;
	config(normalize => qq{route: '\\d+' => 'N', job: '$gen' => ''});
	wait_session();
	sq($pre);
	sq("SELECT pssc_extract_test_regex_inject('$phase', 0, '$action', 1)");
	my ($r, $err) = sq_err('SELECT * FROM t /*route:/r/1*/');
	like($err, $re, "$phase interrupted ($action): the statement is canceled");
	sq('RESET statement_timeout');
	sq("SELECT ${P}_reset()");
	sq('SELECT * FROM t /*route:/r/1*/');
	is(recorded(), qq{{"route": "/r/N"} 1}, "$phase interrupted ($action): the next statement works");
}

$s{in} .= "\\q\n";
$s{h}->finish;
$node->stop;
done_testing();
