# Scope of the cardinality caps (DESIGN.md §6.1, §6.11; backlog
# 20261007-070036-1): pg_stat_statement_context.cardinality_cap_scope
# (postmaster; 'server' | 'database' | 'role', default 'role') selects
# whether the admitted values and per-key counts are kept per (role,
# database), per database, or server-wide.
#
# Covers: the GUC (default, context, enum values, invalid values rejected,
# not settable with SET); under 'role', one role filling a key's cap does
# not null another role's values, and a role can't learn whether another
# role sent a value (no membership oracle), also across databases for the
# same role; under 'database', roles of one database share the caps but
# databases don't; under 'server', the earlier server-wide behaviour.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('cap_scope');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = ''
$P.track_utility = off
$P.cardinality_cap = 1
$P.save = off
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql("CREATE EXTENSION $P; CREATE EXTENSION pssc_store_test; CREATE ROLE alice LOGIN; CREATE ROLE bob LOGIN; CREATE DATABASE db2");
foreach my $db ('postgres', 'db2')
{
	$node->safe_psql($db, 'CREATE TABLE t(i int); GRANT SELECT ON t TO PUBLIC');
}

# role sends one statement per value of key k in database db
sub send_as
{
	my ($role, $db, @values) = @_;
	$node->safe_psql($db, join('', map { "SELECT * FROM t /*k:$_*/;\n" } @values),
		extra_params => [ '-U', $role ]);
}

# role's recorded k values in db: "value=calls,..." (NULL for JSON null)
sub seen
{
	my ($role, $db) = @_;
	return sql("SELECT coalesce(string_agg(x, ',' ORDER BY x COLLATE \"C\"), '') FROM ("
		  . "SELECT coalesce(tags->>'k', 'NULL') || '=' || sum(calls) AS x FROM ${P}_totals"
		  . " WHERE tags ? 'k' AND userid = '$role'::regrole"
		  . " AND dbid = (SELECT oid FROM pg_database WHERE datname = '$db')"
		  . " GROUP BY tags->>'k') s");
}

sub reset_all { sql("SELECT ${P}_reset()"); }

sub set_scope
{
	my ($scope) = @_;
	sql("ALTER SYSTEM SET $P.cardinality_cap_scope = '$scope'");
	$node->restart;
	is(sql("SHOW $P.cardinality_cap_scope"), $scope, "scope set to '$scope'");
}

# ---------------------------------------------------------------------------
# The GUC
# ---------------------------------------------------------------------------
is( sql("SELECT setting || '/' || boot_val || '/' || context || '/' || vartype || '/' || enumvals::text"
		  . " FROM pg_settings WHERE name = '$P.cardinality_cap_scope'"),
	'role/role/postmaster/enum/{server,database,role}',
	'cardinality_cap_scope: default role, postmaster enum');
my ($ret, $out, $err) = $node->psql('postgres',
	"ALTER SYSTEM SET $P.cardinality_cap_scope = 'cluster'");
ok($ret != 0 && $err =~ /invalid value for parameter/, 'invalid scope rejected')
  or diag($err);
($ret, $out, $err) = $node->psql('postgres', "SET $P.cardinality_cap_scope = 'server'");
ok($ret != 0 && $err =~ /cannot be changed without restarting/, 'scope: not settable with SET')
  or diag($err);

# ---------------------------------------------------------------------------
# 'role' (default): per (role, database)
# ---------------------------------------------------------------------------
reset_all();
send_as('alice', 'postgres', 'a1', 'a2');
send_as('bob', 'postgres', 'b1');
is(seen('alice', 'postgres'), 'NULL=1,a1=1', 'role: alice has her own cap of 1');
is(seen('bob', 'postgres'), 'b1=1', "role: alice filling the cap does not null bob's first value");

