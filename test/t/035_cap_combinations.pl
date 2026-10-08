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
# with max_entries 400 and 7 values per key, 6^3 = 216 entries (5 strings
# and null per key), no eviction, and capped_tags counting each collapsed
# value; with each key also optional, 7^3 - 1 = 342 non-empty tag sets
# (absent is one more state of each key, the empty set is not recorded with
# untagged = skip).
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
# all of the same query, in one multi-statement query string. With
# $optional, value 0 of a key leaves the key out (and a statement with no
# key has no comment).
sub flood
{
	my ($n, $optional) = @_;
	my $from = $optional ? 0 : 1;
	my $q = '';
	for my $a ($from .. $n)
	{
		for my $c ($from .. $n)
		{
			for my $j ($from .. $n)
			{
				my @tags = (
					$a ? ("action='a$a'") : (),
					$c ? ("controller='c$c'") : (),
					$j ? ("job='j$j'") : ());
				$q .= "SELECT * FROM t"
				  . (@tags ? ' /*' . join(',', @tags) . '*/' : '') . ";\n";
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
# Which entries survive depends on eviction order (last_bucket first), so
# a bucket boundary during the flood changes the values left; check only
# that the survivors are one query's and use admitted values.
is(sql("SELECT count(*) || ' ' || count(DISTINCT queryid) || ' ' || "
		  . join(" || ' ' || ",
			map {
				my $k = $_;
				my $p = substr($k, 0, 1);
				"bool_and(jsonb_typeof(tags->'$k') = 'string' AND tags->>'$k' IN ("
				  . join(',', map { "'$p$_'" } 1 .. 5) . "))::int"
			} qw(action controller job))
		  . " FROM ${P}_totals"),
	'100 1 1 1 1',
	'the surviving 100 entries all belong to one query and hold only admitted values of each key');

# With room for them, every combination is an entry of its own: (N + 1)^k,
# since null is one more value of each key.
$node->append_conf('postgresql.conf', "$P.max_entries = 400\n");
$node->restart;
flood(7);
is(counters(), '216 0 0 0 0 294 0',
	'7 values per key under cap 5: 6^3 = 216 entries for one query (5 strings and null per key), 294 values collapsed, no eviction');
is(view_shape(), '216 1 5+1 5+1 5+1',
	'each key holds its 5 admitted values plus null, and every combination of them is an entry');

# Optional keys: absent is distinct from every string and from null, so
# each key has N + 2 states: (N + 2)^k - 1 non-empty tag sets. 8^3 - 1
# statements, of which 3 * 2 * 8 * 8 = 384 values collapse.
sql("SELECT ${P}_reset()");
flood(7, 1);
is(counters(), '342 0 0 0 0 384 0',
	'7 optional values per key under cap 5: 7^3 - 1 = 342 entries for one query, no eviction');
is(sql("SELECT count(*) || ' ' || count(DISTINCT queryid) || ' ' || count(DISTINCT tags)"
		  . " || ' ' || count(*) FILTER (WHERE NOT tags ? 'action')"
		  . " FROM ${P}_totals"),
	'342 1 342 48',
	'every (absent | null | 5 strings)^3 tag set but the empty one is an entry; 7^2 - 1 lack action');

$node->stop;
done_testing();
