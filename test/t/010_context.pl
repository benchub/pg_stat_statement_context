# Execution frames and active-frame tracking (DESIGN.md §3.1 item 3, §3.2
# "Frame lifetime", §6.4, §6.5, §6.9; backlog 20261005-091225-16):
# src/context.c, driven by the main library's executor hooks (src/executor.c)
# and the ProcessUtility hook of the TEST-ONLY module
# test/modules/pssc_context_test, which also observes the frames found at
# ExecutorEnd (make install-test-modules).
#
# Covers: the frame registry is empty at every transaction end (counted by
# pssc_frame_xact_stats in every build, an assertion in assert builds) after
# normal execution, errors (also in nested PL/pgSQL, caught and uncaught),
# portals dropped without ExecutorEnd (failed cursors, aborted
# transactions and subtransactions with open cursors), suspended and
# interleaved portals, COMMIT/ROLLBACK inside procedures (utility snapshot,
# pinned portal persisted at COMMIT); frame lookup by QueryDesc; save/restore of the active frame
# and nesting level; nested_tags inherit/scan/none for PL/pgSQL, BEFORE
# and AFTER triggers and DO; constant-folded functions getting their own
# tags; the PG18 owned start of later statements in a multi-statement
# string, also with plan-time SQL (constant folding) and EXECUTE between
# them; planning counted as a nesting level on PG17+ only, as pgss does
# (inner SQL of folded functions is then not top level); the user of a frame refreshed at End; parity with
# pg_stat_statements where its library is installed.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('context');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P, pssc_context_test'
$P.extractors = 'sqlcommenter(position=any)'
});
$node->start;

# Load pg_stat_statements first (as recommended) where it is installed
# (the harness images have it), for parity checks.
my $pkglibdir = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
my $have_pgss = -e "$pkglibdir/pg_stat_statements.so";
note("pg_stat_statements available: " . ($have_pgss ? 'yes' : 'no'));
if ($have_pgss)
{
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'pg_stat_statements, $P, pssc_context_test'\n"
		  . "pg_stat_statements.track = 'all'\n");
	$node->restart;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');
}

my $pg17 = $node->safe_psql('postgres', 'SHOW server_version_num') >= 170000;
# pgss counts planning as a nesting level on PG17+ only (src/compat.h).
my $folded_top = $pg17 ? 'f' : 't';

my $assert_build = $node->safe_psql('postgres', 'SHOW debug_assertions') eq 'on';
note("assert-enabled build: " . ($assert_build ? 'yes' : 'no'));

