# Release tree and documentation hygiene. No server: plain file checks, so
# it runs the same in a git checkout, in docker/run-tests.sh's copy and in a
# tree extracted from a release tarball (git archive).
#
# Documents are classified as:
#   user docs       README.md, CHANGELOG.md and docs/**/*.md except
#                   docs/maintaining.md: what an operator reads.
#   developer docs  DESIGN.md, docs/maintaining.md, fuzz/README.md and
#                   .github/: they ship in release tarballs, and a user doc
#                   may link to them only when the link is labeled, i.e. its
#                   paragraph or list item, or the heading above it, says
#                   "developer(s)".
#   development-only  every path marked export-ignore in .gitattributes
#                   (agent and planning files, research/): not in release
#                   tarballs, so a user doc never links to or names them (a
#                   labeled link to the repository on the web is fine).
# Scripts, tests, docker/ and bench/ (harness, results and bench/README.md,
# which tells anyone how to reproduce docs/benchmarks.md) are source that
# users run: links to them need no label.
#
# Also checked: .gitattributes excludes the agent and planning files and
# research/ from tarballs but keeps what building, testing and regenerating
# the docs need; DESIGN.md describes the implemented version (no "Draft" or
# "(proposed)"); src/ carries no backlog IDs; the README has a glossary and
# says who holds the copyright (NOTICE).
use strict;
use warnings;

use File::Basename qw(dirname);
use File::Find;
use File::Path qw(make_path remove_tree);
use FindBin;
use lib "$FindBin::Bin/../perl";
use PsscDocLinks qw(check_relative_links);
use Test::More;

my $root = dirname(__FILE__) . '/../..';

sub slurp
{
	my ($f) = @_;
	open my $fh, '<', $f or die "open $f: $!";
	local $/;
	return scalar <$fh>;
}

# A backlog item ID (YYYYMMDD-HHMMSS-N) or its short form ("item -30").
my $backlog_id = qr/\b20\d{6}-\d{6}-\d+\b|\b[Ii]tems? -\d+\b|\b[Ii]tem \d{6}-\d+\b/;

