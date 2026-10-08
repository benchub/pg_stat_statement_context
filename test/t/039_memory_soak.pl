# Bounded backend memory over a long session (DESIGN.md §9; backlog
# 20261008-065635-5, TST-10): one backend runs many tagged statements of
# every shape the hooks see, and its memory, read from
# pg_backend_memory_contexts at fixed points, must not grow once warmed up.
#
# Shapes: top-level SELECT/INSERT/UPDATE with sqlcommenter and marginalia
# comments (values that change every round, so the small store keeps
# evicting), regex and appname extractor tags, tags_override, exemplars,
# nested statements (PL/pgSQL static SQL and EXECUTE, a procedure CALL),
# utility statements (SET/RESET, TRUNCATE, ANALYZE, cursors, transaction
# control), PREPARE/EXECUTE/DEALLOCATE, and errors (top level, nested and
# caught in PL/pgSQL), plus reads of the views and _info().
#
# After a warm-up of a fifth of the rounds, the backend's memory is sampled
# at five equally spaced points; from the first to the last sample,
# TopMemoryContext's own blocks, CacheMemoryContext's own blocks, this
# extension's contexts and the total over all contexts may grow by at most a
# small slack. A per-statement leak of a few bytes into any of them exceeds
# it.
#
# PSSC_SOAK_STATEMENTS sets the number of statements (default 20000, a few
# seconds); e.g. PSSC_SOAK_STATEMENTS=200000 for a long run.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $P = 'pg_stat_statement_context';
my $target = $ENV{PSSC_SOAK_STATEMENTS} // 20000;
die "PSSC_SOAK_STATEMENTS must be a positive integer\n"
  unless $target =~ /^[1-9][0-9]*$/;

my $node = PostgreSQL::Test::Cluster->new('soak');
$node->init;
$node->append_conf('postgresql.conf', <<"EOC");
shared_preload_libraries = '$P'
$P.tags = '*'
$P.exclude_tags = 'traceparent'
$P.exemplar_keys = 'traceparent'
$P.max_entries = 100
$P.track = all
$P.cardinality_cap = 40
$P.extractors = 'sqlcommenter, marginalia, regex(pattern=''svc=(\\\\w+)\\\\s+op=(\\\\w+)'', keys=service|operation), appname(format=regex, pattern=''^(\\\\w+)/([0-9]+)\$'', keys=app|version)'
$P.normalize = 'route: ''[0-9]+'' => ''N'''
EOC
$node->start;

$node->safe_psql('postgres', <<"EOS");
CREATE EXTENSION $P;
CREATE TABLE soak_t(i int PRIMARY KEY, v text);
INSERT INTO soak_t SELECT g, 'v' || g FROM generate_series(1, 100) g;
CREATE TABLE soak_u(i int);
CREATE FUNCTION soak_f(n int) RETURNS int LANGUAGE plpgsql AS \$\$
DECLARE r int;
BEGIN
	SELECT count(*) INTO r FROM soak_t WHERE i <= n /*controller='fn',action='static'*/;
	EXECUTE format('SELECT %s /*controller=''fn'',action=''dyn%s''*/', n, n % 7) INTO r;
	BEGIN
		PERFORM 1 / (n - n) /*controller='fn',action='caught'*/;
	EXCEPTION WHEN division_by_zero THEN
		r := r + 1;
	END;
	RETURN r;
END
\$\$;
CREATE PROCEDURE soak_p(n int) LANGUAGE plpgsql AS \$\$
BEGIN
	UPDATE soak_t SET v = 'p' || n WHERE i = n % 100 + 1 /*controller='proc'*/;
	COMMIT;
	INSERT INTO soak_u VALUES (n) /*controller='proc',action='ins'*/;
	ROLLBACK;
END
\$\$;
EOS

# One round: STMTS_PER_ROUND statements, each run by this backend (counting
# the nested ones the hooks also see).
sub round
{
	my ($r) = @_;
	my $c = $r % 60;    # more values than cardinality_cap
	my $a = $r % 13;
	my $nested = $r % 2 ? 'inherit' : 'scan';
	return <<"EOR";
SELECT count(*) FROM soak_t WHERE i < $r /*controller='c$c',action='a$a',traceparent='tp-$r'*/;
SET $P.nested_tags = $nested;
SELECT v FROM soak_t WHERE i = $r % 100 + 1 /* svc=s$a op=o$c */;
SELECT $r AS marginalia /*controller:m$c,action:index*/;
UPDATE soak_t SET v = 'r$r' WHERE i = $r % 100 + 1 /*controller='c$c',action='upd'*/;
INSERT INTO soak_u VALUES ($r) /*controller='ins',route='r$c'*/;
SET application_name = 'app$a/$c';
SELECT 1 AS appname_tagged;
RESET application_name;
SET $P.tags_override = 'tenant=''t$c''';
SELECT 2 AS overridden /*controller='ov'*/;
RESET $P.tags_override;
SET work_mem = '${\ (4 + $a)}MB' /*controller='set'*/;
RESET work_mem;
SELECT soak_f($r % 50 + 1) /*controller='nest',action='a$a'*/;
CALL soak_p($r) /*controller='call'*/;
PREPARE soak_s(int) AS SELECT v FROM soak_t WHERE i = \$1 /*controller='prep',action='a$a'*/;
EXECUTE soak_s($r % 100 + 1);
EXECUTE soak_s(1);
DEALLOCATE soak_s;
SELECT 1 / 0 /*controller='err',action='a$a'*/;
SELECT soak_f(0) / 0 /*controller='nesterr'*/;
BEGIN /*controller='tx'*/;
DECLARE soak_c CURSOR FOR SELECT * FROM soak_t /*controller='cur',action='a$a'*/;
FETCH 3 FROM soak_c;
CLOSE soak_c;
SELECT no_such_column FROM soak_t /*controller='err2'*/;
ROLLBACK;
TRUNCATE soak_u /*controller='trunc'*/;
ANALYZE soak_t /*controller='analyze'*/;
DO \$\$ DECLARE x int; BEGIN SELECT count(*) INTO x FROM soak_u /*controller='do'*/; END \$\$;
EOR
}