$node->safe_psql('postgres', q{
CREATE EXTENSION pssc_context_test;
CREATE TABLE seen(step int, tags text[]);
CREATE ROLE ctx_a;
CREATE TABLE trg_t(i int);
CREATE TABLE trg_log(kind text, tags text[]);

-- (PL/pgSQL drops the text after INTO from the query, so the comments
-- go before it.)
CREATE FUNCTION f_nested() RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE r text[];
BEGIN
  SELECT pssc_context_test_tags() /*controller='inner'*/ INTO r;
  RETURN r;
END $$;

CREATE FUNCTION f_imm(int) RETURNS text[] LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE r text[];
BEGIN
  SELECT pssc_context_test_tags() /*controller='folded'*/ INTO r;
  RETURN r;
END $$;

-- A folded function whose SQL jumbles uniquely (pgss ignores comments, so
-- all "SELECT pssc_context_test_tags() /*...*/" share one entry).
CREATE FUNCTION f_fold_marker() RETURNS bigint LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE r bigint;
BEGIN
  SELECT count(*) INTO r FROM pg_namespace fold_n1, pg_namespace fold_n2
    WHERE fold_n1.oid = fold_n2.oid;
  RETURN r;
END $$;

CREATE FUNCTION f_vol(int) RETURNS text[] LANGUAGE plpgsql VOLATILE AS $$
DECLARE r text[];
BEGIN
  SELECT pssc_context_test_tags() /*controller='folded'*/ INTO r;
  RETURN r;
END $$;

CREATE FUNCTION trg_before() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO trg_log SELECT 'before', pssc_context_test_tags() /*controller='trig_before'*/;
  RETURN NEW;
END $$;
CREATE FUNCTION trg_after() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO trg_log SELECT 'after', pssc_context_test_tags() /*controller='trig_after'*/;
  RETURN NULL;
END $$;
CREATE TRIGGER b BEFORE INSERT ON trg_t FOR EACH ROW EXECUTE FUNCTION trg_before();
CREATE TRIGGER a AFTER INSERT ON trg_t FOR EACH ROW EXECUTE FUNCTION trg_after();

CREATE FUNCTION f_err(depth int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE r int;
BEGIN
  IF depth > 0 THEN
    SELECT f_err(depth - 1) INTO r;
  END IF;
  SELECT 1 / (g - 3) INTO r FROM generate_series(1, 5) g ORDER BY g DESC;
  RETURN r;
END $$;

CREATE FUNCTION f_catch(OUT tags text[], OUT nesting_level int,
                        OUT frame_nesting_level int) LANGUAGE plpgsql AS $$
DECLARE r int;
BEGIN
  BEGIN
    SELECT f_err(2) INTO r;
  EXCEPTION WHEN division_by_zero THEN
    SELECT a.tags, a.nesting_level, a.frame_nesting_level
      INTO tags, nesting_level, frame_nesting_level
      FROM pssc_context_test_active() a /*controller='handler'*/;
  END;
END $$;

-- Cursors with query IDs: SPI plans them (DECLARE CURSOR's inner query
-- has queryId 0, so its executor gets no frame).
CREATE FUNCTION open_cur(name text, fail bool DEFAULT false) RETURNS refcursor
LANGUAGE plpgsql AS $$
DECLARE c refcursor := name;
BEGIN
  IF fail THEN
    OPEN c FOR SELECT 1 / (3 - g), pssc_context_test_tags() FROM generate_series(1, 5) g;
  ELSE
    OPEN c FOR SELECT g, pssc_context_test_tags() FROM generate_series(1, 3) g;
  END IF;
  RETURN c;
END $$;

-- The FOR loop's portal is pinned: each COMMIT persists it (runs it to
-- completion, rows 11-25 after the first prefetch of 10, then ExecutorEnd).
CREATE PROCEDURE p_tx() LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  INSERT INTO seen SELECT 1, pssc_context_test_tags();
  ROLLBACK;
  INSERT INTO seen SELECT 2, pssc_context_test_tags();
  COMMIT;
  INSERT INTO seen SELECT 3, pssc_context_test_tags();
  FOR r IN SELECT g, pssc_context_test_tags() AS t FROM generate_series(1, 25) g LOOP
    INSERT INTO seen SELECT 100 + r.g, pssc_context_test_tags();
    INSERT INTO seen VALUES (200 + r.g, r.t);
    COMMIT;
  END LOOP;
END $$;
});

# Runs $sql (statements may fail) in a new session; returns (stdout, stderr).
sub run
{
	my ($sql) = @_;
	my ($out, $err) = ('', '');
	$node->psql('postgres', $sql, stdout => \$out, stderr => \$err,
		on_error_stop => 0);
	return ($out, $err);
}

# The registry and nesting state checked at the end of a session.
my $CHECK = q{
SELECT 'reg', live, xact_checks > 0, xact_leaks FROM pssc_context_test_registry();
SELECT 'miss', pssc_context_test_end_misses();
SELECT 'nest', nesting_level, frame_nesting_level, utility FROM pssc_context_test_active();
};

# Runs a scenario, then checks in the same session that only the checking
# statement's own frame is registered, that transaction ends were checked
# and none found a frame, that every ExecutorEnd found its frame, and that
# the active frame and nesting level were restored. Returns the scenario's
# output lines.
sub scenario
{
	my ($name, $sql, $err_re) = @_;
	my ($out, $err) = run($sql . $CHECK);
	my @l = split /\n/, $out;
	my @tail = splice(@l, -3);
	is_deeply(\@tail, [ 'reg|1|t|0', 'miss|0', 'nest|1|0|f' ],
		"$name: registry empty at transaction end, state restored");
	if (defined $err_re)
	{
		like($err, $err_re, "$name: expected error");
	}
	else
	{
		is($err, '', "$name: no error");
	}
	return @l;
}

# --- registry lifetime ------------------------------------------------------

scenario('normal execution', q{
SELECT 1;
BEGIN; SELECT g FROM generate_series(1, 3) g; SELECT 2; COMMIT;
});

scenario('top-level error', q{SELECT 1 / (g - 2) FROM generate_series(1, 3) g;},
	qr/division by zero/);

scenario('error in nested PL/pgSQL', q{
SELECT f_err(3) /*controller='outer'*/;
BEGIN; SELECT f_err(2); ROLLBACK;
}, qr/division by zero/);

