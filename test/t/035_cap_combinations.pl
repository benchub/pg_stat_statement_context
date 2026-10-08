# Per-key cardinality caps don't bound tag-set combinations (DESIGN.md
# §6.1, §5.3; backlog 20261008-065635-11, docs/extractors.md "Caps bound
# values, not combinations"): with k kept keys each capped at N values, one
# (role, database, queryid) can still produce N^k entries, (N + 1)^k with
# the collapsed JSON null, and no cap event is counted while every value is
# within its cap. The only signals are the table's: entries, dealloc and
# evicted_entries.
#
# Covers: cap 5, max_entries 100, the three default keys and their first 5
# values each: 125 entries for one query, so 25 live entries evicted in 5
# passes (5% of max_entries each) and zero capped_tags / cap_table_full;
# with max_entries 300 and 7 values per key, 6^3 = 216 entries (5 strings
# and null per key), no eviction, and capped_tags counting each collapsed
# value.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('cap_combinations');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.track_utility = off
$P.cardinality_cap = 5
$P.max_entries = 100
$P.save = off
EOC
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql("CREATE EXTENSION $P; CREATE TABLE t(i int)");

# One statement per (action, controller, job) combination of values 1..n,
# all of the same query, in one multi-statement query string.
sub flood
{
	my ($n) = @_;
	my $q = '';
	for my $a (1 .. $n)
	{
		for my $c (1 .. $n)
		{
			for my $j (1 .. $n)
			{
				$q .= "SELECT * FROM t /*action='a$a',controller='c$c',job='j$j'*/;\n";
			}
		}
	}
	sql($q);
}

sub counters
{
	return sql("SELECT entries || ' ' || dealloc || ' ' || reclaimed_entries"
		  . " || ' ' || evicted_entries || ' ' || dropped_records"
		  . " || ' ' || capped_tags || ' ' || cap_table_full"
		  . " FROM ${P}_counters()");
}

# "entries queries strings nulls" over the view: entries, distinct queryids,
# then the distinct string values and whether null occurs, per key.
sub view_shape
{
	return sql("SELECT count(*) || ' ' || count(DISTINCT queryid) || ' ' || "
		  . join(" || ' ' || ",
			map {
				"count(DISTINCT tags->>'$_') FILTER (WHERE jsonb_typeof(tags->'$_') = 'string')"
				  . " || '+' || (count(*) FILTER (WHERE jsonb_typeof(tags->'$_') = 'null') > 0)::int"
			} qw(action controller job))
		  . " FROM ${P}_totals");
}

# The vetter's reproduction: 5^3 = 125 combinations, each value within its
# cap, against a table of 100 entries.
sql("SELECT ${P}_reset()");
flood(5);
is(counters(), '100 5 0 25 0 0 0',
	'cap 5, 3 keys: 125 combinations of one query fill max_entries 100 and evict 25 live entries in 5 passes, with no cap event');
is(view_shape(), '100 1 5+0 5+0 5+0',
	'the surviving 100 entries all belong to one query and hold only the 5 admitted values of each key');

# With room for them, every combination is an entry of its own: (N + 1)^k,
# since null is one more value of each key.
$node->append_conf('postgresql.conf', "$P.max_entries = 300\n");
$node->restart;
flood(7);
is(counters(), '216 0 0 0 0 294 0',
	'7 values per key under cap 5: 6^3 = 216 entries for one query (5 strings and null per key), 294 values collapsed, no eviction');
is(view_shape(), '216 1 5+1 5+1 5+1',
	'each key holds its 5 admitted values plus null, and every combination of them is an entry');

$node->stop;
done_testing();
