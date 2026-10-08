# untagged, tags, exclude_tags and scan_window are superuser-context GUCs
# (DESIGN.md §4.1; backlog 20261007-133120-1): a superuser, or on PG 15+ a
# role granted SET on the parameter, can set them per database, per role or
# per session. Each backend reads them at extraction, and store entries are
# keyed by dbid, so per-database values do not collide. A function's SET
# clause changes tags mid-statement, also while the outer statement's frame
# holds recap candidates (role-scoped caps, SECURITY DEFINER).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';
my @suset = qw(untagged tags exclude_tags scan_window);
my @sighup = qw(save reclaim_worker_interval extractors normalize
  cardinality_cap cardinality_cap_overrides);

my $node = PostgreSQL::Test::Cluster->new('perdb_gucs');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.extractors = 'sqlcommenter, marginalia(position=any), regex(pattern=''svc=(\\w+)'', keys=svc, merge=on)'
$P.track = all
$P.track_utility = off
$P.save = off
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub dbsql { my ($db, $q, @o) = @_; return $node->safe_psql($db, $q, @o); }

sql(<<"EOS");
CREATE EXTENSION $P;
CREATE EXTENSION pssc_extract_test;
CREATE DATABASE dba;
CREATE DATABASE dbb;
CREATE ROLE alice LOGIN;
CREATE ROLE carol LOGIN;
GRANT ALL ON SCHEMA public TO PUBLIC;
EOS

# ---------------------------------------------------------------------------
# pg_settings.context
# ---------------------------------------------------------------------------
for my $g (@suset)
{
	is(sql("SELECT context FROM pg_settings WHERE name = '$P.$g'"), 'superuser',
		"$g: superuser context");
}
for my $g (@sighup)
{
	is(sql("SELECT context FROM pg_settings WHERE name = '$P.$g'"), 'sighup',
		"$g: still sighup");
}

# ---------------------------------------------------------------------------
# ALTER DATABASE / ALTER ROLE SET: new sessions of that database or role
# ---------------------------------------------------------------------------
my %val = (
	untagged => 'record',
	tags => 'a, svc',
	exclude_tags => 'x',
	scan_window => '4kB');
for my $g (@suset)
{
	sql("ALTER DATABASE dba SET $P.$g = '$val{$g}'");
	sql("ALTER ROLE carol SET $P.$g = '$val{$g}'");
}
for my $g (@suset)
{
	is(dbsql('dba', "SHOW $P.$g"), $val{$g}, "ALTER DATABASE SET $g: new session of dba");
	isnt(dbsql('dbb', "SHOW $P.$g"), $val{$g}, "ALTER DATABASE SET $g: not in dbb");
	is(dbsql('dbb', "SHOW $P.$g", extra_params => [ '-U', 'carol' ]), $val{$g},
		"ALTER ROLE SET $g: new session of carol");
	isnt(dbsql('dbb', "SHOW $P.$g", extra_params => [ '-U', 'alice' ]), $val{$g},
		"ALTER ROLE SET $g: not for alice");
}
for my $g (@suset)
{
	sql("ALTER ROLE carol RESET $P.$g");
}

# ---------------------------------------------------------------------------
# untagged = record only in dba; per-database tags allowlists
# ---------------------------------------------------------------------------
sql("ALTER DATABASE dba SET $P.tags = 'a'");
sql("ALTER DATABASE dbb SET $P.tags = 'b'");
sql("SELECT ${P}_reset()");
dbsql('dba', "SELECT 'untagged probe'");
dbsql('dbb', "SELECT 'untagged probe'");
dbsql('dba', "SELECT 'tagged probe' /*a:1,b:2,c:3*/");
dbsql('dbb', "SELECT 'tagged probe' /*a:1,b:2,c:3*/");

sub db_entries
{
	my ($db, $where) = @_;
	return sql("SELECT count(*) FROM ${P}_totals"
		  . " WHERE dbid = (SELECT oid FROM pg_database WHERE datname = '$db') AND $where");
}
cmp_ok(db_entries('dba', "tags = '{}'"), '>', 0, 'untagged = record in dba: recorded');
is(db_entries('dbb', "tags = '{}'"), '0', 'untagged = skip in dbb: not recorded');
is(db_entries('dba', q{tags = '{"a": "1"}'}), '1', "dba keeps only its allowlisted key a");
is(db_entries('dba', "tags ? 'b' OR tags ? 'c'"), '0', 'dba drops b and c');
is(db_entries('dbb', q{tags = '{"b": "2"}'}), '1', "dbb keeps only its allowlisted key b");
is(db_entries('dbb', "tags ? 'a' OR tags ? 'c'"), '0', 'dbb drops a and c');
for my $g (@suset)
{
	sql("ALTER DATABASE dba RESET $P.$g");
	sql("ALTER DATABASE dbb RESET $P.$g");
}

