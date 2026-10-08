# The shared-memory sizing formula of docs/configuration.md ("Shared memory
# sizing"; backlog 20261008-065635-10), as implemented in
# test/perl/PsscShmemSizing.pm, checked against what the server reports.
#
# For several settings combinations (defaults; a long history with a large
# tag set; exemplars on; the minimum cap table with tiny exemplars), a node
# is started and:
#   - _info().shmem_bytes matches the store formula within the documented
#     rounding (the fixed struct headers), and exactly once that constant
#     is taken from the server's own named allocations;
#   - _info().exemplar_shmem_bytes and _info().cap_shmem_bytes match exactly;
#   - the activity array, which _info() does not report, matches the
#     documented formula (MaxBackends from the documented per-version sum)
#     exactly, as pg_shmem_allocations shows it;
#   - the extension has no other named allocations, and the sources make
#     exactly the three shared-memory requests the formula covers.
# The table of totals in docs/configuration.md must equal the module's
# output (scripts/shmem-sizing.pl --update regenerates it).
use strict;
use warnings;

use File::Basename qw(dirname);
use File::Spec;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscShmemSizing;

my $P = 'pg_stat_statement_context';
my $ROOT = File::Spec->rel2abs(dirname(__FILE__) . '/../..');

my @combos = (
	{ name => 'defaults', conf => {} },
	{
		name => 'long history, large tag set, many cap slots',
		conf => {
			"$P.bucket_count" => 288,
			"$P.max_entries" => 2000,
			"$P.max_tagset_bytes" => 1001,
			"$P.cardinality_cap_slots" => 100000,
			max_connections => 50,
		},
	},
	{
		name => 'exemplars on',
		conf => {
			"$P.exemplar_keys" => "'traceparent, tracestate'",
			"$P.exemplar_memory" => "'512kB'",
			"$P.max_entries" => 4000,
			"$P.max_tagset_bytes" => 128,
			max_connections => 300,
			max_worker_processes => 4,
		},
	},
	{
		name => 'minimum cap table, tiny exemplars',
		conf => {
			"$P.exemplar_keys" => "'a, b, c'",
			"$P.exemplar_memory" => "'64kB'",
			"$P.max_entries" => 100,
			"$P.bucket_count" => 1,
			"$P.cardinality_cap_slots" => 256,
			max_wal_senders => 0,
		},
	},
);

my $i = 0;
for my $c (@combos)
{
	$i++;
	my $node = PostgreSQL::Test::Cluster->new("sizing$i");
	$node->init;
	my $conf = "shared_preload_libraries = '$P'\n";
	$conf .= "$_ = $c->{conf}{$_}\n" for sort keys %{ $c->{conf} };
	$node->append_conf('postgresql.conf', $conf);
	$node->start;
	$node->safe_psql('postgres', "CREATE EXTENSION $P");

	my %g;
	for my $k (qw(server_version_num max_connections autovacuum_max_workers
		max_worker_processes max_wal_senders),
		"$P.max_entries", "$P.max_tagset_bytes", "$P.bucket_count",
		"$P.exemplar_keys", "$P.exemplar_memory", "$P.cardinality_cap_slots")
	{
		$g{$k} = $node->safe_psql('postgres',
			"SELECT setting FROM pg_settings WHERE name = '$k'");
	}
	my $major = int($g{server_version_num} / 10000);
	my $av = $g{autovacuum_max_workers};
	$av = $node->safe_psql('postgres', 'SHOW autovacuum_worker_slots')
	  if $major >= 18;
	my $nkeys = grep { length } map { s/^\s+|\s+$//gr } split /,/,
	  $g{"$P.exemplar_keys"};

	my %s = (
		max_entries => $g{"$P.max_entries"},
		max_tagset_bytes => $g{"$P.max_tagset_bytes"},
		bucket_count => $g{"$P.bucket_count"},
		exemplar_keys => $nkeys,
		exemplar_memory_kb => $g{"$P.exemplar_memory"},
		cardinality_cap_slots => $g{"$P.cardinality_cap_slots"},
		max_backends => PsscShmemSizing::max_backends(
			$major,
			max_connections => $g{max_connections},
			autovacuum_workers => $av,
			max_worker_processes => $g{max_worker_processes},
			max_wal_senders => $g{max_wal_senders}),
	);
	my $est = PsscShmemSizing::estimate(%s);

	my ($shmem, $cap, $ex) = split /\|/, $node->safe_psql('postgres',
		"SELECT shmem_bytes, cap_shmem_bytes, exemplar_shmem_bytes FROM ${P}_info()");
	my %alloc = map { split /\|/ } split /\n/, $node->safe_psql('postgres',
		"SELECT name, size FROM pg_shmem_allocations WHERE name LIKE '$P%'");

	cmp_ok(abs($shmem - $est->{store}), '<=', PsscShmemSizing::STORE_ROUNDING,
		"$c->{name}: shmem_bytes $shmem is within the documented rounding of $est->{store}");
	# With the fixed parts read from the server, the formula is exact.
	my $hdr_fixed = $alloc{$P} - 24 * $s{max_entries};
	is(PsscShmemSizing::store_bytes(%s, header_fixed => $hdr_fixed), $shmem,
		"$c->{name}: store formula is exact with the server's header size");
	is($ex, $est->{exemplar}, "$c->{name}: exemplar_shmem_bytes");
	is($cap, $est->{cap}, "$c->{name}: cap_shmem_bytes");
	is($alloc{"$P cardinality caps"}, $est->{cap},
		"$c->{name}: cap table allocation");
	is($alloc{"$P activity"}, $est->{activity},
		"$c->{name}: activity allocation (MaxBackends $s{max_backends})");
	is_deeply([ sort keys %alloc ],
		[ sort ($P, "$P activity", "$P cardinality caps", "$P hash") ],
		"$c->{name}: no other named allocations");
	$node->stop;
}

# Every shared-memory request is one the formula covers.
my @requests;
for my $f (glob("$ROOT/src/*.c"))
{
	open my $fh, '<', $f or die "$f: $!";
	my $n = () = do { local $/; <$fh> } =~ /RequestAddinShmemSpace\s*\(/g;
	push @requests, (File::Basename::basename($f)) x $n;
	close $fh;
}
is_deeply([ sort @requests ], [qw(activity.c cardcap.c store.c)],
	'the three shared-memory requests are the ones documented');

# The documented table is the module's output.
my $doc = "$ROOT/docs/configuration.md";
open my $fh, '<', $doc or die "$doc: $!";
my $text = do { local $/; <$fh> };
close $fh;
my ($table) = $text =~ /\Q${\PsscShmemSizing::BEGIN_MARK}\E\n(.*?)\Q${\PsscShmemSizing::END_MARK}\E/s;
ok(defined $table, 'docs/configuration.md has the generated sizing table');
is($table // '', PsscShmemSizing::docs_table(),
	'the sizing table in docs/configuration.md is current (scripts/shmem-sizing.pl --update)');

done_testing();