# no membership oracle: bob, at his cap, gets the same answer for a value
# alice sent as for one nobody sent, whether or not alice sent it
send_as('bob', 'postgres', 'a1', 'zz');
my $with_alice = seen('bob', 'postgres');
reset_all();
send_as('bob', 'postgres', 'b1', 'a1', 'zz');
my $without_alice = seen('bob', 'postgres');
is($with_alice, 'NULL=2,b1=1', "role: alice's admitted a1 is null for bob, like unseen zz");
is($with_alice, $without_alice, 'role: bob cannot tell whether alice sent a1');

# _extract() peeks in the caller's scope too
reset_all();
send_as('alice', 'postgres', 'a1');
send_as('bob', 'postgres', 'b1');
sql("GRANT EXECUTE ON FUNCTION ${P}_extract(text, int, int) TO bob");
is( $node->safe_psql('postgres',
		"SELECT (${P}_extract('SELECT 1 /*k:a1*/')->>'tags') || ' ' || (${P}_extract('SELECT 1 /*k:b1*/')->>'tags')",
		extra_params => [ '-U', 'bob' ]),
	'{"k": null} {"k": "b1"}', "role: _extract() checks bob's own scope");

# the same role in two databases
reset_all();
send_as('alice', 'postgres', 'a1');
send_as('alice', 'db2', 'd1', 'a1');
is(seen('alice', 'postgres'), 'a1=1', 'role: alice in postgres');
is(seen('alice', 'db2'), 'NULL=1,d1=1', 'role: alice in db2 has her own cap there');

# Slot positions in a scoped table are keyed with a secret drawn at startup
# and on each _reset(): otherwise they would follow from public OIDs, and a
# role could fill the probe window around another scope's candidate value
# with its own values and tell from one more whether that slot is taken.
# ("first slot of k=v1..v8 in role's scope in postgres", via the TEST-ONLY
# pssc_store_test_cap_slot())
sub slots
{
	my ($role) = @_;
	return sql("SELECT string_agg(pssc_store_test_cap_slot('k', 'v' || i, '$role'::regrole,"
		  . " (SELECT oid FROM pg_database WHERE datname = 'postgres'))::text, ',' ORDER BY i)"
		  . " FROM generate_series(1, 8) i");
}
my $before = slots('alice');
like($before, qr/^\d+(,\d+){7}$/, "role: slots of alice's values ($before)");
is(slots('alice'), $before, 'role: slots are stable between resets');
isnt(slots('bob'), $before, "role: bob's slots differ from alice's");
$node->restart;
my $restarted = slots('alice');
isnt($restarted, $before, "role: slots are rekeyed on restart ($restarted)");
reset_all();
isnt(slots('alice'), $restarted, 'role: slots are rekeyed by _reset()');

# ---------------------------------------------------------------------------
# 'database': shared by the roles of a database, not across databases
# ---------------------------------------------------------------------------
set_scope('database');
reset_all();
send_as('alice', 'postgres', 'a1');
send_as('bob', 'postgres', 'b1', 'a1');
send_as('bob', 'db2', 'd1');
is(seen('bob', 'postgres'), 'NULL=1,a1=1', "database: bob shares alice's cap in postgres");
is(seen('bob', 'db2'), 'd1=1', 'database: db2 has its own cap');
is(slots('bob'), slots('alice'), 'database: the roles of a database share slots');

# ---------------------------------------------------------------------------
# 'server': one cap for everyone (the earlier behaviour)
# ---------------------------------------------------------------------------
set_scope('server');
reset_all();
send_as('alice', 'postgres', 'a1');
send_as('bob', 'db2', 'b1', 'a1');
is(seen('bob', 'db2'), 'NULL=1,a1=1', 'server: shared across roles and databases');

# server keeps the unkeyed hash of earlier versions: the same slots for
# every role, across restarts and resets
my $server = slots('alice');
is(slots('bob'), $server, 'server: the same slots for every role');
reset_all();
is(slots('alice'), $server, 'server: slots unchanged by _reset()');
$node->restart;
is(slots('alice'), $server, 'server: slots unchanged by a restart');

$node->stop;
done_testing();
