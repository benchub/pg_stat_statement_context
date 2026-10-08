# ProcessUtility hook (DESIGN.md §3.2, §6.6, §6.7, §6.9; backlog
# 20261005-091225-18): src/utility.c snapshots a utility frame before
# chaining, activates it around the chained call (bumping the nesting level
# except for EXECUTE and PREPARE), and records one call and the elapsed time
# of tracked utilities from the snapshot. Entries are read through the
# TEST-ONLY module test/modules/pssc_store_test, the active frame through
# test/modules/pssc_context_test.
#
# Covers: DDL recorded with its tags and pgss's queryid; EXECUTE's plan
# recorded as top level (EXECUTE and PREPARE not recorded, not nested);
# DEALLOCATE excluded on PG14-16 and recorded on PG17+; CALL/DO children
# inheriting the utility's tags with track_utility = off and on, and nested
# exactly when pgss nests them (PG17+: always; PG14-16: only when pgss's own
# track_utility and track make it track the utility, or this extension's
# settings when pgss is not loaded), checked against pgss's rows over a
# matrix of both extensions' track/track_utility settings;
# track_utility = off / track = top for nested utilities; the inner
# statement of a plain EXPLAIN nested as in pgss; COMMIT and ROLLBACK inside
# procedures and DO (also a nested CALL) recorded without crashing;
# per-(userid, queryid, toplevel) calls equal to pgss's for a utility-heavy
# workload (track = top / all); and, with the wrong load order
# (pg_stat_statements loaded after this extension), utility_missing_queryid
# counting the utilities pgss zeroed instead of recording them.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

require_testing_build();

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('utility');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P, pssc_context_test'
$P.extractors = 'sqlcommenter(position=any)'
});
$node->start;

my $have_pgss = defined pgss_suffix($node);
note("pg_stat_statements available: " . ($have_pgss ? 'yes' : 'no'));
if ($have_pgss)
{
	# The documented order: pgss first, so this extension's utility hook is
	# outside pgss's and sees the utility's queryId (DESIGN.md §3.2).
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'pg_stat_statements, $P, pssc_context_test'\n");
	$node->restart;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');
}

my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
my $pg17 = $vnum >= 170000;

$node->safe_psql('postgres', q{
CREATE EXTENSION pssc_store_test;
CREATE EXTENSION pssc_context_test;
CREATE TABLE seen(step int, tags text[], toplevel bool, nested bool,
                  frame_nesting_level int);
CREATE TABLE u_exec(i int);
CREATE TABLE u_explain(i int);
CREATE TABLE u_child(i int);
CREATE TABLE u_do(i int);

-- What a statement nested in a utility sees (its own executor frame).
CREATE PROCEDURE p_tags(s int) LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO seen SELECT s, a.tags, a.toplevel, a.nested, a.frame_nesting_level
    FROM pssc_context_test_active() a;
END $$;

-- A child statement with its own queryid.
CREATE PROCEDURE p_child() LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO u_child VALUES (1);
END $$;

CREATE PROCEDURE p_tx() LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  INSERT INTO seen (step, tags) SELECT 1, pssc_context_test_tags();
  ROLLBACK;
  INSERT INTO seen (step, tags) SELECT 2, pssc_context_test_tags();
  COMMIT;
  CREATE TEMP TABLE u_tx_tmp(i int);
  DROP TABLE u_tx_tmp;
  INSERT INTO seen (step, tags) SELECT 3, pssc_context_test_tags();
  FOR r IN SELECT g FROM generate_series(1, 3) g LOOP
    INSERT INTO seen (step, tags) SELECT 100 + r.g, pssc_context_test_tags();
    COMMIT;
  END LOOP;
  ROLLBACK;
END $$;

CREATE PROCEDURE p_outer() LANGUAGE plpgsql AS $$
BEGIN
  CALL p_tx();
  COMMIT;
END $$;

-- Recorded entries of this database, summed over buckets.
CREATE VIEW rec AS
  SELECT userid, queryid, toplevel, tags, sum(calls) AS calls,
         sum(total_exec_time) AS total_exec_time
    FROM pssc_store_test_entries()
   WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
   GROUP BY 1, 2, 3, 4;
});

