# Core GUCs (DESIGN.md §4.1, backlog 20261005-091225-7): ALTER SYSTEM +
# reload for sighup GUCs, restart for postmaster GUCs (pending_restart),
# invalid values rejected with the previous value kept (reload and startup),
# bounds with units, the parsed tags/exclude_tags lists, the backend-local
# config generation, and prefix reservation. The parsed state is read
# through the TEST-ONLY module test/modules/pssc_guc_test
# (make install-test-modules).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('guc');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
# Placeholder under the reserved prefix, present before the module loads.
$P.no_such_setting = 'x'
});
$node->start;

my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
note "server_version_num = $vnum";

$node->safe_psql('postgres', 'CREATE EXTENSION pssc_guc_test');

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub show { return sql("SHOW $P.$_[0]"); }
sub vars { return sql('SELECT pssc_guc_test_vars()'); }
sub var
{
	my ($name) = @_;
	my ($line) = grep { /^\Q$name\E=/ } split /\n/, vars();
	die "no var $name" unless defined $line;
	return substr($line, length($name) + 1);
}
sub list { return sql("SELECT match_all, nkeys, keys FROM pssc_guc_test_list('$_[0]')"); }
sub find { return sql("SELECT pssc_guc_test_find('$_[0]', \$k\$$_[1]\$k\$)"); }
sub gen { return sql('SELECT pssc_guc_test_generation()'); }

# A persistent psql session (one backend for its whole life), built on
# IPC::Run directly: PostgreSQL::Test::BackgroundPsql is missing from some
# supported releases (e.g. 15.0) and PG14's background_psql differs.
my @sessions;
my $session_marker = 0;
sub session_open
{
	my %s = (in => '', out => '', err => '');
	$s{h} = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d', $node->connstr('postgres') ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err},
		IPC::Run::timeout(180));
	push @sessions, \%s;
	return \%s;
}
# Run one statement in the session; dies on any error output.
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