{
	my @l = scenario('error caught in PL/pgSQL', q{
SELECT * FROM f_catch() /*controller='outer'*/;
});
	# The handler's statement runs at level 2 (top-level Run, its own Run);
	# its frame was made at level 1 and inherits the top-level tags, so
	# the levels and active frame of the failed inner calls were restored.
	is_deeply(\@l, ['{controller=outer}|2|1'],
		'caught error: handler sees restored frame and nesting level');
}

{
	my @l = scenario('failed cursor (portal dropped without ExecutorEnd)', q{
BEGIN;
SELECT open_cur('c', true);
SELECT 'live', live FROM pssc_context_test_registry();
FETCH ALL FROM c;
ROLLBACK;
}, qr/division by zero/);
	is_deeply(\@l, [ 'c', 'live|2' ], 'failed cursor had a frame');
}

{
	my @l = scenario('transaction aborted by error with open cursor', q{
BEGIN;
SELECT open_cur('c');
FETCH 1 FROM c;
SELECT 'live', live FROM pssc_context_test_registry();
SELECT 1 / 0;
ROLLBACK;
}, qr/division by zero/);
	is($l[-1], 'live|2', 'open cursor had a frame');
}

scenario('ROLLBACK with open cursors', q{
BEGIN;
SELECT open_cur('c');
SELECT open_cur('d');
FETCH 1 FROM c;
ROLLBACK;
});

scenario('DECLARE CURSOR (no query ID, no frame) dropped by ROLLBACK', q{
BEGIN;
SELECT 'framed';
DECLARE c CURSOR FOR SELECT g FROM generate_series(1, 5) g;
FETCH 1 FROM c;
ROLLBACK;
});

{
	my @l = scenario('ROLLBACK TO SAVEPOINT with open cursor', q{
BEGIN;
SELECT open_cur('outer_c');
SAVEPOINT s;
SELECT open_cur('c');
FETCH 1 FROM c;
SELECT 'during', live FROM pssc_context_test_registry();
ROLLBACK TO SAVEPOINT s;
SELECT 'after', live FROM pssc_context_test_registry();
FETCH 1 FROM outer_c;
COMMIT;
});
	is_deeply(\@l, [ 'outer_c', 'c', '1|{}', 'during|3', 'after|2', '1|{}' ],
		'cursor frame unlinked by ROLLBACK TO SAVEPOINT');
}

{
	my @l = scenario('cursor closed early', q{
BEGIN;
SELECT open_cur('c') /*controller='early'*/;
FETCH 1 FROM c;
CLOSE c;
SELECT 'live', live FROM pssc_context_test_registry();
COMMIT;
SELECT query, tags FROM pssc_context_test_ended() WHERE query LIKE 'SELECT g,%';
});
	is_deeply(\@l, [ 'c', '1|{controller=early}', 'live|1',
		'SELECT g, pssc_context_test_tags() FROM generate_series(1, 3) g|{controller=early}' ],
		'closed cursor ended with its frame');
}

{
	my @l = scenario('interleaved portals', q{
BEGIN;
SELECT open_cur('c1') /*controller='c1'*/;
SELECT open_cur('c2') /*controller='c2'*/;
FETCH 1 FROM c1 /*controller='fetch'*/;
FETCH 1 FROM c2 /*controller='fetch'*/;
FETCH 1 FROM c1 /*controller='fetch'*/;
SELECT 'live', live FROM pssc_context_test_registry();
FETCH 2 FROM c2 /*controller='fetch'*/;
FETCH 1 FROM c1;
SELECT 'ended', count(*) FROM pssc_context_test_ended() WHERE query LIKE 'SELECT g,%';
COMMIT;
SELECT tags, toplevel, start_toplevel FROM pssc_context_test_ended()
  WHERE query LIKE 'SELECT g,%' ORDER BY tags::text;
});
	is_deeply(\@l, [
		'c1', 'c2',
		'1|{controller=c1}', '1|{controller=c2}', '2|{controller=c1}',
		'live|3',
		'2|{controller=c2}', '3|{controller=c2}', '3|{controller=c1}',
		'ended|0',
		# started inside open_cur (level 1), ended by COMMIT at level 0
		'{controller=c1}|t|f', '{controller=c2}|t|f',
	], 'each FETCH runs with its own portal\'s frame; End finds it');
}

{
	my @l = scenario('COMMIT and ROLLBACK inside a procedure', q{
TRUNCATE seen;
CALL p_tx() /*controller='call'*/;
SELECT step, tags FROM seen WHERE step < 100 ORDER BY step;
SELECT count(*), string_agg(DISTINCT tags::text, ',') FROM seen WHERE step > 100;
});
	is_deeply(\@l, [ '2|{controller=call}', '3|{controller=call}',
		'50|{controller=call}' ],
		'utility frame snapshot survives COMMIT/ROLLBACK in CALL; pinned '
		  . 'portal persisted at COMMIT runs with its frame');
}