sub sql { return $node->safe_psql('postgres', $_[0]); }

# psql on stdin; stops at the first error.
sub run
{
	my ($input) = @_;
	my ($out, $err);
	$node->psql('postgres', $input, stdout => \$out, stderr => \$err,
		extra_params => [ '-v', 'ON_ERROR_STOP=1' ]);
	return ($out, $err);
}

sub set_conf
{
	my (%kv) = @_;
	for my $k (sort keys %kv)
	{
		sql("ALTER SYSTEM SET $P.$k = '$kv{$k}'");
	}
	sql('SELECT pg_reload_conf()');
	for my $k (sort keys %kv)
	{
		$node->poll_query_until('postgres', "SHOW $P.$k", $kv{$k})
		  or die "$P.$k did not become $kv{$k}";
	}
}

sub qid
{
	my ($stmt) = @_;
	my $out = sql("EXPLAIN (VERBOSE, COSTS OFF) $stmt");
	$out =~ /Query Identifier: (-?\d+)/
	  or die "no query identifier for $stmt: $out";
	return $1;
}

my $reset_sql = 'SELECT pssc_store_test_reset()'
  . ($have_pgss ? ', pg_stat_statements_reset()' : '');

sub reset_all { sql($reset_sql); }

sub counter
{
	return sql("SELECT $_[0] FROM pssc_store_test_counters()");
}

# Recorded rows with the given tag value: "toplevel|calls" lines.
sub rec_tagged
{
	my ($v) = @_;
	return sql(
		"SELECT toplevel, calls FROM rec WHERE tags = '{controller,$v}' ORDER BY 1");
}

sub rec_of
{
	my ($qid) = @_;
	return sql(
		"SELECT toplevel, tags, calls FROM rec WHERE queryid = $qid ORDER BY 1, 2::text");
}

my %q = (
	exec => qid('SELECT count(*) FROM u_exec'),
	explain => qid('SELECT count(*) FROM u_explain'),
	child => qid('INSERT INTO u_child VALUES (1)'),
	do => qid('INSERT INTO u_do VALUES (1)'),
);

my $missing0 = counter('utility_missing_queryid');

# ------------------------------------------------- DDL with tags

reset_all();
{
	my ($out, $err) = run(qq{
CREATE TABLE u_ddl(i int) /*controller='ddl'*/;
SELECT 1 \\; ALTER TABLE u_ddl ADD COLUMN j int /*controller='alter'*/ \\; SELECT 2;
});
	is($err, '', 'DDL ran');
	is(rec_tagged('ddl'), 't|1', 'CREATE TABLE recorded once, top level, with its tags');
	is(rec_tagged('alter'), 't|1',
		'utility in a multi-statement string recorded with its own tags');
	is(sql(q{SELECT count(*) FROM rec WHERE tags = '{controller,ddl}'
	           AND queryid <> 0 AND total_exec_time > 0}), '1',
		'utility has a queryid and a measured time');
  SKIP:
	{
		skip 'pg_stat_statements not installed', 2 unless $have_pgss;
		is( sql(q{SELECT r.queryid = s.queryid FROM rec r, pg_stat_statements s
				   WHERE r.tags = '{controller,ddl}' AND s.query LIKE 'CREATE TABLE u_ddl%'}),
			't', 'CREATE TABLE queryid equals pg_stat_statements\'');
		is( sql(q{SELECT r.queryid = s.queryid FROM rec r, pg_stat_statements s
				   WHERE r.tags = '{controller,alter}' AND s.query LIKE 'ALTER TABLE u_ddl%'}),
			't', 'ALTER TABLE (multi-statement) queryid equals pg_stat_statements\'');
	}
}

