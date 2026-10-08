#!/usr/bin/env perl
# Shared-memory sizing (docs/configuration.md "Shared memory sizing").
#   scripts/shmem-sizing.pl             print the docs table
#   scripts/shmem-sizing.pl --update    rewrite the table in docs/configuration.md
#   scripts/shmem-sizing.pl --check     fail if that table is out of date
#   scripts/shmem-sizing.pl name=value ...
#       estimate for one combination; names: max_entries max_tagset_bytes
#       bucket_count exemplar_keys (number of keys) exemplar_memory_kb
#       cardinality_cap_slots max_backends (default 136: max_connections 100
#       on PG 18)
# The formula is in test/perl/PsscShmemSizing.pm; test/t/034_shmem_sizing.pl
# checks it against the server and the docs table against this output.
use strict;
use warnings;
use FindBin;
use lib "$FindBin::Bin/../test/perl";
use PsscShmemSizing;

my $doc = "$FindBin::Bin/../docs/configuration.md";
my $mode = $ARGV[0] // '';

if ($mode eq '--update' || $mode eq '--check')
{
	open my $fh, '<', $doc or die "$doc: $!\n";
	my $text = do { local $/; <$fh> };
	close $fh;
	my ($b, $e) = (PsscShmemSizing::BEGIN_MARK, PsscShmemSizing::END_MARK);
	my $new = $text;
	$new =~ s/(\Q$b\E\n).*?(\Q$e\E)/$1 . PsscShmemSizing::docs_table() . $2/se
	  or die "$doc: no sizing table markers\n";
	if ($mode eq '--check')
	{
		die "$doc: sizing table is out of date; run $0 --update\n"
		  if $new ne $text;
		exit 0;
	}
	if ($new ne $text)
	{
		open $fh, '>', $doc or die "$doc: $!\n";
		print $fh $new;
		close $fh;
	}
	exit 0;
}

if (@ARGV)
{
	my %s = (%PsscShmemSizing::DEFAULTS, max_backends => 136);
	for (@ARGV)
	{
		my ($k, $v) = /^(\w+)=(\d+)$/ or die "usage: $0 [--update|--check|name=value ...]\n";
		die "unknown setting $k\n" unless exists $s{$k};
		$s{$k} = $v;
	}
	my $r = PsscShmemSizing::estimate(%s);
	printf "%-10s %12d\n", $_, $r->{$_} for qw(store exemplar cap activity total);
	exit 0;
}

print PsscShmemSizing::docs_table();