# --- active frame -------------------------------------------------------------

{
	my @l = scenario('top-level frame', q{
SELECT tags, utility, nested, toplevel, recordable, frame_nesting_level, queryid <> 0, nesting_level
  FROM pssc_context_test_active() /*controller='top'*/;
SELECT tags, recordable FROM pssc_context_test_active();
});
	is_deeply(\@l, [ '{controller=top}|f|f|t|t|0|t|1', '{}|f' ],
		'top-level frame: own tags, level, recordable unless untagged');
}

{
	my ($out, $err) = run(q{
SET pg_stat_statement_context.track = 'all';
DO $$ DECLARE r record; BEGIN
  SELECT * INTO r FROM pssc_context_test_active();
  RAISE NOTICE 'do: % % % %', r.tags, r.toplevel, r.recordable, r.nested;
END $$ /*controller='do'*/;
SET pg_stat_statement_context.track = 'top';
DO $$ DECLARE r record; BEGIN
  SELECT * INTO r FROM pssc_context_test_active();
  RAISE NOTICE 'do: % % % %', r.tags, r.toplevel, r.recordable, r.nested;
END $$ /*controller='do'*/;
});
	like($err, qr/do: \{controller=do\} f t t.*do: \{controller=do\} f f t/s,
		'nested frame: inherits, not toplevel, recordable only with track=all');
}

# --- nested_tags --------------------------------------------------------------

my %expect = (
	inherit => {
		plpgsql => '{controller=outer}',
		before  => '{controller=outer}',
		after   => '{controller=outer}',
		do      => '{controller=do}',
		folded  => '{controller=folded}',
		vol     => '{controller=outer}',
	},
	scan => {
		plpgsql => '{controller=inner}',
		before  => '{controller=trig_before}',
		after   => '{controller=trig_after}',
		do      => '{controller=in_do}',
		folded  => '{controller=folded}',
		vol     => '{controller=folded}',
	},
	none => {
		plpgsql => '{}',
		before  => '{}',
		after   => '{}',
		do      => '{}',
		folded  => '{controller=folded}',
		vol     => '{}',
	},
);

for my $mode (qw(inherit scan none))
{
	my @l = scenario("nested_tags = $mode", qq{
SET $P.nested_tags = '$mode';
TRUNCATE trg_log, seen;
SELECT 'plpgsql', f_nested() /*controller='outer'*/;
INSERT INTO trg_t VALUES (1) /*controller='outer'*/;
SELECT kind, tags FROM trg_log ORDER BY kind DESC;
DO \$\$ BEGIN INSERT INTO seen SELECT 0, pssc_context_test_tags() /*controller='in_do'*/; END \$\$ /*controller='do'*/;
SELECT 'do', tags FROM seen;
SELECT 'folded', f_imm(1) /*controller='outer'*/;
SELECT 'vol', f_vol(1) /*controller='outer'*/;
});
	my %got = map { split /\|/, $_, 2 } @l;
	is_deeply(\%got, $expect{$mode}, "nested_tags = $mode: resolved tags");
}

# The folded call really ran at plan time: its result is a constant. (Under
# EXPLAIN, a utility, the planner runs with EXPLAIN's frame active.)
like($node->safe_psql('postgres',
		q{EXPLAIN (VERBOSE, COSTS OFF) SELECT f_imm(1) /*controller='outer'*/}),
	qr/Output: '\{controller=\w+\}'::text\[\]/,
	'f_imm(1) is constant-folded by the planner');

# --- planning is a nesting level (PG17+, as pgss) -----------------------------

{
	my @l = scenario('constant folding: plan-time SQL is nested', q{
SELECT count(*) FROM pssc_context_test_ended();
SELECT f_imm(7) /*controller='outer'*/, f_fold_marker();
SELECT regexp_replace(query, '\s+', ' ', 'g'), tags, toplevel, start_toplevel
  FROM pssc_context_test_ended()
  WHERE query LIKE '%folded%' OR query LIKE '%fold_n1%' ORDER BY query;
});
	like($l[-2], qr/^SELECT count\(\*\) FROM pg_namespace fold_n1.*\|\{\}\|$folded_top\|$folded_top$/,
		'SQL run by a constant-folded function: top level as pgss');
	is($l[-1], qq{SELECT pssc_context_test_tags() /*controller='folded'*/|{controller=folded}|$folded_top|$folded_top},
		'SQL run by a constant-folded function: own tags, top level as pgss');
	if ($have_pgss)
	{
		is($node->safe_psql('postgres', q{
SELECT string_agg(DISTINCT toplevel::text, ',') FROM pg_stat_statements
  WHERE query LIKE 'SELECT count%fold_n1%'}), $pg17 ? 'false' : 'true',
			'pg_stat_statements agrees on the toplevel of folded SQL');
	}
}