# Statements per round as the hooks see them: 31 top level, plus soak_f's 3
# (twice: nest and nesterr), soak_p's 4 and the DO block's 1.
my $per_round = 31 + 3 * 2 + 4 + 1;
my $rounds = int(($target + $per_round - 1) / $per_round);
$rounds = 25 if $rounds < 25;
my $warmup = int($rounds / 5);
my $samples = 5;
my $step = int(($rounds - $warmup) / ($samples - 1));

my $sample_sql = <<"EOS";
SELECT 'MEM ' || :'mark' || ' ' ||
  coalesce(sum(total_bytes) FILTER (WHERE name = 'TopMemoryContext'), 0) || ' ' ||
  coalesce(sum(total_bytes) FILTER (WHERE name = 'CacheMemoryContext'), 0) || ' ' ||
  coalesce(sum(total_bytes) FILTER (WHERE name LIKE '$P%'), 0) || ' ' ||
  sum(total_bytes)
FROM pg_backend_memory_contexts;
EOS

# One round first, on an empty store, to check that every shape is recorded
# (in the soak itself, eviction keeps only some of them).
{
	my ($ret, $o, $e) = $node->psql('postgres', round(0), on_error_stop => 0);
	like($e, qr/division by zero/, 'one round: errors were raised');
	my $kinds = $node->safe_psql('postgres',
		"SELECT string_agg(DISTINCT tags->>'controller', ',' ORDER BY tags->>'controller')"
		  . " FROM ${P}_totals WHERE tags ? 'controller'");
	is($kinds, 'analyze,c0,call,cur,do,fn,ins,m0,nest,ov,prep,proc,set,trunc,tx',
		'one round: top-level, nested, utility, prepared, error and override shapes recorded');
	is($node->safe_psql('postgres',
			"SELECT bool_or(tags ? 'service') || '|' || bool_or(tags ? 'app') || '|' || bool_or(tags ? 'tenant')"
			  . " || '|' || bool_or(tags->>'route' = 'rN') || '|' || bool_or(exemplars ? 'traceparent')"
			  . " FROM ${P}_totals"),
		'true|true|true|true|true', 'one round: regex, appname, override, normalize and exemplar tags recorded');
	$node->safe_psql('postgres', "SELECT ${P}_reset()");
}

my $script = "\\set VERBOSITY terse\n";
my $mark = 0;
for my $r (1 .. $rounds)
{
	$script .= round($r);
	# Reads of the views and SRFs every few rounds.
	$script .= "SELECT count(*) FROM ${P}_totals;\n"
	  . "SELECT count(*) FROM ${P}_info();\n"
	  if $r % 10 == 0;
	if ($r >= $warmup && ($r - $warmup) % $step == 0 && $mark < $samples)
	{
		$script .= "\\set mark $mark\n$sample_sql";
		$mark++;
	}
}
is($mark, $samples, "$samples samples planned over $rounds rounds");

my $file = PostgreSQL::Test::Utils::tempdir() . '/soak.sql';
open(my $fh, '>', $file) or die "open $file: $!";
print $fh $script;
close($fh);

note("running $rounds rounds (~" . $rounds * $per_round . " statements)");
my $stdout = '';
my $stderr = '';
my $ret = $node->psql(
	'postgres', undef,
	extra_params => [ '-f', $file ],
	stdout => \$stdout,
	stderr => \$stderr,
	on_error_stop => 0);
is($ret, 0, 'psql ran the whole script (errors are expected and ignored)');

my @samples;
for (split /\n/, $stdout)
{
	push @samples, [ $1, $2, $3, $4, $5 ] if /^MEM (\d+) (\d+) (\d+) (\d+) (\d+)$/;
}
is(scalar @samples, $samples, "$samples memory samples")
  or diag("stdout tail: " . substr($stdout, -2000));

like($stderr, qr/division by zero/, 'errors were raised');
cmp_ok($node->safe_psql('postgres', "SELECT evicted_entries FROM ${P}_info()"),
	'>', 0, 'the store evicted');

my @names = (
	'TopMemoryContext', 'CacheMemoryContext',
	"$P contexts", 'all contexts');
# Slack: allocator block granularity and caches that settle late. A leak of
# 8 bytes per statement over the sampled rounds exceeds it.
my $sampled = ($samples - 1) * $step * $per_round;
my @slack = (32768, 32768, 16384, 65536);
for my $i (0 .. $#names)
{
	my @v = map { $_->[ $i + 1 ] } @samples;
	my $growth = $v[-1] - $v[0];
	note("$names[$i] samples: @v");
	cmp_ok($growth, '<=', $slack[$i],
		"$names[$i]: growth over ~$sampled statements within slack")
	  or diag("$names[$i] samples: @v");
}

$node->stop;
done_testing();
