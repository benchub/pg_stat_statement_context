package PsscShmemSizing;

# The shared-memory sizing formula of docs/configuration.md ("Shared memory
# sizing"), and the table of totals there. Mirrors:
#   src/store.c    pssc_store_shmem_size_for() (+ PostgreSQL's
#                  hash_estimate_size() and choose_nelem_alloc(), dynahash.c)
#   src/cardcap.c  cap_shmem_size()
#   src/activity.c activity_shmem_size()
# for 64-bit platforms (8-byte MAXALIGN and pointers). test/t/034_shmem_sizing.pl
# checks it against the server; scripts/shmem-sizing.pl prints or updates the
# docs table.

use strict;
use warnings;

# Fixed struct sizes (bytes, 64-bit).
use constant {
	KEY_HEADER => 24,			# offsetof(PsscKey, tags)
	ENTRY_HEADER => 48,			# MAXALIGN(sizeof(PsscEntryHeader))
	BUCKET_SLOT => 24,			# sizeof(PsscSlot)
	EVICT_SLOT => 24,			# sizeof(PsscEvictSlot)
	STORE_HEADER => 240,		# offsetof(PsscSharedState, evict_slots)
	HASH_HEADER => 848,			# MAXALIGN(sizeof(HASHHDR)), dynahash.c
	HASH_ELEMENT => 16,			# MAXALIGN(sizeof(HASHELEMENT))
	HASH_SEGSIZE => 256,		# DEF_SEGSIZE: buckets per segment
	HASH_DIRSIZE => 256,		# DEF_DIRSIZE: initial directory entries
	EXEMPLAR_VALUE_MAX => 256,	# PSSC_EXEMPLAR_VALUE_MAX
	CAP_HEADER => 32,			# offsetof(CapShared, words)
	ACTIVITY_HEADER => 16,		# MAXALIGN(sizeof(ActivityHeader))
	ACTIVITY_SLOT_HEADER => 36,	# offsetof(ActivitySlot, tags)
};

# The fixed header sizes (STORE_HEADER, HASH_HEADER) can differ by a few
# bytes between versions of the extension and of PostgreSQL; the formula is
# documented as exact to within this many bytes.
use constant STORE_ROUNDING => 256;

use constant BEGIN_MARK => '<!-- shmem-sizing-table:begin (scripts/shmem-sizing.pl --update) -->';
use constant END_MARK => '<!-- shmem-sizing-table:end -->';

sub maxalign { my ($n) = @_; return ($n + 7) & ~7; }

sub next_pow2
{
	my ($n) = @_;
	my $p = 1;
	$p <<= 1 while $p < $n;
	return $p;
}

# MaxBackends: the activity array has one slot per backend.
#   max_connections + autovacuum_workers + max_worker_processes
#   + max_wal_senders + 1 (PG 14-16: the autovacuum launcher) or 2 (PG 17+:
#   and the slot sync worker)
# where autovacuum_workers is autovacuum_max_workers before PG 18 and
# autovacuum_worker_slots from PG 18.
sub max_backends
{
	my ($major, %g) = @_;
	return $g{max_connections} + $g{autovacuum_workers}
	  + ($major >= 17 ? 2 : 1)
	  + $g{max_worker_processes} + $g{max_wal_senders};
}

# Per-entry exemplar bytes (pssc_store_exemplar_layout_for()).
sub exemplar_block
{
	my %s = @_;
	my ($n, $kb, $nkeys) = @s{qw(max_entries exemplar_memory_kb exemplar_keys)};
	return 0 if !$nkeys || $kb <= 0;
	my $per_entry = int($kb * 1024 / $n);
	$per_entry -= $per_entry % 8;
	my $per_key = int($per_entry / $nkeys);
	return 0 if $per_key <= 2;
	my $len = $per_key - 2;
	$len = EXEMPLAR_VALUE_MAX if $len > EXEMPLAR_VALUE_MAX;
	return maxalign($nkeys * (2 + $len));
}

sub entry_bytes
{
	my %s = @_;
	return maxalign(KEY_HEADER + $s{max_tagset_bytes}) + ENTRY_HEADER
	  + BUCKET_SLOT * $s{bucket_count} + exemplar_block(%s);
}

# choose_nelem_alloc(): elements are allocated in groups of this many.
sub nelem_alloc
{
	my ($elem) = @_;
	my $alloc = 128;
	my $n;
	do { $alloc <<= 1; $n = int($alloc / $elem); } while ($n < 32);
	return $n;
}