# ------------------------------------------------- EXECUTE / PREPARE

reset_all();
{
	my ($out, $err) = run(qq{
PREPARE pu AS SELECT count(*) FROM u_exec /*controller='prep'*/;
EXECUTE pu /*controller='exec'*/;
EXECUTE pu /*controller='exec'*/;
PREPARE pa AS SELECT tags, toplevel, nested, frame_nesting_level
  FROM pssc_context_test_active() /*controller='pa'*/;
EXECUTE pa;
});
	is($err, '', 'PREPARE/EXECUTE ran');
	my @l = split /\n/, $out;
	is($l[-1], '{controller=pa}|t|f|0',
		'EXECUTE neither bumps the nesting level nor activates a frame');
	is(rec_of($q{exec}), 't|{controller,prep}|2',
		'EXECUTE: the plan is recorded as top level, with the PREPARE text\'s tags');
	is(rec_tagged('exec'), '', 'EXECUTE itself is not recorded');
	is(rec_tagged('prep'), 't|2', 'PREPARE itself is not recorded');
  SKIP:
	{
		skip 'pg_stat_statements not installed', 1 unless $have_pgss;
		is( sql(
				"SELECT toplevel, calls FROM pg_stat_statements WHERE queryid = $q{exec}"),
			't|2', 'pgss also records the plan as top level');
	}
}

# ------------------------------------------------- DEALLOCATE (per version)

reset_all();
{
	my ($out, $err) = run(qq{
PREPARE pd AS SELECT 1;
DEALLOCATE pd /*controller='dealloc'*/;
});
	is($err, '', 'DEALLOCATE ran');
	is(rec_tagged('dealloc'), ($pg17 ? 't|1' : ''),
		'DEALLOCATE recorded on PG17+ only');
  SKIP:
	{
		skip 'pg_stat_statements not installed', 1 unless $have_pgss;
		is( sql(q{SELECT count(*) FROM pg_stat_statements WHERE query LIKE 'DEALLOCATE%'}),
			($pg17 ? '1' : '0'), 'pgss agrees on DEALLOCATE');
		if ($pg17)
		{
			is( sql(q{SELECT r.queryid = s.queryid FROM rec r, pg_stat_statements s
					   WHERE r.tags = '{controller,dealloc}' AND s.query LIKE 'DEALLOCATE%'}),
				't', 'DEALLOCATE queryid equals pg_stat_statements\'');
		}
	}
}

# ------------------------------------------------- CALL / DO children
#
# Whether a CALL/DO is a nesting level for its children follows pgss's
# settings (see "nesting follows pgss's settings" below): always on PG17+; on
# PG14-16 only when pgss tracks the utility. pgss's defaults here
# (track_utility = on, track = top) do; without pgss loaded, this extension's
# own settings stand in for them, so with track_utility = off the children
# are top level.

for my $tu ('off', 'on')
{
	my $nested = $pg17 || $have_pgss || $tu eq 'on';
	my ($tl, $lvl, $how) = $nested ? ('f', 1, 'nested') : ('t', 0, 'top level');
	reset_all();
	my ($out, $err) = run(qq{
SET $P.track = 'all';
SET $P.track_utility = $tu;
TRUNCATE seen;
CALL p_tags(1) /*controller='call_$tu'*/;
DO \$\$ BEGIN
  INSERT INTO seen SELECT 2, a.tags, a.toplevel, a.nested, a.frame_nesting_level
    FROM pssc_context_test_active() a;
END \$\$ /*controller='do_$tu'*/;
CALL p_child() /*controller='child_$tu'*/;
SELECT step, tags, toplevel, nested, frame_nesting_level FROM seen ORDER BY step;
});
	is($err, '', "track_utility = $tu: CALL/DO ran");
	is_deeply([ split /\n/, $out ],
		[ "1|{controller=call_$tu}|$tl|t|$lvl", "2|{controller=do_$tu}|$tl|t|$lvl" ],
		"track_utility = $tu: CALL/DO children are $how and inherit the utility's tags");
	is(rec_of($q{child}), "$tl|{controller,child_$tu}|1",
		"track_utility = $tu: CALL child recorded $how with the CALL's tags");
	is(rec_tagged("child_$tu"), ($tu eq 'on' ? "f|1\nt|1" : "$tl|1"),
		"track_utility = $tu: CALL itself recorded only when tracking utilities");
	is(rec_tagged("call_$tu"), ($tu eq 'on' ? "f|1\nt|1" : "$tl|1"),
		"track_utility = $tu: CALL p_tags likewise");
}

