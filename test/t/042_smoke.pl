# "make smoke" (test/smoke/smoke.sql), the smoke test for a provisioned
# server (backlog 20261008-065635-12): psql only, as a NOSUPERUSER role,
# against an existing server that preloads the library with its own
# configuration, changing no setting. Run here as:
#   - "dba", the managed-service administrator of test/t/037 (NOSUPERUSER,
#     CREATEDB, CREATEROLE, pg_monitor), with and without the right to call
#     _extract() (as the extension's owner, or not), and as a plain role;
#     each passes, records its tagged statements, and leaves
#     postgresql.auto.conf, pg_file_settings and the role and database
#     settings as they were;
#   - a superuser in a database without the extension, which it creates;
# and it fails (non-zero exit, a FAIL line) when:
#   - the extension is missing and the role may not create it;
#   - the tagged statements are not recorded (the role's tags allowlist
#     keeps none of their keys; recording is off for the role);
#   - the library is not preloaded.
use strict;
use warnings;

use File::Basename qw(dirname);
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';
my $root = dirname(__FILE__) . '/../..';
my $script = "$root/test/smoke/smoke.sql";

# ------------------------------------------------------------ the files
ok(-f $script, 'test/smoke/smoke.sql exists');
my $smoke_sql = -f $script ? slurp_file($script) : '';
unlike($smoke_sql, qr/ALTER\s+(?:SYSTEM|ROLE|DATABASE)|pg_reload_conf|_reset\s*\(|\bset_config\b/i,
	'the smoke script changes no setting and resets nothing');

my $makefile = slurp_file("$root/Makefile");
like($makefile, qr{^smoke:\n\t[^\n]*(?i:psql)[^\n]*-f [^\n]*test/smoke/smoke\.sql}m,
	'Makefile: the smoke target runs test/smoke/smoke.sql with psql');
like($makefile, qr/installcheck[^\n]*\n(?:#[^\n]*\n)*?#[^\n]*ALTER SYSTEM/,
	'Makefile warns that installcheck changes settings with ALTER SYSTEM');
my $readme = slurp_file("$root/README.md");
like($readme, qr/make smoke/, 'README.md documents make smoke');
like($readme, qr/installcheck[^\n]*ALTER SYSTEM[^\n]*disposable/,
	'README.md warns that installcheck is for disposable clusters');

# ------------------------------------------------------------ the server
my $node = PostgreSQL::Test::Cluster->new('smoke');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }
my $super = sql('SELECT current_user');

sql(q{CREATE ROLE dba LOGIN NOSUPERUSER CREATEDB CREATEROLE;
	GRANT pg_monitor TO dba;
	CREATE ROLE app LOGIN;
	CREATE DATABASE appdb OWNER dba;
	CREATE DATABASE dbadb OWNER dba;
	CREATE DATABASE noext OWNER dba;
	CREATE DATABASE superdb});
sql("CREATE EXTENSION $P", 'appdb');
# In dbadb dba owns the extension (as after CREATE EXTENSION by a managed
# service's administrator), so it may call _extract().
sql('ALTER ROLE dba SUPERUSER');
$node->safe_psql('dbadb', "CREATE EXTENSION $P", extra_params => [ '-U', 'dba' ]);
sql('ALTER ROLE dba NOSUPERUSER');

# Runs the smoke script as $role in $db; returns (exit code, stdout, stderr).
sub smoke
{
	my ($role, $db) = @_;
	my ($out, $err) = ('', '');
	IPC::Run::run([ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1',
			'-d', $node->connstr($db) . " user=$role", '-f', $script ],
		'<', \undef, '>', \$out, '2>', \$err);
	my $rc = $? >> 8;
	note "smoke as $role in $db: exit $rc\n$out$err";
	return ($rc, $out, $err);
}

# What the smoke test must leave alone, read as superuser.
sub settings_snapshot
{
	my $auto = slurp_file($node->data_dir . '/postgresql.auto.conf');
	my $file = sql(q{SELECT string_agg(concat_ws('|', sourcefile, sourceline,
		seqno, name, setting, applied, error), E'\n' ORDER BY seqno)
		FROM pg_file_settings});
	my $roledb = sql(q{SELECT string_agg(setrole || '|' || setdatabase || '|'
		|| array_to_string(setconfig, ','), E'\n' ORDER BY setrole, setdatabase)
		FROM pg_db_role_setting});
	return "auto.conf:\n$auto\npg_file_settings:\n$file\npg_db_role_setting:\n$roledb\n";
}

# The calls recorded for $role's smoke statements, read as superuser.
sub smoke_calls
{
	my ($role, $db) = @_;
	return 0
	  if sql("SELECT count(*) FROM pg_extension WHERE extname = '$P'", $db) eq '0';
	return sql(qq{SELECT coalesce(sum(calls_total), 0) FROM ${P}(true, true)
		WHERE userid = '$role'::regrole AND dbid = (SELECT oid FROM pg_database
		  WHERE datname = current_database())
		  AND tags->>'controller' = 'pssc_smoke'}, $db);
}

sub smoke_passes
{
	my ($role, $db, $name, @like) = @_;
	my $before = settings_snapshot();
	my $calls = smoke_calls($role, $db);
	my ($rc, $out, $err) = smoke($role, $db);
	is($rc, 0, "$name: exit 0");
	unlike($out . $err, qr/FAIL|ERROR/, "$name: no FAIL or ERROR");
	like($out, qr/^smoke test passed/m, "$name: reports success");
	like($out, $_, "$name: reports $_") for @like;
	ok(smoke_calls($role, $db) >= $calls + 3,
		"$name: its tagged statements were recorded");
	is(settings_snapshot(), $before,
		"$name: postgresql.auto.conf, pg_file_settings and role/database settings unchanged");
	return $out;
}

sub smoke_fails
{
	my ($role, $db, $name, $re) = @_;
	my ($rc, $out, $err) = smoke($role, $db);
	isnt($rc, 0, "$name: non-zero exit");
	like($out . $err, $re, "$name: reports why");
}

# ------------------------------------------------------------ passes
# Some settings in the file, to see that they survive the run.
$node->append_conf('postgresql.conf', "$P.track_utility = on\n");
sql("ALTER SYSTEM SET $P.untagged = 'skip'");
sql('SELECT pg_reload_conf()');

smoke_passes('dba', 'appdb', 'dba, extension owned by a superuser',
	qr/^ok: .*shared_preload_libraries/m,
	qr/^skipped: _extract\(\) not executable by this role/m,
	qr/^ok: .*pg_stat_statement_context_info\(\)/m,
	qr/^ok: .*pg_stat_statement_context_counters\(\)/m);
smoke_passes('dba', 'dbadb', 'dba, owner of the extension',
	qr/^ok: _extract\(\)/m);
smoke_passes('app', 'appdb', 'plain role',
	qr/^skipped: shared_preload_libraries not readable by this role/m,
	qr/^skipped: _extract\(\) not executable by this role/m);
is(sql("SELECT count(*) FROM pg_extension WHERE extname = '$P'", 'superdb'),
	'0', 'superdb has no extension yet');
smoke_passes($super, 'superdb', 'superuser, extension missing',
	qr/^ok: created extension/m, qr/^ok: _extract\(\)/m);
is(sql("SELECT count(*) FROM pg_extension WHERE extname = '$P'", 'superdb'),
	'1', 'the smoke test created the extension');

# ------------------------------------------------------------ failures
smoke_fails('dba', 'noext', 'extension missing, not creatable',
	qr/^FAIL: .*CREATE EXTENSION/m);
is(sql("SELECT count(*) FROM pg_extension WHERE extname = '$P'", 'noext'),
	'0', 'no extension in noext');

sql("ALTER ROLE app SET $P.tags = 'job'");
smoke_fails('app', 'appdb', 'no tag of the smoke statements kept',
	qr/^FAIL: .*not recorded/m);
sql("ALTER ROLE app RESET $P.tags");

sql("ALTER ROLE dba SET $P.enabled = off");
smoke_fails('dba', 'dbadb', 'recording off, _extract() executable',
	qr/^FAIL: .*not recorded/m);
sql("ALTER ROLE dba RESET $P.enabled");
smoke_passes('dba', 'dbadb', 'dba again, recording back on');

$node->append_conf('postgresql.conf', "shared_preload_libraries = ''\n");
$node->restart;
smoke_fails($super, 'appdb', 'not preloaded (superuser)',
	qr/^FAIL: .*shared_preload_libraries/m);
smoke_fails('app', 'appdb', 'not preloaded (plain role)',
	qr/must be loaded via "shared_preload_libraries"/);

$node->stop;
done_testing();