# _info().shmem_bytes. header_fixed overrides STORE_HEADER.
sub store_bytes
{
	my %s = @_;
	my $n = $s{max_entries};
	my $hdr = $s{header_fixed} // STORE_HEADER;
	my $elem = HASH_ELEMENT + entry_bytes(%s);
	my $group = nelem_alloc($elem);
	my $nbuckets = next_pow2($n);
	my $nsegs = next_pow2(int(($nbuckets - 1) / HASH_SEGSIZE) + 1);
	my $ndir = HASH_DIRSIZE;
	$ndir <<= 1 while $ndir < $nsegs;
	return maxalign($hdr + EVICT_SLOT * $n)
	  + HASH_HEADER
	  + 8 * $ndir
	  + $nsegs * 8 * HASH_SEGSIZE
	  + int(($n - 1) / $group + 1) * $group * $elem;
}

sub cap_bytes
{
	my ($slots) = @_;
	my $nkeys = int($slots / 16);
	$nkeys = 64 if $nkeys < 64;
	return CAP_HEADER + 8 * ($slots + 2 * $nkeys);
}

sub activity_bytes
{
	my ($backends, $tagset) = @_;
	return ACTIVITY_HEADER + $backends * maxalign(ACTIVITY_SLOT_HEADER + $tagset);
}

# Settings: max_entries, max_tagset_bytes, bucket_count, exemplar_keys (the
# number of keys), exemplar_memory_kb, cardinality_cap_slots, max_backends.
sub estimate
{
	my %s = @_;
	my %r = (
		store => store_bytes(%s),
		exemplar => $s{max_entries} * exemplar_block(%s),
		cap => cap_bytes($s{cardinality_cap_slots}),
		activity => activity_bytes($s{max_backends}, $s{max_tagset_bytes}),
	);
	$r{total} = $r{store} + $r{cap} + $r{activity};
	return \%r;
}

our %DEFAULTS = (
	max_entries => 10000,
	max_tagset_bytes => 512,
	bucket_count => 12,
	exemplar_keys => 0,
	exemplar_memory_kb => 2048,
	cardinality_cap_slots => 16384,
	max_connections => 100,
);

# Rows of the docs table: label and the settings that differ from the
# defaults.
our @ROWS = (
	[ 'Defaults' => {} ],
	[ '24 h of history: `bucket_count = 288`' => { bucket_count => 288 } ],
	[ '`max_entries = 50000`' => { max_entries => 50000 } ],
	[ q{Exemplars on: `exemplar_keys = 'traceparent'`} => { exemplar_keys => 1 } ],
	[ '`max_connections = 5000`' => { max_connections => 5000 } ],
	[ 'Small instance: `max_entries = 2000`, `max_tagset_bytes = 256`'
		  => { max_entries => 2000, max_tagset_bytes => 256 } ],
);

sub commify
{
	my ($n) = @_;
	1 while $n =~ s/^(\d+)(\d{3})/$1,$2/;
	return $n;
}

sub fmt
{
	my ($b) = @_;
	return '0' if $b == 0;
	return sprintf('%s (%.1f MiB)', commify($b), $b / 1048576)
	  if $b >= 1048576;
	return sprintf('%s (%.0f KiB)', commify($b), $b / 1024);
}

# The markdown table, ending with a newline. Activity uses the PG 18
# default MaxBackends (autovacuum_worker_slots 16, max_worker_processes 8,
# max_wal_senders 10: 136 at max_connections 100).
sub docs_table
{
	my $out = "| Settings (others at their defaults) | Store (`shmem_bytes`) | of which exemplars | Cap table (`cap_shmem_bytes`) | Activity | Total |\n"
	  . "|---|---:|---:|---:|---:|---:|\n";
	for my $row (@ROWS)
	{
		my ($label, $over) = @$row;
		my %s = (%DEFAULTS, %$over);
		$s{max_backends} = max_backends(18,
			max_connections => $s{max_connections},
			autovacuum_workers => 16,
			max_worker_processes => 8,
			max_wal_senders => 10);
		my $e = estimate(%s);
		$out .= '| ' . join(' | ', $label,
			map { fmt($e->{$_}) } qw(store exemplar cap activity total)) . " |\n";
	}
	return $out;
}

1;