# ALTER SYSTEM statements, then reload, then wait until the reload has been
# processed: each call also moves scan_window (which does not affect the
# config generation) to a new sentinel value and polls for it.
my $sentinel = 1000;
sub alter_and_reload
{
	$sentinel++;
	sql(join('', map { "ALTER SYSTEM $_;\n" } @_)
		  . "ALTER SYSTEM SET $P.scan_window = $sentinel;");
	$node->reload;
	$node->poll_query_until('postgres',
		"SELECT setting::int = $sentinel FROM pg_settings WHERE name = '$P.scan_window'")
	  or die "reload not processed (sentinel $sentinel)";
	# Open sessions process SIGHUP before their next command; wait for it.
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

# ALTER SYSTEM on a value: returns stderr ('' on success).
sub alter_err
{
	my ($ret, $out, $err) = $node->psql('postgres', "ALTER SYSTEM $_[0]");
	return $ret == 0 ? '' : $err;
}

# ---------------------------------------------------------------------------
# Defaults (C side and pg_settings)
# ---------------------------------------------------------------------------
is( vars(), join("\n",
		'enabled=on', 'track=top', 'track_utility=on', 'nested_tags=inherit',
		'max_entries=10000', 'bucket_count=12', 'bucket_interval=300',
		'max_tags=8', 'max_tag_value_len=64', 'max_tagset_bytes=512',
		'scan_window=2048', 'extractors=sqlcommenter, marginalia',
		'tags=action, controller, job',
		'exclude_tags=traceparent, tracestate, request_id', 'untagged=skip',
		'normalize=', 'tags_override='),
	'C variables hold the §4.1 defaults (enums map to the right constants)');
is( sql(qq{SELECT count(*), count(*) FILTER (WHERE source = 'default' AND setting = boot_val AND NOT pending_restart)
              FROM pg_settings WHERE name LIKE '$P.%'}),
	'23|23', 'all 23 GUCs are defined and at their defaults');
is(show('bucket_interval'), '5min', 'bucket_interval default shown with its unit');
is(show('scan_window'), '2kB', 'scan_window default shown with its unit');
is(list('tags'), 'f|3|{action,controller,job}', 'default tags parsed');
is(list('exclude_tags'), 'f|3|{traceparent,tracestate,request_id}',
	'default exclude_tags parsed');
is(find('tags', 'controller'), '1', 'find: position in allowlist order');
is(find('tags', 'Controller'), '-1', 'find: keys are case-sensitive');
is(find('tags', 'control'), '-1', 'find: no prefix match');
is(find('tags', 'controllerx'), '-1', 'find: no extension match');
is(find('exclude_tags', 'request_id'), '2', 'find in exclude_tags');

# ---------------------------------------------------------------------------
# Superuser GUCs: SET reaches the C variables, enum values map correctly
# ---------------------------------------------------------------------------
like(sql(qq{SET $P.enabled = off; SET $P.track = 'all'; SET $P.track_utility = off;
           SET $P.nested_tags = 'scan'; SELECT pssc_guc_test_vars()}),
	qr/^enabled=off\ntrack=all\ntrack_utility=off\nnested_tags=scan\n/,
	'SET of superuser GUCs updates the C variables');
like(sql(qq{SET $P.track = 'none'; SET $P.nested_tags = 'none'; SELECT pssc_guc_test_vars()}),
	qr/^enabled=on\ntrack=none\ntrack_utility=on\nnested_tags=none\n/,
	'track = none and nested_tags = none map correctly');
like(sql(qq{BEGIN; SET LOCAL $P.track = 'all'; ROLLBACK; SELECT pssc_guc_test_vars()}),
	qr/\ntrack=top\n/, 'SET LOCAL is undone on rollback');

# ---------------------------------------------------------------------------
# sighup GUCs: ALTER SYSTEM + reload
# ---------------------------------------------------------------------------
my $g0 = gen();
alter_and_reload(
	"SET $P.extractors = 'marginalia(position=append)'",
	"SET $P.tags = ' b ,a, b,  c ,a'",
	"SET $P.exclude_tags = ''",
	"SET $P.untagged = 'record'");
is(show('extractors'), 'marginalia(position=append)', 'reload: extractors changed');
is(show('tags'), ' b ,a, b,  c ,a', 'reload: tags changed (shown as written)');
is(show('exclude_tags'), '', 'reload: exclude_tags changed');
is(show('untagged'), 'record', 'reload: untagged changed');
is(var('untagged'), 'record', 'reload: untagged C variable is record');
is(var('extractors'), 'marginalia(position=append)', 'reload: extractors C variable');
is(list('tags'), 'f|3|{b,a,c}',
	'tags: whitespace trimmed, duplicates dropped, list order kept');
is(find('tags', 'c'), '2', 'tags: find after dedup');
is(find('tags', ' a'), '-1', 'tags: untrimmed key not found');
is(list('exclude_tags'), 'f|0|{}', 'exclude_tags: empty list');
cmp_ok(gen(), '>', $g0, 'config generation bumped by the change');
is( sql(qq{SELECT count(*) FROM pg_settings WHERE name LIKE '$P.%' AND pending_restart}),
	'0', 'no restart pending for sighup changes');

# scan_window takes units.
sql("ALTER SYSTEM SET $P.scan_window = '4kB'");
$node->reload;
$node->poll_query_until('postgres',
	"SELECT setting = '4096' FROM pg_settings WHERE name = '$P.scan_window'")
  or die 'scan_window reload';
is(show('scan_window'), '4kB', 'reload: scan_window = 4kB');
is(var('scan_window'), '4096', 'scan_window C variable is in bytes');

# A reload that changes nothing relevant leaves the generation alone.
my $g1 = gen();
alter_and_reload("SET $P.tags = ' b ,a, b,  c ,a'");
is(gen(), $g1, 'unchanged tags/exclude_tags/extractors: generation not bumped');
alter_and_reload("SET $P.tags = 'b,a,c'");
is(gen(), $g1, 'same parsed tag list, different spelling: generation not bumped');
alter_and_reload("SET $P.extractors = 'sqlcommenter'");
cmp_ok(gen(), '>', $g1, 'extractors change bumps the generation');
my $g2 = gen();
alter_and_reload("SET $P.exclude_tags = 'x'");
cmp_ok(gen(), '>', $g2, 'exclude_tags change bumps the generation');

# Superuser GUCs can also be changed with ALTER SYSTEM + reload.
alter_and_reload("SET $P.enabled = off", "SET $P.track = 'all'");
is(show('enabled') . '/' . show('track'), 'off/all',
	'ALTER SYSTEM + reload changes superuser GUCs');
alter_and_reload("RESET $P.enabled", "RESET $P.track");

# '*' handling.
alter_and_reload("SET $P.tags = ' * '", "SET $P.exclude_tags = 'traceparent , x,traceparent'");
is(list('tags'), 't|0|{}', "tags = ' * ': match all, no keys");
is(find('tags', '*'), '-1', "'*' is not stored as a key");
is(list('exclude_tags'), 'f|2|{traceparent,x}', 'exclude_tags parsed with tags = *');
alter_and_reload("SET $P.tags = '*'");
is(list('tags'), 't|0|{}', "tags = '*'");

# Empty and whitespace-only allowlists keep nothing.
alter_and_reload("SET $P.tags = ''");
is(list('tags'), 'f|0|{}', "tags = '': empty, not match-all");
alter_and_reload("SET $P.tags = ' \t '");
is(list('tags'), 'f|0|{}', 'whitespace-only tags: empty');

# Key length: 63 bytes is the maximum.
my $k63 = 'k' x 63;
alter_and_reload("SET $P.tags = 'a, $k63'");
is(list('tags'), "f|2|{a,$k63}", '63-byte key accepted');
like(alter_err("SET $P.tags = 'a, ${k63}k'"),
	qr/invalid value for parameter "$P\.tags".*\n.*DETAIL:.*longer than 63 bytes/,
	'64-byte key rejected with a clear error');
# Multibyte keys: 21 x 3-byte characters = 63 bytes is fine, 64 bytes is not.
my $mb63 = "\x{20AC}" x 21;
my $mb64 = $mb63 . 'x';
utf8::encode($mb63);
utf8::encode($mb64);
alter_and_reload("SET $P.tags = '$mb63'");
is(sql("SELECT nkeys, octet_length(keys[1]) FROM pssc_guc_test_list('tags')"),
	'1|63', '63-byte multibyte key accepted');
like(alter_err("SET $P.tags = '$mb64'"), qr/longer than 63 bytes/,
	'64-byte multibyte key rejected');

# List size cap: at most 1024 entries (duplicates count), so the parsed blob
# stays small and its size computation cannot overflow.
my $k1024 = join(',', map { sprintf('%063d', $_) } 1 .. 1024);
alter_and_reload("SET $P.tags = '$k1024'");
is(sql("SELECT nkeys, keys[1024] = lpad('1024', 63, '0') FROM pssc_guc_test_list('tags')"),
	'1024|t', '1024 entries of 63-byte keys accepted');
is(find('tags', sprintf('%063d', 1000)), '999', 'find in a 1024-key list');
like(alter_err("SET $P.tags = '$k1024,x'"),
	qr/invalid value for parameter "$P\.tags".*\n.*DETAIL:  The list has more than 1024 entries\./,
	'1025 entries rejected with a clear error');
like(alter_err("SET $P.exclude_tags = '" . join(',', ('a') x 1025) . "'"),
	qr/DETAIL:  The list has more than 1024 entries\./,
	'1025 entries rejected even when all are duplicates');
is(alter_err("SET $P.exclude_tags = '" . join(',', ('a') x 1024) . "'"), '',
	'1024 duplicate entries accepted');
like(alter_err("SET $P.tags = '" . (',' x 200000) . "'"),
	qr/more than 1024 entries/, 'huge list rejected');
is(list('tags'), 'f|1024|{' . join(',', map { sprintf('%063d', $_) } 1 .. 1024) . '}',
	'rejected lists leave the previous value');
# Size computation boundaries (-1 is SIZE_MAX). Arguments beyond SIZE_MAX
# (e.g. 2^32 on 32-bit) are rejected, not truncated.
is( sql(q{SELECT string_agg(coalesce(pssc_guc_test_blob_size(n, b)::text, 'null'), ',' ORDER BY i)
            FROM (VALUES (1, 0, 0), (2, 1, 64), (3, 1024, 65536), (4, 1025, 0),
                         (5, 1024, 65537), (6, 1, 65), (7, 0, 1), (8, -1, -1),
                         (9, 4294967296, 0), (10, 390000000, 780000000),
                         (11, 1024, -1), (12, 1, 4294967296)) v(i, n, b)}),
	'12,84,73740,null,null,null,null,null,null,null,null,null',
	'blob size: exact at the limits, rejected beyond them (no overflow)');
my ($bs_rc, $bs_out, $bs_err) =
  $node->psql('postgres', 'SELECT pssc_guc_test_blob_size(-4294967296, 0)');
like($bs_err, qr/ERROR:.*-1 is the only negative argument/,
	'blob size: negative arguments other than -1 are an error, not truncated');


# ---------------------------------------------------------------------------
# An existing backend picks up reloads and replaces its parsed lists
# ---------------------------------------------------------------------------
alter_and_reload("SET $P.tags = 'x, y'", "SET $P.exclude_tags = 'e'");
my $s = session_open();
my $spid = sq($s, 'SELECT pg_backend_pid()');
is(sq($s, "SELECT match_all, nkeys, keys FROM pssc_guc_test_list('tags')"), 'f|2|{x,y}',
	'session: initial tags');
my $sg = sq($s, 'SELECT pssc_guc_test_generation()');

# (a) changed values
alter_and_reload("SET $P.tags = 'y, z, x'", "SET $P.exclude_tags = 'f, e'");
is(sq($s, "SELECT match_all, nkeys, keys FROM pssc_guc_test_list('tags')"), 'f|3|{y,z,x}',
	'session: changed tags applied in the existing backend');
is(sq($s, "SELECT keys FROM pssc_guc_test_list('exclude_tags')"), '{f,e}',
	'session: changed exclude_tags applied');
is(sq($s, "SELECT pssc_guc_test_find('tags', 'z')"), '1', 'session: find in new list');
my $sg1 = sq($s, 'SELECT pssc_guc_test_generation()');
cmp_ok($sg1, '>', $sg, 'session: generation bumped');

# (b) equivalent values
alter_and_reload("SET $P.tags = ' y,z ,x,y '", "SET $P.exclude_tags = 'f,e,f'");
is(sq($s, "SELECT match_all, nkeys, keys FROM pssc_guc_test_list('tags')"), 'f|3|{y,z,x}',
	'session: equivalent tags, same parsed list');
is(sq($s, 'SELECT pssc_guc_test_generation()'), $sg1,
	'session: equivalent values do not bump the generation');

# (c) invalid values (postgresql.conf; ALTER SYSTEM would refuse them)
$node->append_conf('postgresql.conf', "$P.tags = 'y,z,x'\n$P.exclude_tags = 'f, e'\n");
alter_and_reload("RESET $P.tags", "RESET $P.exclude_tags");
is(sq($s, 'SELECT pssc_guc_test_generation()'), $sg1,
	'session: same values moved to postgresql.conf, generation unchanged');
my $slogpos = -s $node->logfile;
$node->append_conf('postgresql.conf', "$P.tags = 'y, b ad'\n$P.exclude_tags = '*'\n");
alter_and_reload();
$node->wait_for_log(qr/invalid value for parameter "$P\.exclude_tags"/, $slogpos);
is(sq($s, "SELECT match_all, nkeys, keys FROM pssc_guc_test_list('tags')"), 'f|3|{y,z,x}',
	'session: invalid tags on reload, previous parsed list kept');
is(sq($s, "SELECT keys FROM pssc_guc_test_list('exclude_tags')"), '{f,e}',
	'session: invalid exclude_tags on reload, previous parsed list kept');
is(sq($s, "SHOW $P.tags"), 'y,z,x', 'session: previous tags string kept');
is(sq($s, 'SELECT pssc_guc_test_generation()'), $sg1,
	'session: invalid values do not bump the generation');

# Old blobs are freed: many reloads alternating two large lists must not
# grow the backend's malloc'd memory by anything near one blob per reload.
$node->append_conf('postgresql.conf', "$P.tags = 'action, controller, job'\n"
	  . "$P.exclude_tags = 'traceparent, tracestate, request_id'\n");
my @big = map { my $c = $_; join(',', map { sprintf("$c%062d", $_) } 1 .. 1024) } ('p', 'q');
my $m0_null = sq($s, 'SELECT pssc_guc_test_malloc_used() IS NULL');
# The probe needs mallinfo2(), i.e. a module compiled against glibc >= 2.33.
# Check that against the compile-time glibc version, so a NULL probe there is
# never a silently skipped leak check.
my $built_glibc = sq($s, 'SELECT coalesce(pssc_guc_test_glibc_version(), \'none\')');
my $want_probe = $built_glibc =~ /^(\d+)\.(\d+)$/ && ($1 > 2 || ($1 == 2 && $2 >= 33));
note "module built against glibc: $built_glibc; malloc probe "
  . ($m0_null eq 't' ? 'NULL' : 'available');
is($m0_null, $want_probe ? 'f' : 't',
	'session: malloc probe available exactly when built against glibc >= 2.33');
# Sanity check of the compile-time detection: a module running on glibc must
# have been built against glibc. getconf is only an optional oracle.
SKIP:
{
	my $rt = $^O eq 'linux' ? `getconf GNU_LIBC_VERSION 2>/dev/null` : undef;
	skip 'runtime glibc version unknown (getconf unavailable or not glibc)', 1
	  unless defined $rt && $? == 0 && $rt =~ /^glibc (\d+\.\d+)/;
	note "runtime glibc: $1";
	isnt($built_glibc, 'none', 'session: module running on glibc was built against glibc');
}
SKIP:
{
	skip 'malloc statistics need glibc >= 2.33 (mallinfo2); leak check not run', 2
	  if $m0_null eq 't';
	for my $i (1 .. 4)
	{
		alter_and_reload("SET $P.tags = '$big[$i % 2]'", "SET $P.exclude_tags = '$big[1 - $i % 2]'");
	}
	my $m1 = sq($s, 'SELECT pssc_guc_test_malloc_used()');
	my $g_before = sq($s, 'SELECT pssc_guc_test_generation()');
	my $cycles = 30;
	for my $i (1 .. $cycles)
	{
		alter_and_reload("SET $P.tags = '$big[$i % 2]'", "SET $P.exclude_tags = '$big[1 - $i % 2]'");
	}
	my $m2 = sq($s, 'SELECT pssc_guc_test_malloc_used()');
	is(sq($s, 'SELECT pssc_guc_test_generation()') - $g_before, 2 * $cycles,
		'session: every alternating reload replaced both lists');
	# One leaked tag-list blob per list and reload would be ~4.4 MB.
	cmp_ok($m2 - $m1, '<', 1_000_000,
		"session: malloc'd memory stable over $cycles reloads (grew by " . ($m2 - $m1) . ' bytes)');
}
is(sq($s, 'SELECT pg_backend_pid()'), $spid, 'session: same backend throughout');
session_close($s);
alter_and_reload("RESET $P.tags", "RESET $P.exclude_tags");

# ---------------------------------------------------------------------------
# Invalid values in postgresql.conf are rejected on reload; previous value kept
# ---------------------------------------------------------------------------
alter_and_reload("RESET $P.tags", "RESET $P.exclude_tags", "RESET $P.extractors",
	"RESET $P.untagged");
$node->append_conf('postgresql.conf', qq{$P.tags = 'job, action'\n$P.untagged = 'record'\n});
alter_and_reload();
is(list('tags'), 'f|2|{job,action}', 'tags from postgresql.conf');
my $g3 = gen();
my $logpos = -s $node->logfile;
$node->append_conf('postgresql.conf', qq{
$P.tags = 'job, act ion'
$P.exclude_tags = 'a, *'
$P.untagged = 'sometimes'
$P.max_tags = 0
});
alter_and_reload();
$node->wait_for_log(qr/invalid value for parameter "$P\.untagged"/, $logpos);
my $log = substr(slurp_file($node->logfile), $logpos);
like($log,
	qr/invalid value for parameter "$P\.tags": "job, act ion"\n.*DETAIL:  Tag key "act ion" contains whitespace\./,
	'reload: invalid tags logged with detail');
like($log,
	qr/invalid value for parameter "$P\.exclude_tags": "a, \*"\n.*DETAIL:  .*\*/,
	'reload: invalid exclude_tags logged with detail');
like($log, qr/invalid value for parameter "$P\.untagged": "sometimes"/,
	'reload: invalid enum logged');
like($log, qr/0 is outside the valid range for parameter "$P\.max_tags" \(1 \.\. 64\)/,
	'reload: out-of-range postmaster value logged');
is(show('tags'), 'job, action', 'reload: previous tags value kept');
is(list('tags'), 'f|2|{job,action}', 'reload: previous parsed tags kept');
is(list('exclude_tags'), 'f|3|{traceparent,tracestate,request_id}',
	'reload: previous parsed exclude_tags kept');
is(show('untagged'), 'record', 'reload: previous untagged kept');
is(show('max_tags'), '8', 'reload: max_tags unchanged');
is(gen(), $g3, 'reload: rejected values do not bump the generation');

# Invalid values in postgresql.conf at startup: rejected, defaults used.
$logpos = -s $node->logfile;
$node->restart;
$log = substr(slurp_file($node->logfile), $logpos);
like($log, qr/invalid value for parameter "$P\.tags": "job, act ion"/,
	'startup: invalid tags reported');
like($log, qr/0 is outside the valid range for parameter "$P\.max_tags"/,
	'startup: out-of-range max_tags reported');
is(list('tags'), 'f|3|{action,controller,job}', 'startup: invalid tags -> default kept');
is(list('exclude_tags'), 'f|3|{traceparent,tracestate,request_id}',
	'startup: invalid exclude_tags -> default kept');
is(show('untagged'), 'skip', 'startup: invalid untagged -> default kept');
is(var('max_tags'), '8', 'startup: invalid max_tags -> default kept');

# Prefix reservation (placeholder from postgresql.conf above).
if ($vnum >= 150000)
{
	like($log, qr/invalid configuration parameter name "$P\.no_such_setting", removing it/,
		'prefix: existing placeholder removed at load (PG15+)');
	my ($ret, $out, $err) = $node->psql('postgres', "SET $P.other_setting = 1");
	like($err, qr/invalid configuration parameter name "$P\.other_setting"/,
		'prefix: new placeholders are rejected (PG15+)');
}
else
{
	like($log, qr/unrecognized configuration parameter "$P\.no_such_setting"/,
		'prefix: warning for existing placeholder at load (PG14)');
}

# Fix postgresql.conf for the rest of the test (later lines win).
$node->append_conf('postgresql.conf', qq{
$P.tags = 'action, controller, job'
$P.exclude_tags = 'traceparent, tracestate, request_id'
$P.untagged = 'skip'
$P.max_tags = 8
});
alter_and_reload();

# ---------------------------------------------------------------------------
# postmaster GUCs: reload leaves them alone (pending_restart), restart applies
# ---------------------------------------------------------------------------
my @pm = (
	[ 'max_entries', '500', '500', '500' ],
	[ 'bucket_count', '6', '6', '6' ],
	[ 'bucket_interval', "'1min'", '1min', '60' ],
	[ 'max_tags', '4', '4', '4' ],
	[ 'max_tag_value_len', '32', '32', '32' ],
	[ 'max_tagset_bytes', '256', '256', '256' ]);
my %before = map { $_->[0] => show($_->[0]) } @pm;
alter_and_reload(map { "SET $P.$_->[0] = $_->[1]" } @pm);
for my $g (@pm)
{
	is(show($g->[0]), $before{ $g->[0] }, "reload: $g->[0] unchanged until restart");
}
is( sql(qq{SELECT string_agg(name, ',' ORDER BY name COLLATE "C") FROM pg_settings
            WHERE name LIKE '$P.%' AND pending_restart}),
	join(',', map { "$P.$_" } sort map { $_->[0] } @pm),
	'pending_restart set for exactly the changed postmaster GUCs');
$node->restart;
for my $g (@pm)
{
	is(show($g->[0]), $g->[2], "restart: $g->[0] = $g->[2]");
	is(var($g->[0]), $g->[3], "restart: $g->[0] C variable = $g->[3]");
}
is( sql(qq{SELECT count(*) FROM pg_settings WHERE name LIKE '$P.%' AND pending_restart}),
	'0', 'no restart pending after restart');

# Lower bounds work at startup.
sql(join('', map { "ALTER SYSTEM SET $P.$_;\n" }
	  'max_entries = 100', 'bucket_count = 1', "bucket_interval = '1s'", 'max_tags = 1',
	  'max_tag_value_len = 1', 'max_tagset_bytes = 128', "scan_window = '64B'"));
$node->restart;
is( sql(qq{SELECT string_agg(setting, ',' ORDER BY name COLLATE "C") FROM pg_settings
            WHERE name IN ('$P.max_entries', '$P.bucket_count', '$P.bucket_interval',
                           '$P.max_tags', '$P.max_tag_value_len', '$P.max_tagset_bytes',
                           '$P.scan_window')}),
	'1,1,100,1,1,128,64', 'server starts with every lower bound');
sql("ALTER SYSTEM RESET ALL");
$node->restart;

# ---------------------------------------------------------------------------
# Bounds with units (the range message includes units only on PG17+)
# ---------------------------------------------------------------------------
is(alter_err("SET $P.bucket_interval = '86400s'"), '', 'bucket_interval = 86400s accepted');
is(alter_err("SET $P.bucket_interval = '1d'"), '', "bucket_interval = '1d' accepted");
like(alter_err("SET $P.bucket_interval = '86401s'"),
	qr/86401 s is outside the valid range for parameter "$P\.bucket_interval" \(1(?: s)? \.\. 86400(?: s)?\)/,
	'bucket_interval: 86401s rejected');
like(alter_err("SET $P.bucket_interval = 0"),
	qr/0 s is outside the valid range for parameter "$P\.bucket_interval"/,
	'bucket_interval: 0 rejected');
is(alter_err("SET $P.scan_window = '1MB'"), '', 'scan_window = 1MB accepted');
is(alter_err("SET $P.scan_window = '64B'"), '', 'scan_window = 64B accepted');
like(alter_err("SET $P.scan_window = '1025kB'"),
	qr/1049600 B is outside the valid range for parameter "$P\.scan_window" \(64(?: B)? \.\. 1048576(?: B)?\)/,
	'scan_window: 1025kB rejected');
like(alter_err("SET $P.scan_window = '63B'"),
	qr/63 B is outside the valid range for parameter "$P\.scan_window"/,
	'scan_window: 63B rejected');
# Upper bounds of unitless postmaster GUCs (validated, never started with).
for my $ok ('max_entries = 1073741823', 'bucket_count = 10000', 'max_tags = 64',
	'max_tag_value_len = 4096', 'max_tagset_bytes = 8192')
{
	is(alter_err("SET $P.$ok"), '', "upper bound accepted: $ok");
}
sql("ALTER SYSTEM RESET ALL");

$node->stop;
$log = slurp_file($node->logfile);
unlike($log, qr/PANIC|TRAP|terminated by signal|server process .* was terminated/,
	'server log has no crashes');

# ---------------------------------------------------------------------------
# Not preloaded: no GUCs are defined
# ---------------------------------------------------------------------------
my $nopre = PostgreSQL::Test::Cluster->new('guc_nopreload');
$nopre->init;
$nopre->start;
is( $nopre->safe_psql('postgres',
		"LOAD '$P'; SELECT count(*) FROM pg_settings WHERE name LIKE '$P.%'"),
	'0', 'LOAD without preload defines no GUCs');
$nopre->stop;

done_testing();
