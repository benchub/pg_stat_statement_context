# Cardinality caps follow the identity that records the tags (DESIGN.md
# §6.1, §6.11; backlog 20261007-070036-2). Under cardinality_cap_scope =
# 'role' a statement's tags are capped when they are extracted, in the
# scope of the role extracting them, but they can be recorded or inherited
# under another role: a cursor opened under one role and closed under
# another (the entry takes the role at ExecutorEnd, as pgss's does), a
# SECURITY DEFINER function's statements inheriting the caller's tags, a
# cursor a definer opens and the caller closes (nested_tags = scan), and a
# tagged SET ROLE (recorded under the new role). In each case every
# recorded value must obey the cap of the role it is recorded under: the
# caps are applied again, to the uncapped values, for that role.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('cap_identity');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = ''
# bob's cursors are tagged mid-statement in PL/pgSQL
$P.extractors = 'marginalia(position=any)'
$P.track = all
$P.track_utility = off
$P.cardinality_cap = 1
$P.save = off
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(<<"EOS");
CREATE EXTENSION $P;
CREATE ROLE alice LOGIN;
CREATE ROLE bob LOGIN;
CREATE TABLE t(i int);
INSERT INTO t VALUES (1), (2);
GRANT SELECT ON t TO PUBLIC;

-- an invoker's cursor: it inherits the tags of the statement calling this
CREATE FUNCTION open_cur(name text) RETURNS refcursor LANGUAGE plpgsql AS \$\$
DECLARE c refcursor := name;
BEGIN
  OPEN c FOR SELECT * FROM t;
  RETURN c;
END \$\$;

-- bob's SECURITY DEFINER function: its statement runs (and is recorded) as
-- bob with the caller's tags (nested_tags = inherit)
CREATE FUNCTION bob_count() RETURNS bigint LANGUAGE plpgsql SECURITY DEFINER AS \$\$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM t;
  RETURN n;
END \$\$;
ALTER FUNCTION bob_count() OWNER TO bob;

-- bob's cursors, tagged in his own text (nested_tags = scan)
CREATE FUNCTION bob_open(name text, v text) RETURNS refcursor LANGUAGE plpgsql
SECURITY DEFINER AS \$\$
DECLARE c refcursor := name;
BEGIN
  IF v = 'b1' THEN
    OPEN c FOR SELECT * /*k:b1*/ FROM t;
  ELSE
    OPEN c FOR SELECT * /*k:a1*/ FROM t;
  END IF;
  RETURN c;
END \$\$;
ALTER FUNCTION bob_open(text, text) OWNER TO bob;
EOS

# role sends one statement per value of key k
sub send_as
{
	my ($role, @values) = @_;
	$node->safe_psql('postgres', join('', map { "SELECT * FROM t /*k:$_*/;\n" } @values),
		extra_params => [ '-U', $role ]);
}

# role's recorded k values: "value=calls,..." (NULL for JSON null)
sub seen
{
	my ($role) = @_;
	return sql("SELECT coalesce(string_agg(x, ',' ORDER BY x COLLATE \"C\"), '') FROM ("
		  . "SELECT coalesce(tags->>'k', 'NULL') || '=' || sum(calls) AS x FROM ${P}_totals"
		  . " WHERE tags ? 'k' AND userid = '$role'::regrole"
		  . " GROUP BY tags->>'k') s");
}

# alice admits a1 and bob b1, so both are at their cap of 1
sub fill_caps
{
	sql("SELECT ${P}_reset()");
	send_as('alice', 'a1');
	send_as('bob', 'b1');
	is(seen('alice') . ' ' . seen('bob'), 'a1=1 b1=1', 'alice has a1, bob has b1');
}

# ---------------------------------------------------------------------------
# A cursor opened under alice and closed under bob is recorded under bob
# (pgss's userid at ExecutorEnd), so bob's cap applies.
# ---------------------------------------------------------------------------
fill_caps();
sql(<<'EOS');
BEGIN;
SET ROLE alice;
SELECT open_cur('c1') /*k:a1*/;
SELECT open_cur('c2') /*k:b1*/;
FETCH 1 FROM c1;
FETCH 1 FROM c2;
SET ROLE bob;
CLOSE c1;
CLOSE c2;
COMMIT;
EOS
is(seen('bob'), 'NULL=1,b1=2',
	"role change: alice's a1 is null under bob, b1 (null for alice) is bob's");
is(seen('alice'), 'NULL=1,a1=2', "role change: alice's own statements keep her caps");