# --- user of a frame ----------------------------------------------------------

{
	# The cursor is opened under ctx_a and ended (by COMMIT) under the
	# session user: pgss records with GetUserId() at ExecutorEnd.
	run('SELECT pg_stat_statements_reset()') if $have_pgss;
	my @l = scenario('cursor ended under another role', q{
BEGIN;
SET ROLE ctx_a;
SELECT open_cur('c') /*controller='role'*/;
FETCH 1 FROM c;
RESET ROLE;
COMMIT;
SELECT userid = (SELECT oid FROM pg_roles WHERE rolname = current_user),
       start_userid = 'ctx_a'::regrole
  FROM pssc_context_test_ended() WHERE query LIKE 'SELECT g,%';
});
	is($l[-1], 't|t', 'frame user refreshed at End');
	if ($have_pgss)
	{
		is($node->safe_psql('postgres', q{
SELECT string_agg(userid::regrole::text, ',') FROM pg_stat_statements
  WHERE query LIKE 'SELECT g, pssc_context_test_tags()%'}),
			$node->safe_psql('postgres', 'SELECT current_user'),
			'pg_stat_statements records the user at End too');
	}
}

# --- multi-statement strings (DESIGN.md §6.5) -------------------------------

{
	# A statement whose leading comment lies more than scan_window bytes
	# into the string: PG18 reports stmt_location at its first token, so
	# the owned start must be found from the previous statement's end.
	my $pad = 'x' x 3000;
	my ($out, $err) = run(
		"SELECT length('$pad') \\; /*controller='second'*/ SELECT pssc_context_test_tags();\n");
	my @l = split /\n/, $out;
	is($l[-1], '{controller=second}',
		'leading comment of a later statement found past scan_window');
	is($err, '', 'multi-statement: no error');

	# Plan-time SQL (folded f_imm) and an EXECUTE'd prepared statement (own
	# source text) run between the statements of the long string; they
	# must not disturb the previous-statement boundary of the string.
	($out, $err) = run(
		"SELECT length('$pad') \\; /*controller='second'*/ SELECT f_imm(2), pssc_context_test_tags();\n");
	@l = split /\n/, $out;
	is($l[-1], '{controller=folded}|{controller=second}',
		'past scan_window, after plan-time SQL of a folded function');
	is($err, '', 'multi-statement with folding: no error');

	($out, $err) = run(
		"PREPARE p AS SELECT 1 /*controller='prep'*/;\n"
		  . "SELECT length('$pad') \\; EXECUTE p \\; /*controller='second'*/ SELECT pssc_context_test_tags();\n");
	@l = split /\n/, $out;
	is($l[-1], '{controller=second}',
		'past scan_window, after an EXECUTE of a prepared statement');
	is($err, '', 'multi-statement with EXECUTE: no error');

	($out, $err) = run(
		"SELECT /*controller='first'*/ 1 \\; SELECT pssc_context_test_tags() \\; SELECT 3 /*controller='third'*/;\n"
		  . "SELECT 'ended', tags FROM pssc_context_test_ended() WHERE query LIKE '%pssc_context_test_tags%';\n");
	@l = split /\n/, $out;
	is($l[-1], 'ended|{}', 'comments of other statements are not attributed');
}

# --- transaction-end check: negative control -----------------------------------

SKIP:
{
	skip 'a leaked frame fails an assertion in assert-enabled builds', 1
	  if $assert_build;
	my ($out, $err) = run(q{
SELECT 'before', xact_leaks FROM pssc_context_test_registry();
SELECT pssc_context_test_leak(true);
SELECT 'leaked', live, xact_leaks FROM pssc_context_test_registry();
SELECT pssc_context_test_leak(false);
SELECT 'after', live, xact_leaks FROM pssc_context_test_registry();
});
	is_deeply([ split /\n/, $out ],
		[ 'before|0', '', 'leaked|2|1', '', 'after|1|2' ],
		'a frame still registered at transaction end is detected');
}

unlike(slurp_file($node->logfile), qr/TRAP|PANIC|terminated by signal/,
	'no crash or assertion failure');

$node->stop;
done_testing();