# exclude_tags and scan_window per session, used by _extract()
is(sql(qq{SET $P.tags = '*'; SET $P.exclude_tags = 'b';
          SELECT ${P}_extract('SELECT 1 /*a:1,b:2*/')->'tags'}),
	'{"a": "1"}', 'session exclude_tags is used by _extract()');
# a sqlcommenter comment (position=append) longer than the default 2kB window
my $long = q{SELECT 1 /*a=''1'',pad=''} . ('x' x 3000) . q{''*/};
is(sql(qq{SET $P.tags = 'a'; SELECT ${P}_extract('$long')->'tags'}),
	'{}', 'default scan_window 2kB: a 3kB trailing comment is not found');
is(sql(qq{SET $P.tags = 'a'; SET $P.scan_window = '4kB'; SELECT ${P}_extract('$long')->'tags'}),
	'{"a": "1"}', 'session scan_window 4kB: the 3kB trailing comment is found');

# ---------------------------------------------------------------------------
# Permissions: a non-superuser cannot SET them; GRANT SET ON PARAMETER (15+)
# ---------------------------------------------------------------------------
for my $g (@suset)
{
	my ($ret, $out, $err) = $node->psql('postgres', "SET $P.$g = '$val{$g}'",
		extra_params => [ '-U', 'alice' ]);
	ok($ret != 0 && $err =~ /permission denied to set parameter/,
		"non-superuser cannot SET $g") or diag($err);
	($ret, $out, $err) = $node->psql('postgres', "ALTER ROLE alice SET $P.$g = '$val{$g}'",
		extra_params => [ '-U', 'alice' ]);
	ok($ret != 0 && $err =~ /permission denied/,
		"non-superuser cannot ALTER ROLE SET $g") or diag($err);
}
SKIP:
{
	skip 'GRANT SET ON PARAMETER needs PG 15+', 2 * @suset
	  if $node->safe_psql('postgres', 'SHOW server_version_num') < 150000;
	for my $g (@suset)
	{
		sql("GRANT SET ON PARAMETER $P.$g TO alice");
		is($node->safe_psql('postgres', "SET $P.$g = '$val{$g}'; SHOW $P.$g",
				extra_params => [ '-U', 'alice' ]),
			$val{$g}, "GRANT SET ON PARAMETER: alice can SET $g");
	}
	sql("SELECT ${P}_reset()");
	$node->safe_psql('postgres',
		"SET $P.untagged = 'record'; SET $P.tags = 'k'; SELECT 'granted probe' /*k:v,j:w*/",
		extra_params => [ '-U', 'alice' ]);
	is(sql("SELECT count(*) FROM ${P}_totals WHERE userid = 'alice'::regrole"
			  . q{ AND tags = '{"k": "v"}'}),
		'1', "alice's session tags are used for her statements");
}

# ---------------------------------------------------------------------------
# A tags change does not recompile the regex extractors (only extractors and
# normalize changes do): one backend, executor hooks extracting each statement
# ---------------------------------------------------------------------------
like(sql(qq{SELECT 1 /* svc=a */;
           SET $P.tags = 'x';
           SELECT 2 /* svc=a */;
           SET $P.tags = 'svc';
           SELECT 3 /* svc=a */;
           SET $P.exclude_tags = 'svc';
           SELECT 4 /* svc=a */;
           SELECT compiles || '/' || frees FROM pssc_extract_test_regex_stats()}),
	qr/\n1\/0$/, 'tags/exclude_tags changes keep the compiled regexes');

# ---------------------------------------------------------------------------
# Mid-statement changes through a function's SET clause, while the outer
# frame holds recap candidates: role-scoped caps, a SECURITY DEFINER function
# (owned by the superuser) whose statements inherit or scan tags, called per
# row, with errors rolling back the SET in subtransactions.
# ---------------------------------------------------------------------------
sql(<<"EOS");
ALTER SYSTEM SET $P.cardinality_cap = 1;
ALTER SYSTEM SET $P.tags = '*';
ALTER SYSTEM SET $P.exclude_tags = '';
SELECT pg_reload_conf();
EOS
$node->poll_query_until('postgres',
	"SELECT current_setting('$P.cardinality_cap') = '1' AND current_setting('$P.tags') = '*'")
  or die 'reload';

sql(<<"EOS");
CREATE TABLE t(i int);
INSERT INTO t SELECT generate_series(1, 20);
GRANT SELECT ON t TO PUBLIC;