# ---------------------------------------------------------------------------
# SECURITY DEFINER: bob's statement inherits alice's tags and is recorded
# under bob, so bob's cap applies.
# ---------------------------------------------------------------------------
fill_caps();
$node->safe_psql('postgres', "SELECT bob_count() /*k:a1*/; SELECT bob_count() /*k:b1*/;",
	extra_params => [ '-U', 'alice' ]);
is(seen('bob'), 'NULL=1,b1=2',
	"definer: alice's a1 is null under bob, b1 (null for alice) is bob's");
is(seen('alice'), 'NULL=1,a1=2', "definer: the caller's statements keep her caps");

# one statement calling the definer per row: every call is capped for bob,
# and alice's statement keeps her caps
fill_caps();
$node->safe_psql('postgres', "SELECT bob_count() + bob_count() FROM t /*k:a1*/;",
	extra_params => [ '-U', 'alice' ]);
is(seen('bob') . ' ' . seen('alice'), 'NULL=4,b1=1 a1=2',
	'definer: every call is capped for bob, the caller keeps a1');

# ---------------------------------------------------------------------------
# nested_tags = scan: bob's cursors are tagged and capped as bob, then
# fetched and closed (recorded) by alice, so alice's cap applies.
# ---------------------------------------------------------------------------
fill_caps();
sql(<<"EOS");
SET $P.nested_tags = scan;
SET ROLE alice;
BEGIN;
SELECT bob_open('c1', 'b1');
SELECT bob_open('c2', 'a1');
FETCH 1 FROM c1;
FETCH 1 FROM c2;
CLOSE c1;
CLOSE c2;
COMMIT;
EOS
is(seen('alice'), 'NULL=1,a1=2',
	"definer cursor: bob's b1 is null under alice, a1 (null for bob) is alice's");
is(seen('bob'), 'b1=1', 'definer cursor: nothing is recorded under bob');

# ---------------------------------------------------------------------------
# A tagged SET ROLE is extracted under the old role and recorded under the
# new one.
# ---------------------------------------------------------------------------
fill_caps();
sql(<<"EOS");
SET $P.track_utility = on;
SET ROLE alice;
SET ROLE bob /*k:a1*/;
SET ROLE alice;
SET ROLE bob /*k:b1*/;
RESET ROLE;
EOS
is(seen('bob'), 'NULL=1,b1=2',
	"SET ROLE: alice's a1 is null under bob, b1 (null for alice) is bob's");
is(seen('alice'), 'a1=1', 'SET ROLE: nothing tagged is recorded under alice');

# ---------------------------------------------------------------------------
# Candidates past the recap budget (1 + 2 * max_tagset_bytes) are left out,
# but a set they leave out of still depends on the role: a (300 bytes, kept
# by no fill) comes first and ends the budget, k=a1 is kept for alice and
# must not reach bob's rows uncapped.
# ---------------------------------------------------------------------------
sql("ALTER SYSTEM SET $P.max_tagset_bytes = 128");
sql("ALTER SYSTEM SET $P.max_tag_value_len = 4096");
$node->restart;
my $long = 'v' x 300;
fill_caps();
sql(<<"EOS");
BEGIN;
SET ROLE alice;
SELECT open_cur('c1') /*a:$long,k:a1*/;
SET ROLE bob;
CLOSE c1;
COMMIT;
EOS
is(seen('bob'), 'b1=1', "budget, role change: alice's a1 is not recorded under bob");
is(seen('alice'), 'a1=2', 'budget, role change: alice keeps a1');
fill_caps();
$node->safe_psql('postgres', "SELECT bob_count() /*a:$long,k:a1*/;",
	extra_params => [ '-U', 'alice' ]);
is(seen('bob'), 'b1=1', "budget, definer: alice's a1 is not recorded under bob");
sql("ALTER SYSTEM RESET $P.max_tagset_bytes");
sql("ALTER SYSTEM RESET $P.max_tag_value_len");

# ---------------------------------------------------------------------------
# 'server' scope: the caps don't depend on the role, nothing is re-applied.
# ---------------------------------------------------------------------------
sql("ALTER SYSTEM SET $P.cardinality_cap_scope = 'server'");
$node->restart;
sql("SELECT ${P}_reset()");
send_as('alice', 'a1');
$node->safe_psql('postgres', "SELECT bob_count() /*k:a1*/; SELECT bob_count() /*k:b1*/;",
	extra_params => [ '-U', 'alice' ]);
is(seen('bob'), 'NULL=1,a1=1', 'server: the shared cap applies as before');

$node->stop;
done_testing();