{
	# track = top: a nested utility is not recorded, its parent is.
	reset_all();
	my ($out, $err) = run(qq{
DO \$\$ BEGIN CREATE TEMP TABLE u_nested_tmp(i int); END \$\$ /*controller='do_top'*/;
});
	is($err, '', 'track = top: nested utility ran');
	is(rec_tagged('do_top'), 't|1',
		'track = top: DO recorded, the nested CREATE TABLE not');

	reset_all();
	($out, $err) = run(qq{
SET $P.track = 'all';
DO \$\$ BEGIN CREATE TEMP TABLE u_nested_tmp(i int); END \$\$ /*controller='do_all'*/;
});
	is(rec_tagged('do_all'), "f|1\nt|1",
		'track = all: the nested CREATE TABLE is recorded, nested, inheriting tags');
}

# ------------------------------------------------- nesting follows pgss's settings
#
# A utility other than EXECUTE/PREPARE is a nesting level for its children
# exactly when pgss makes it one, so a CALL/DO child's toplevel always
# matches pgss's key: on PG17+ always; on PG14-16 only when pgss tracks the
# utility (pg_stat_statements.track_utility and pgss_enabled(level), i.e.
# track = all, or top at level 0). This extension's own settings only decide
# what it records.

SKIP:
{
	skip 'pg_stat_statements not installed', 2 * 3 * 2 * 2 * 7
	  unless $have_pgss;
	for my $ptu ('on', 'off')
	{
		for my $ptrack ('none', 'top', 'all')
		{
			for my $tu ('on', 'off')
			{
				for my $track ('top', 'all')
				{
					my $c = "pgss $ptu/$ptrack, ours $tu/$track";
					my $nested = $pg17 || ($ptu eq 'on' && $ptrack ne 'none');
					my $tl = $nested ? 'f' : 't';
					my $we = $track eq 'all' || !$nested;
					my $they = $ptrack eq 'all' || ($ptrack eq 'top' && !$nested);
					reset_all();
					my ($out, $err) = run(qq{
SET pg_stat_statements.track_utility = $ptu;
SET pg_stat_statements.track = '$ptrack';
SET $P.track_utility = $tu;
SET $P.track = '$track';
TRUNCATE seen;
CALL p_child() /*controller='mc'*/;
DO \$\$ BEGIN INSERT INTO u_do VALUES (1); END \$\$ /*controller='md'*/;
CALL p_tags(1) /*controller='mt'*/;
SELECT toplevel FROM seen;
});
					is($err, '', "$c: ran");
					is($out, $tl, "$c: CALL child's toplevel as pgss would key it");
					is(rec_of($q{child}), ($we ? "$tl|{controller,mc}|1" : ''),
						"$c: CALL child recorded per our track, with pgss's toplevel");
					is(rec_of($q{do}), ($we ? "$tl|{controller,md}|1" : ''),
						"$c: DO child likewise");
					is( sql("SELECT string_agg(CASE WHEN toplevel THEN 't' ELSE 'f' END || '|' || calls, ',')
					           FROM pg_stat_statements WHERE queryid IN ($q{child}, $q{do})"),
						($they ? "$tl|1,$tl|1" : ''),
						"$c: pgss keys the children the same way");
					is( sql(qq{SELECT count(*) FROM rec r JOIN pg_stat_statements s
					             USING (userid, queryid, toplevel)
					           WHERE queryid IN ($q{child}, $q{do}) AND r.calls = s.calls}),
						($we && $they ? 2 : 0),
						"$c: children join pgss rows on (userid, queryid, toplevel)");
					is( sql(qq{SELECT count(*) FROM rec r LEFT JOIN pg_stat_statements s
					             USING (userid, queryid, toplevel)
					           WHERE queryid IN ($q{child}, $q{do}) AND s.queryid IS NULL
					             AND EXISTS (SELECT FROM pg_stat_statements p
					                          WHERE p.queryid = r.queryid)}),
						0, "$c: no child row keyed differently from pgss's");
				}
			}
		}
	}
}

# ------------------------------------------------- EXPLAIN (no ANALYZE)

reset_all();
{
	my $pgss_set = $have_pgss ? "SET pg_stat_statements.track = 'all';" : '';
	my ($out, $err) = run(qq{
SET $P.track = 'all'; $pgss_set
EXPLAIN (COSTS OFF) SELECT count(*) FROM u_explain /*controller='ex'*/;
});
	is($err, '', 'EXPLAIN ran');
	is(rec_of($q{explain}), 'f|{controller,ex}|1',
		'inner statement of a plain EXPLAIN is nested, inheriting the tags');
	is(rec_tagged('ex'), "f|1\nt|1", 'and EXPLAIN itself is recorded at top level');
  SKIP:
	{
		skip 'pg_stat_statements not installed', 1 unless $have_pgss;
		is( sql(
				"SELECT toplevel, calls FROM pg_stat_statements WHERE queryid = $q{explain}"),
			'f|1', 'pgss counts it as nested too');
	}
}

# ------------------------------------------------- COMMIT / ROLLBACK inside procedures

reset_all();
{
	my $pgss_set = $have_pgss ? "SET pg_stat_statements.track = 'all';" : '';
	my ($out, $err) = run(qq{
SET $P.track = 'all'; $pgss_set
TRUNCATE seen;
CALL p_tx() /*controller='tx'*/;
CALL p_outer() /*controller='tx_outer'*/;
DO \$\$ BEGIN
  INSERT INTO seen (step, tags) SELECT 10, pssc_context_test_tags();
  COMMIT;
  INSERT INTO seen (step, tags) SELECT 11, pssc_context_test_tags();
  ROLLBACK;
  INSERT INTO seen (step, tags) SELECT 12, pssc_context_test_tags();
END \$\$ /*controller='tx_do'*/;
BEGIN;
CREATE TABLE u_rb(i int);
ROLLBACK /*controller='rb'*/;
BEGIN /*controller='begin'*/;
COMMIT /*controller='commit'*/;
SELECT step, tags FROM seen ORDER BY step, tags::text COLLATE "C";
});
	is($err, '', 'COMMIT/ROLLBACK in procedures ran');
	is_deeply([ split /\n/, $out ], [
		'2|{controller=tx_outer}', '2|{controller=tx}',
		'3|{controller=tx_outer}', '3|{controller=tx}',
		'10|{controller=tx_do}', '12|{controller=tx_do}',
		'101|{controller=tx_outer}', '101|{controller=tx}',
		'102|{controller=tx_outer}', '102|{controller=tx}',
		'103|{controller=tx_outer}', '103|{controller=tx}',
	], 'children keep the utility frame\'s tags across COMMIT and ROLLBACK');
	is(sql(q{SELECT calls FROM rec WHERE tags = '{controller,tx}' AND toplevel}),
		'1', 'CALL with COMMIT/ROLLBACK recorded once at top level');
	is(sql(q{SELECT calls FROM rec WHERE tags = '{controller,tx_outer}' AND toplevel}),
		'1', 'CALL of a procedure that CALLs one with COMMIT recorded at top level');
	is(sql(q{SELECT count(*) FROM rec WHERE tags = '{controller,tx_do}' AND toplevel}),
		'1', 'DO with COMMIT/ROLLBACK recorded at top level');
	is(rec_tagged('rb'), 't|1', 'top-level ROLLBACK recorded with its tags');
	is(rec_tagged('begin'), 't|1', 'BEGIN recorded with its tags');
	is(rec_tagged('commit'), 't|1', 'COMMIT recorded with its tags');
  SKIP:
	{
		skip 'pg_stat_statements not installed', 2 unless $have_pgss;
		# (On PG14/15 a utility's queryid hashes its text, so the nested
		# "CALL p_tx()" is found through pgss's text.)
		is( sql(q{SELECT r.calls FROM rec r JOIN pg_stat_statements s
		             USING (userid, queryid, toplevel)
		           WHERE r.tags = '{controller,tx_outer}' AND NOT r.toplevel
		             AND s.query = 'CALL p_tx()'}),
			'1', 'nested CALL with COMMIT recorded nested, inheriting tags');
		is( sql(q{SELECT count(*) FROM rec r JOIN pg_stat_statements s
		             USING (userid, queryid, toplevel)
		           WHERE r.tags IN ('{controller,tx}', '{controller,tx_outer}',
		                            '{controller,tx_do}', '{controller,rb}')
		             AND r.calls = s.calls AND r.toplevel}),
			'4', 'their queryids and calls equal pgss\'s');
	}
}

# ------------------------------------------------- queryid / calls parity with pgss

SKIP:
{
	skip 'pg_stat_statements not installed', 4 unless $have_pgss;
	set_conf(untagged => 'record');

	my $workload = q{
CREATE TABLE u_par(i int) /*controller='p'*/;
INSERT INTO u_par VALUES (1), (2);
ALTER TABLE u_par ADD COLUMN j int;
CREATE INDEX u_par_i ON u_par(i) /*controller='idx'*/;
CREATE TABLE u_ctas AS SELECT * FROM u_par;
SELECT 1 \; DROP TABLE u_ctas /*controller='drop'*/ \; SELECT 2;
BEGIN;
DECLARE c CURSOR FOR SELECT * FROM u_par;
FETCH 1 FROM c;
CLOSE c;
COMMIT;
EXPLAIN (COSTS OFF) SELECT * FROM u_par WHERE i > 0;
PREPARE pp(int) AS SELECT * FROM u_par WHERE i = $1;
EXECUTE pp(1);
EXECUTE pp(2);
DEALLOCATE pp;
CALL p_tags(9);
CALL p_child() /*controller='c'*/;
DO $$ BEGIN PERFORM 1; CREATE TEMP TABLE u_tmp(i int); DROP TABLE u_tmp; END $$;
SET work_mem = '8MB';
RESET work_mem;
VACUUM u_par;
ANALYZE u_par;
TRUNCATE u_par;
CALL p_tx();
DROP TABLE u_par;
};

	my $parity_sql = q{
SELECT coalesce(r.queryid, s.queryid), coalesce(r.toplevel, s.toplevel),
       r.calls, s.calls, s.query
  FROM (SELECT userid, queryid, toplevel, sum(calls) AS calls FROM rec
         GROUP BY 1, 2, 3) r
  FULL JOIN (SELECT userid, queryid, toplevel, calls, query FROM pg_stat_statements
              WHERE dbid = (SELECT oid FROM pg_database
                             WHERE datname = current_database())) s
       USING (userid, queryid, toplevel)
 WHERE r.calls IS DISTINCT FROM s.calls
 ORDER BY 1, 2
};

	for my $track ('top', 'all')
	{
		my ($out, $err) = run(
			"SET $P.track = '$track'; SET pg_stat_statements.track = '$track';\n"
			  . "$reset_sql;\n$workload\n"
			  . "SELECT 'parity', count(*) FROM ($parity_sql) d;\n$parity_sql;\n");
		is($err, '', "track = $track: utility workload ran");
		my ($parity) = grep { /^parity\|/ } split /\n/, $out;
		is($parity, 'parity|0',
			"track = $track: utility queryids and calls per (userid, queryid, toplevel) equal pgss's")
		  or diag($out);
	}
	set_conf(untagged => 'skip');
}

is(counter('utility_missing_queryid'), $missing0,
	'documented load order: no utility arrived without a queryid');

# ------------------------------------------------- wrong load order

SKIP:
{
	skip 'pg_stat_statements not installed', 9 unless $have_pgss;
	# pgss's hook now runs outside this extension's and zeroes the
	# utility's queryId before chaining (when it tracks utilities).
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = '$P, pg_stat_statements, pssc_context_test'\n");
	$node->restart;

	reset_all();
	my $m0 = counter('utility_missing_queryid');
	my ($out, $err) = run(qq{
CREATE TABLE u_wrong(i int) /*controller='wrong'*/;
});
	is($err, '', 'wrong order: DDL ran');
	is(counter('utility_missing_queryid') - $m0, 1,
		'wrong order: tracked utility with queryId 0 counted');
	is(rec_tagged('wrong'), '', 'wrong order: and not recorded');

	# (The SET itself is tracked when it starts, so it is counted.)
	($out, $err) = run(qq{
SET $P.track_utility = off;
SELECT 'm', utility_missing_queryid FROM pssc_store_test_counters();
DROP TABLE u_wrong /*controller='untracked'*/;
SELECT 'm', utility_missing_queryid FROM pssc_store_test_counters();
});
	my @m = map { (split /\|/)[1] } grep { /^m\|/ } split /\n/, $out;
	is(scalar(@m) == 2 && $m[1] - $m[0], 0,
		'wrong order: untracked utility not counted');

	$m0 = counter('utility_missing_queryid');
	($out, $err) = run(qq{
PREPARE pw AS SELECT count(*) FROM u_exec /*controller='wexec'*/;
EXECUTE pw;
DEALLOCATE pw;
});
	is($err, '', 'wrong order: PREPARE/EXECUTE ran');
	is(counter('utility_missing_queryid') - $m0, ($pg17 ? 1 : 0),
		'wrong order: PREPARE/EXECUTE never counted (DEALLOCATE on PG17+ only)');
	is(rec_of($q{exec}), 't|{controller,wexec}|1',
		'wrong order: EXECUTE\'s plan still recorded at top level');

	($out, $err) = run(qq{
SET pg_stat_statements.track_utility = off;
CREATE TABLE u_wrong2(i int) /*controller='wrong2'*/;
});
	is($err, '', 'wrong order, pgss track_utility = off: DDL ran');
	is(rec_tagged('wrong2'), 't|1',
		'wrong order: recorded when pgss leaves queryId alone');
}

# ------------------------------------------------- pgss not loaded

# Without pgss, PG14-16 nest by this extension's track_utility/track, as
# pgss would with those settings; pgss's GUCs set as placeholders (pgss not
# loaded) are ignored.
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = '$P, pssc_context_test'\n");
$node->restart;
for my $tu ('on', 'off')
{
	my ($out, $err) = run(qq{
SET pg_stat_statements.track_utility = off;
SET pg_stat_statements.track = 'none';
SET $P.track = 'all';
SET $P.track_utility = $tu;
TRUNCATE seen;
CALL p_tags(1) /*controller='np'*/;
SELECT toplevel FROM seen;
});
	is($err, '', "no pgss, track_utility = $tu: ran");
	is($out, (($pg17 || $tu eq 'on') ? 'f' : 't'),
		"no pgss, track_utility = $tu: CALL child nested per our settings (always on PG17+)");
}

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
