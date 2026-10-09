# Relative-link checker for the repository's Markdown documents, shared by
# test/t/037_managed_services.pl (the real tree) and test/t/043_release_tree.pl
# (fixture trees). No server needed.
package PsscDocLinks;

use strict;
use warnings;

use Exporter 'import';
use File::Basename qw(dirname);
use File::Find;
our @EXPORT_OK = qw(check_relative_links);

sub _slurp
{
	my ($f) = @_;
	open my $fh, '<', $f or die "open $f: $!";
	local $/;
	return scalar <$fh>;
}

# Checks every relative link (file and #anchor) of every *.md under $root.
# Returns { nlinks => checked, nomitted => skipped as left out of a release
# tree, bad => [ "file: link (reason)", ... ] }.
sub check_relative_links
{
	my ($root) = @_;
	my @files;
	find(sub {
			push @files, $File::Find::name
			  if /\.md$/
			  && substr($File::Find::name, length($root)) !~ m{/(?:tmp|worktrees|tmp_check|\.git)/};
		}, $root);
	my %anchors;
	my $anchors_of = sub {
		my ($file) = @_;
		return $anchors{$file} //= do {
			my $t = _slurp($file);
			$t =~ s/^```.*?^```//msg;
			my (%a, %n);
			for my $h ($t =~ /^#{1,6}\s+(.+?)\s*$/mg)
			{
				my $s = lc $h;
				$s =~ s/\[([^\]]*)\]\([^)]*\)/$1/g;
				$s =~ s/[^\w\- ]//g;
				$s =~ s/ /-/g;
				my $k = $n{$s}++;
				$a{ $k ? "$s-$k" : $s } = 1;
			}
			$a{$_} = 1 for $t =~ /<a\s+(?:name|id)="([^"]+)"/g;
			\%a;
		};
	};
	# In a release tree (git archive: no .git), the export-ignore paths of
	# .gitattributes are absent, so developer docs' links into them (DESIGN.md
	# to research/, say) can't resolve: a missing target is skipped when its
	# export-ignored root is absent too. 043_release_tree.pl makes sure no
	# user doc links there.
	my @export_ignored;
	if (!-e "$root/.git" && -f "$root/.gitattributes")
	{
		for (split /\n/, _slurp("$root/.gitattributes"))
		{
			my ($pat, @attrs) = split ' ';
			next unless defined $pat && $pat =~ m{^/} && grep { $_ eq 'export-ignore' } @attrs;
			push @export_ignored, $pat =~ s{^/}{}r;
		}
	}
	my $omitted = sub {
		my ($target) = @_;
		return 0 unless index($target, "$root/") == 0;
		my @out;
		for (split m{/}, substr($target, length("$root/")))
		{
			next if $_ eq '' || $_ eq '.';
			if ($_ eq '..') { pop @out; } else { push @out, $_; }
		}
		my $rel = join '/', @out;
		# Only when the root itself was left out: in docker/run-tests.sh's
		# copy (no .git either) research/ and the backlog are still there.
		return scalar grep { ($rel eq $_ || index($rel, "$_/") == 0) && !-e "$root/$_" }
		  @export_ignored;
	};
	my @bad;
	my $nlinks = 0;
	my $nomitted = 0;
	for my $f (sort @files)
	{
		my $t = _slurp($f);
		$t =~ s/^```.*?^```//msg;
		$t =~ s/`[^`\n]*`//g;
		my @links = $t =~ /\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g;
		push @links, $t =~ /^\[[^\]]+\]:\s*(\S+)/mg;
		for my $l (@links)
		{
			next if $l =~ /^[a-z][a-z0-9+.-]*:/i;
			$nlinks++;
			my ($p, $a) = split /#/, $l, 2;
			my $target = $p eq '' ? $f : dirname($f) . "/$p";
			if (!-e $target && $omitted->($target)) { $nomitted++; next; }
			if (!-e $target) { push @bad, "$f: $l (no such file)"; next; }
			push @bad, "$f: $l (no such anchor)"
			  if defined $a && $target =~ /\.md$/ && !$anchors_of->($target)->{$a};
		}
	}
	return { nlinks => $nlinks, nomitted => $nomitted, bad => \@bad };
}

1;