# ------------------------------------------------------------ .gitattributes
my @ignored;
{
	ok(-f "$root/.gitattributes", '.gitattributes exists');
	my $ga = -f "$root/.gitattributes" ? slurp("$root/.gitattributes") : '';
	for my $line (split /\n/, $ga)
	{
		next if $line =~ /^\s*(?:#|$)/;
		my ($pat, @attrs) = split ' ', $line;
		# A directory is matched without a trailing slash, so that git
		# archive leaves out the whole tree.
		push @ignored, $pat if grep { $_ eq 'export-ignore' } @attrs;
		ok($pat =~ m{^/[^*]*[^/]$}, "export-ignore pattern $pat is a rooted path")
		  if grep { $_ eq 'export-ignore' } @attrs;
	}
	for my $p ('/CLAUDE.md', '/BACKLOG.md', '/BACKLOG-COMPLETE.md', '/research',
		'/worktrees', '/tmp')
	{
		ok((grep { $_ eq $p } @ignored), "export-ignore $p");
	}
	# Needed to build, test, package, or regenerate docs/benchmarks.md.
	for my $p (@ignored)
	{
		(my $path = $p) =~ s{^/}{};
		ok($path !~ m{^(?:src|sql|test|docker|docs|bench|fuzz|Makefile|LICENSE|NOTICE|README\.md|CHANGELOG\.md|DESIGN\.md|pg_stat_statement_context\.control|\.gitattributes)(?:/|$)}
			  && $path !~ m{^scripts/(?!backlog-complete\.py$)},
			"export-ignore $p is not needed by a release tree");
	}
}

# Is a repository-relative path development-only (export-ignored)?
sub dev_only
{
	my ($path) = @_;
	for my $p (@ignored)
	{
		(my $q = $p) =~ s{^/}{};
		return 1 if $path eq $q || index($path, "$q/") == 0;
	}
	return 0;
}

sub developer_doc
{
	my ($path) = @_;
	return $path =~ m{^(?:DESIGN\.md|docs/maintaining\.md|fuzz/README\.md|\.github(?:/|$))};
}

# Lexically normalizes "a/b/../c" relative to the repository root.
sub repo_path
{
	my ($from, $link) = @_;
	my @parts = split m{/}, dirname($from) . "/$link";
	my @out;
	for (@parts)
	{
		next if $_ eq '' || $_ eq '.';
		if ($_ eq '..') { pop @out; next; }
		push @out, $_;
	}
	return join '/', @out;
}

# ------------------------------------------------------------ user docs
my @user_docs = ('README.md', 'CHANGELOG.md');
find(sub {
		push @user_docs, substr($File::Find::name, length("$root/"))
		  if /\.md$/ && $File::Find::name !~ m{/docs/maintaining\.md$};
	}, "$root/docs");
@user_docs = sort @user_docs;
ok(@user_docs >= 10, 'user docs found (' . scalar(@user_docs) . ')');
ok(!dev_only($_), "$_ ships in release tarballs") for @user_docs;

{
	my (@bad, @mentions);
	my $checked = 0;
	for my $f (@user_docs)
	{
		my $t = slurp("$root/$f");
		$t =~ s/^```.*?^```//msg;
		my $heading = '';
		for my $block (split /\n(?:[ \t]*\n)+|\n(?=#)|\n(?=\s*[-*] )|\n(?=\s*\d+\. )/, $t)
		{
			$heading = $1 if $block =~ /^#{1,6}\s+(.*)$/m;
			my $labeled = $block =~ /\bdevelopers?\b/i || $heading =~ /\bdevelop/i;
			(my $plain = $block) =~ s/`[^`\n]*`//g;
			my @links = $plain =~ /\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g;
			push @links, $plain =~ /^\[[^\]]+\]:\s*(\S+)/mg;
			for my $l (@links)
			{
				next if $l =~ /^[a-z][a-z0-9+.-]*:/i;
				my ($p) = split /#/, $l, 2;
				next if $p eq '';
				my $target = repo_path($f, $p);
				$checked++;
				if (dev_only($target))
				{
					push @bad, "$f: $l (development-only, not in release tarballs)";
				}
				elsif (developer_doc($target) && !$labeled)
				{
					push @bad, "$f: $l (developer doc; label the link as a developer reference)";
				}
			}
			# A labeled link may point to the repository on the web.
			(my $named = $block) =~ s/\]\([a-z][a-z0-9+.-]*:[^)]*\)/]()/gi if $labeled;
			$named //= $block;
			push @mentions, "$f: $1"
			  while $named =~ /(\bBACKLOG(?:-COMPLETE)?\.md\b|\bCLAUDE\.md\b|\bresearch\/|$backlog_id)/g;
		}
	}
	ok($checked > 30, "relative links of user docs checked ($checked)");
	is_deeply(\@bad, [], 'user docs link to no development-only file, and label developer references')
	  or diag(join("\n", @bad));
	is_deeply(\@mentions, [], 'user docs name no backlog item or development-only file')
	  or diag(join("\n", @mentions));
}

# ------------------------------------------------------------ DESIGN.md
{
	my $d = slurp("$root/DESIGN.md");
	my ($status) = $d =~ /^> Status: (.*)$/m;
	ok(defined $status, 'DESIGN.md has a status line');
	like($status // '', qr/\bv1\.0\b/, 'DESIGN.md status names the implemented version');
	my @stale = $d =~ /^(.*(?:\bDraft\b|\(proposed\)).*)$/mg;
	is_deeply(\@stale, [], 'DESIGN.md has no "Draft" or "(proposed)" marker')
	  or diag(join("\n", @stale));
}

# ------------------------------------------------------------ src/
{
	my @hits;
	for my $f (sort glob("$root/src/*.[ch]"))
	{
		my $t = slurp($f);
		my $n = 0;
		for my $line (split /\n/, $t)
		{
			$n++;
			push @hits, "$f:$n: $line" if $line =~ /$backlog_id|\bbacklog\b/i;
		}
	}
	is_deeply(\@hits, [], 'src/ comments cite no backlog item') or diag(join("\n", @hits));
}

# ------------------------------------------------------------ README
{
	my $readme = slurp("$root/README.md");
	my ($glossary) = $readme =~ /^## Glossary\n(.*?)(?=^## |\z)/ms;
	ok(defined $glossary, 'README.md has a glossary');
	for my $term ('Tag', 'Tag set', 'Context', 'Key', 'Extractor', 'Frame', 'Bucket')
	{
		like($glossary // '', qr/^- \*\*\Q$term\E\*\*/m, "the glossary defines \"$term\"");
	}
	# exclude_tags applies only with tags = '*' (docs/configuration.md).
	my ($key_def) = ($glossary // '') =~ /^- \*\*Key\*\*(.*)$/m;
	like($key_def // '', qr/exclude_tags.*\Qtags = '*'\E|\Qtags = '*'\E.*exclude_tags/,
		'the glossary limits exclude_tags to tags = \'*\'')
	  if ($key_def // '') =~ /exclude_tags/;
	ok(-f "$root/NOTICE", 'NOTICE exists');
	my $notice = -f "$root/NOTICE" ? slurp("$root/NOTICE") : '';
	like($notice, qr/\bLICENSE\b/, 'NOTICE refers to LICENSE');
	like($notice, qr/^Copyright\b/m, 'NOTICE has a copyright line');
	my ($license) = $readme =~ /^## License\n(.*?)(?=^## |\z)/ms;
	like($license // '', qr/\[NOTICE\]\(NOTICE\)/, 'README.md license section links to NOTICE');
	like($license // '', qr/[Cc]opyright/, 'README.md says who holds the copyright');
}

# ------------------------------------------------------------ link checker
# check_relative_links() (037's relative-link check) skips a missing link
# target only in a release tree: no .git, and the export-ignored root that
# would hold it absent. docker/run-tests.sh's copy has no .git but keeps
# research/ and the backlog, so a broken link into them must still fail.
{
	my $fx = "$root/tmp_check/043_links";
	my $mk = sub {
		my (%o) = @_;
		remove_tree($fx);
		make_path("$fx/docs");
		my $w = sub {
			my ($f, $t) = @_;
			make_path(dirname("$fx/$f"));
			open my $fh, '>', "$fx/$f" or die "write $fx/$f: $!";
			print $fh $t;
			close $fh;
		};
		$w->('.gitattributes', "/research export-ignore\n/BACKLOG.md export-ignore\n");
		$w->('DESIGN.md',
			"# Design\n\nSee [gone](research/drivers/does-not-exist.md), "
			  . "[log](BACKLOG.md#x) and [user](docs/a.md#a).\n");
		$w->('docs/a.md', "# A\n");
		$w->('research/drivers/README.md', "# Drivers\n") if $o{research};
		$w->('BACKLOG.md', "# Backlog\n") if $o{backlog};
		make_path("$fx/.git") if $o{git};
	};

	$mk->(research => 1, backlog => 1);
	my $r = check_relative_links($fx);
	is_deeply([ sort @{ $r->{bad} } ],
		[ "$fx/DESIGN.md: BACKLOG.md#x (no such anchor)",
		  "$fx/DESIGN.md: research/drivers/does-not-exist.md (no such file)" ],
		'no .git, export-ignored roots present: broken links into them fail');
	is($r->{nomitted}, 0, '... and none is skipped');

	$mk->();
	$r = check_relative_links($fx);
	is_deeply($r->{bad}, [], 'release tree (roots absent, no .git): links into them are skipped');
	is($r->{nomitted}, 2, '... both counted as skipped');
	is($r->{nlinks}, 3, '... and the other link is checked');

	$mk->(git => 1);
	$r = check_relative_links($fx);
	is(scalar @{ $r->{bad} }, 2, 'a git checkout missing the roots fails');

	remove_tree($fx);
}

done_testing();
