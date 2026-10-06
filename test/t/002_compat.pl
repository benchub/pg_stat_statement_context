# Exercises every src/compat.h shim through the TEST-ONLY module
# test/modules/pssc_compat_test (install it first: make install-test-modules).
# Expectations are derived from server_version_num here, independently of
# compat.h, so a shim that picks the wrong branch for a version fails.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('compat');
$node->init;
$node->append_conf('postgresql.conf', q{
shared_preload_libraries = 'pssc_compat_test'
# Placeholder under the reserved prefix, present before the module loads.
pssc_compat_test.bogus = 'x'
});
$node->start;

my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
note "server_version_num = $vnum";

$node->safe_psql('postgres', 'CREATE EXTENSION pssc_compat_test');

# ---- shared memory request: PG14 in _PG_init, PG15+ shmem_request_hook ----
is( $node->safe_psql('postgres',
		q{SELECT size >= 1024 * 1024 FROM pg_shmem_allocations WHERE name = 'pssc_compat_test'}),
	't', 'shmem: requested 1MB segment was allocated');
is($node->safe_psql('postgres', 'SELECT pssc_compat_test_shmem_bump()'),
	'1', 'shmem: named LWLock tranche usable, first bump');
is($node->safe_psql('postgres', 'SELECT pssc_compat_test_shmem_bump()'),
	'2', 'shmem: counter is shared across backends');

# ---- per-backend shared memory: MaxBackends at request time, slot index ----
is( $node->safe_psql('postgres', q{
	SELECT at_request = max_backends, slot >= 0, slot < max_backends,
	       max_backends >= current_setting('max_connections')::int
	FROM pssc_compat_test_backends()}),
	't|t|t|t', 'backends: the request-time count equals MaxBackends; the slot is in range');

# ---- ExecutorRun and ProcessUtility signatures ----
# FETCH 2 then FETCH 3 runs the executor twice.
my $out = $node->safe_psql('postgres', q{
BEGIN;
SELECT pssc_compat_test_reset();
DECLARE c CURSOR FOR SELECT g FROM generate_series(1, 10) g;
FETCH 2 FROM c;
FETCH 3 FROM c;
CLOSE c;
SELECT executor_runs, utility_calls FROM pssc_compat_test_stats();
COMMIT;
});
is($out, "\n1\n2\n3\n4\n5\n3|4", 'ExecutorRun/ProcessUtility pass-through');

# ---- GUC extra allocator: malloc on PG14/15, guc_malloc on PG16+ ----
is( $node->safe_psql('postgres',
		q{SET pssc_compat_test.extra = 'abc'; SELECT pssc_compat_test_guc_extra();}),
	'ABC', 'GUC extra: assign hook sees the extra built by the check hook');
is( $node->safe_psql('postgres', q{
SET pssc_compat_test.extra = 'outer';
BEGIN;
SET LOCAL pssc_compat_test.extra = 'inner';
ROLLBACK;
SELECT pssc_compat_test_guc_extra();
}), 'OUTER', 'GUC extra: rollback restores previous extra');

# Repeatedly replacing the value makes guc.c free old extras with free() on
# PG14/15 and guc_free() on PG16+; a mismatched allocator crashes here.
is( $node->safe_psql('postgres', q{
SELECT count(set_config('pssc_compat_test.extra', 'v' || g, false))
  FROM generate_series(1, 200) g;
SELECT pssc_compat_test_guc_extra();
}), "200\nV200", 'GUC extra: 200 replacements free old extras cleanly');

my $guc_cxt_sql = q{SELECT coalesce(sum(total_bytes), 0) FROM pg_backend_memory_contexts WHERE name = 'GUCMemoryContext'};
$out = $node->safe_psql('postgres', qq{
SELECT ($guc_cxt_sql);
SET pssc_compat_test.extra = 'big';
SELECT ($guc_cxt_sql);
SELECT count(set_config('pssc_compat_test.extra', 'v' || g, false))
  FROM generate_series(1, 200) g;
SELECT ($guc_cxt_sql);
});
my ($cxt0, $cxt1, undef, $cxt2) = split /\n/, $out;
if ($vnum >= 160000)
{
	cmp_ok($cxt1 - $cxt0, '>=', 256 * 1024,
		'GUC extra: allocated in GUCMemoryContext on PG16+');
	cmp_ok($cxt2 - $cxt1, '<', 4 * 256 * 1024,
		'GUC extra: replaced extras are freed, no growth after 200 sets');
}
else
{
	is("$cxt0/$cxt1/$cxt2", '0/0/0',
		'GUC extra: no GUCMemoryContext on PG14/15 (malloc)');
}

