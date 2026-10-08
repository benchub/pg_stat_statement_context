# Shared helpers for the TAP tests (test/t). The Makefile adds test/perl to
# prove's include path.
package PsscTest;

use strict;
use warnings;

use Exporter 'import';
our @EXPORT = qw(pgss_suffix module_suffix);

# Suffixes a loadable module can have, by platform (DLSUFFIX): .so on Linux
# and on macOS before PG16, .dylib on macOS from PG16, .dll on Windows.
my @SUFFIXES = ('.so', '.dylib', '.dll');

# The suffix of the installed pg_stat_statements module (for example '.so'),
# or undef when it is not installed in the node's pkglibdir.
#
# PSSC_REQUIRE_PGSS=1 (set by docker/run-tests.sh) makes a missing module a
# hard failure, so the pgss parity checks cannot be skipped silently.
# PSSC_TEST_WITHOUT_PGSS=1 treats pgss as absent, to exercise the paths
# taken without it.
sub pgss_suffix
{
	my ($node) = @_;
	my $require = ($ENV{PSSC_REQUIRE_PGSS} // '') eq '1';
	if (($ENV{PSSC_TEST_WITHOUT_PGSS} // '') eq '1')
	{
		die "PSSC_TEST_WITHOUT_PGSS=1 contradicts PSSC_REQUIRE_PGSS=1\n"
		  if $require;
		return undef;
	}
	my $pkglibdir = $node->safe_psql('postgres',
		q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
	my $s = _find_module($pkglibdir, 'pg_stat_statements');
	return $s if defined $s;
	die "pg_stat_statements is not installed in $pkglibdir "
	  . "(looked for pg_stat_statements{" . join(',', @SUFFIXES) . "}), "
	  . "but PSSC_REQUIRE_PGSS=1\n"
	  if $require;
	return undef;
}

sub _find_module
{
	my ($pkglibdir, $name) = @_;
	for my $s (@SUFFIXES)
	{
		return $s if -e "$pkglibdir/$name$s";
	}
	return undef;
}

# The suffix of another installed module (for example auto_explain or
# pgaudit, test/t/036_hook_coexistence.pl), or undef when it is not
# installed in the node's pkglibdir. A module named in PSSC_REQUIRE_MODULES
# (space-separated; docker/run-tests.sh sets it to what the harness
# installed) must be present: a missing one is a hard failure rather than a
# skipped check.
sub module_suffix
{
	my ($node, $name) = @_;
	my $pkglibdir = $node->safe_psql('postgres',
		q{SELECT setting FROM pg_config WHERE name = 'PKGLIBDIR'});
	my $s = _find_module($pkglibdir, $name);
	return $s if defined $s;
	my %require = map { $_ => 1 } split ' ', ($ENV{PSSC_REQUIRE_MODULES} // '');
	die "$name is not installed in $pkglibdir "
	  . "(looked for $name\{" . join(',', @SUFFIXES) . "}), "
	  . "but PSSC_REQUIRE_MODULES lists it\n"
	  if $require{$name};
	return undef;
}

1;