-- runs as the superuser: inherits the caller's tags (recap for the definer)
CREATE FUNCTION def_inherit() RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET $P.tags = 'j' SET $P.nested_tags = 'inherit' AS \$\$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM t;
  RETURN n;
END \$\$;

-- runs as the superuser: tags from its own text, under its own allowlist
CREATE FUNCTION def_scan() RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET $P.tags = 'j' SET $P.exclude_tags = 'k' SET $P.nested_tags = 'scan' AS \$\$
DECLARE n bigint;
BEGIN
  SELECT count(*) /*j:inner,k:inner*/ INTO n FROM t;
  RETURN n;
END \$\$;

-- changes tags, then fails: the SET is rolled back with the subtransaction
CREATE FUNCTION def_fail() RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER
SET $P.tags = 'zz' SET $P.nested_tags = 'scan' AS \$\$
BEGIN
  PERFORM count(*) /*zz:1*/ FROM t;
  RAISE EXCEPTION 'boom';
END \$\$;

-- an invoker function that catches the failure of def_fail per call
CREATE FUNCTION try_fail() RETURNS int LANGUAGE plpgsql AS \$\$
BEGIN
  BEGIN
    PERFORM def_fail();
  EXCEPTION WHEN others THEN
    RETURN 0;
  END;
  RETURN 1;
END \$\$;
GRANT EXECUTE ON FUNCTION def_inherit(), def_scan(), def_fail(), try_fail() TO PUBLIC;
EOS

my $su = sql('SELECT current_user');

sub seen
{
	my ($role, $key) = @_;
	return sql("SELECT coalesce(string_agg(x, ',' ORDER BY x COLLATE \"C\"), '') FROM ("
		  . "SELECT coalesce(tags->>'$key', 'NULL') || '=' || sum(calls) AS x FROM ${P}_totals"
		  . " WHERE tags ? '$key' AND userid = '$role'::regrole"
		  . " GROUP BY tags->>'$key') s");
}

sql("SELECT ${P}_reset()");
# the superuser takes its cap of k (k=s1), and alice hers (k=a1)
sql("SELECT 'fill' /*k:s1*/");
$node->safe_psql('postgres', "SELECT 'fill' /*k:a1*/", extra_params => [ '-U', 'alice' ]);

my ($ret, $out, $err) = $node->psql('postgres', <<'EOS', extra_params => [ '-U', 'alice' ]);
SELECT sum(def_inherit() + def_scan() + try_fail()) FROM t /*k:a1,m:x*/;
SELECT def_inherit() + def_scan() + try_fail() /*k:a1,m:x*/;
SELECT 'after' /*k:a1,m:y*/;
EOS
is($ret, 0, 'mid-statement tags changes: no error') or diag($err);
ok($node->safe_psql('postgres', 'SELECT 1') eq '1', 'server still up');

# alice's statements keep her tags under the session allowlist '*'
is(seen('alice', 'k'), 'a1=4', "alice: k kept, the function's allowlist did not leak out");
is(seen('alice', 'm'), 'NULL=1,x=2', 'alice: m kept (y is over her cap)');
# the definer inherits alice's tags, capped for the superuser: k is over its cap
is(seen($su, 'k'), 'NULL=21,s1=1', 'definer, inherit: alice\'s k is null under the superuser');
# def_scan's statement: its own text under its own allowlist 'j'
is(seen($su, 'j'), 'inner=21', 'definer, scan: own tags under the function allowlist');
# def_fail's statement completed before the error rolled its SET back
is(seen($su, 'zz'), '1=21', 'failing definer: own tags under its allowlist');
is(sql("SELECT current_setting('$P.tags')"), '*', 'session tags unchanged');

# the same in one backend, with SET LOCAL and a rollback in between
($ret, $out, $err) = $node->psql('postgres', <<"EOS", extra_params => [ '-U', $su ]);
BEGIN;
SET LOCAL $P.tags = 'k';
SELECT def_inherit() + def_scan() + try_fail() FROM t /*k:s1,m:z*/;
SAVEPOINT s;
SET LOCAL $P.tags = 'm';
SELECT def_inherit() FROM t /*k:s1,m:z*/;
ROLLBACK TO s;
SELECT def_inherit() /*k:s1,m:z*/;
COMMIT;
SELECT current_setting('$P.tags');
EOS
is($ret, 0, 'SET LOCAL and savepoint rollback around tags changes: no error') or diag($err);
like($out, qr/\*$/, 'tags restored after the transaction');
# slurp_file, not log_contains: older PG14 minors (14.6) lack log_contains.
unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no assertion failure or crash');

done_testing();