# ---- regex allocator: malloc on PG14/15, palloc in context on PG16+ ----
my $regex_in_cxt = $vnum >= 160000 ? 't' : 'f';
is( $node->safe_psql('postgres',
		q{SELECT matched, capture, compiled_in_context, freed FROM pssc_compat_test_regex('svc=(\w+)', 'x svc=billing y')}),
	"t|billing|$regex_in_cxt|t",
	"regex: match/capture work; compiled in caller context = $regex_in_cxt; pssc_regfree releases it");
is( $node->safe_psql('postgres',
		q{SELECT matched, capture IS NULL, freed FROM pssc_compat_test_regex('^nomatch$', 'something')}),
	'f|t|t', 'regex: non-match');
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SELECT * FROM pssc_compat_test_regex('(a', 'a')});
like($stderr, qr/invalid regular expression/, 'regex: compile error reported');

# ---- materialized SRF: InitMaterializedSRF on PG15+, hand-rolled on PG14 ----
is($node->safe_psql('postgres', 'SELECT i, label FROM pssc_compat_test_srf(3)'),
	"1|row 1\n2|row 2\n3|row 3", 'SRF: returns rows in a FROM clause');
is( $node->safe_psql('postgres',
		'SELECT i, label FROM pssc_compat_test_srf(2, use_expected_desc => true)'),
	"1|row 1\n2|row 2", 'SRF: USE_EXPECTED_DESC flag with a composite result');
is($node->safe_psql('postgres', 'SELECT pssc_compat_test_srf(2)'),
	"(1,\"row 1\")\n(2,\"row 2\")", 'SRF: works in the target list');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pssc_compat_test_srf(0)'),
	'0', 'SRF: empty result');
is($node->safe_psql('postgres', 'SELECT * FROM pssc_compat_test_srf_scalar(3)'),
	"10\n20\n30", 'SRF: scalar result needs and uses USE_EXPECTED_DESC (unblessed)');
is($node->safe_psql('postgres', 'SELECT * FROM pssc_compat_test_srf_scalar(2, bless => true)'),
	"11\n21", 'SRF: BLESS flag registers the tuple descriptor');
($ret, $stdout, $stderr) = $node->psql('postgres', 'SELECT pssc_compat_test_srf_direct()');
like($stderr,
	qr/set-valued function called in context that cannot accept a set/,
	'SRF: rejects a call without ReturnSetInfo');

# ---- src/counters.h: executor/utility time in ms, exactly as pgss ----
# The helper must end the instrumentation loop itself (total is 0 before),
# be idempotent, and equal pgss's expressions bit for bit.
is( $node->safe_psql('postgres', q{
	SELECT pre_total = 0, exec_ms >= 20, exec_ms < 20000, exec_again = exec_ms,
	       exec_pgss = exec_ms, util_ms >= 20, util_ms < 20000, util_pgss = util_ms
	FROM pssc_compat_test_counters_ms(20)}),
	't|t|t|t|t|t|t|t', 'counters: ms conversions match pgss (InstrEndLoop, * 1000.0, GET_MILLISEC)');

# ---- GUC prefix reservation: MarkGUCPrefixReserved (PG15+) / EmitWarningsOnPlaceholders (PG14) ----
my $log = slurp_file($node->logfile);
if ($vnum >= 150000)
{
	like($log,
		qr/invalid configuration parameter name "pssc_compat_test\.bogus", removing it/,
		'prefix: existing placeholder removed at load (PG15+)');
	($ret, $stdout, $stderr) = $node->psql('postgres', q{SET pssc_compat_test.other = 1});
	like($stderr, qr/invalid configuration parameter name "pssc_compat_test\.other"/,
		'prefix: new placeholders under the prefix are rejected (PG15+)');
}
else
{
	like($log, qr/unrecognized configuration parameter "pssc_compat_test\.bogus"/,
		'prefix: warning for existing placeholder at load (PG14)');
	($ret, $stdout, $stderr) = $node->psql('postgres', q{SET pssc_compat_test.other = 1});
	is($ret, 0, 'prefix: PG14 still allows new placeholders');
}

$node->stop;
$log = slurp_file($node->logfile);
unlike($log, qr/PANIC|terminated by signal|server process .* was terminated/,
	'server log has no crashes');

done_testing();
