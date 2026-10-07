#!/usr/bin/perl
#
# regex_fuzz.pl
#	SQL-level fuzz driver for the regex extractor (DESIGN.md §4.2, §9).
#
# Generates regex extractor configurations and queries with comments from a
# seeded PRNG and runs them through pg_stat_statement_context_extract() on a
# running server (fuzz/sql/container.sh starts it in Docker; see
# fuzz/README.md). Each epoch restarts the server with new postmaster limits
# (max_tags, max_tag_value_len, max_tagset_bytes). Each round:
#
#   1. asks the server whether the pattern compiles (regexp_matches), then
#	  sets extractors/tags/exclude_tags/scan_window (and normalize = '')
#	  with ALTER SYSTEM and reloads. A rejection must be the one predicted from the pattern
#	  (length, compile error, back-reference, too many groups, keys versus
#	  groups), and a configuration predicted to be rejected must not pass;
#   2. runs a batch of extract calls in a UTF8 or a SQL_ASCII database and
#	  checks every result: the documented keys, ntags/tagset_bytes within
#	  the limits, statement range within the query, no regex compile failure,
#	  every value a slice of the query no longer than max_tag_value_len,
#	  keys only from the extractor (filtered by tags/exclude_tags), argument
#	  errors exactly for invalid arguments, and the same result when a call
#	  is repeated;
#   3. for single-comment queries compares the tags with an oracle built from
#	  regexp_matches(body, pattern, 'g') (first match per group, truncation
#	  on a character boundary);
#   4. fails on a lost connection, a hang, an unexpected ERROR, or a crash,
#	  assertion failure (TRAP) or PANIC in the server log.
#
# Everything is derived from --seed (printed at the start), so a run is
# reproduced with the same --seed and --rounds. On a failure the round's SQL,
# psql output, server log and parameters are saved to <out>/fail-round-<n>/.
#
# Usage (inside the container; see fuzz/sql/run.sh):
#	regex_fuzz.pl --bindir DIR --pgdata DIR --work DIR --out DIR
#		[--seed N] [--duration SECONDS] [--rounds N] [--calls N] [--valgrind]

use strict;
use warnings;
use Getopt::Long qw(GetOptions);
use JSON::PP ();
use Time::HiRes qw(time);
use File::Path qw(make_path);
use File::Basename qw(dirname);
use File::Spec ();

my %opt = (
	duration => 60,
	rounds => 0,
	rounds_per_epoch => 25,
	calls => 40,
	port => 5499,
	valgrind => 0,
);
GetOptions(
	'seed=i' => \$opt{seed},
	'duration=i' => \$opt{duration},
	'rounds=i' => \$opt{rounds},
	'rounds-per-epoch=i' => \$opt{rounds_per_epoch},
	'calls=i' => \$opt{calls},
	'bindir=s' => \$opt{bindir},
	'pgdata=s' => \$opt{pgdata},
	'work=s' => \$opt{work},
	'out=s' => \$opt{out},
	'port=i' => \$opt{port},
	'valgrind' => \$opt{valgrind},
	'self-test' => \$opt{self_test},
) or die "bad options\n";
for (qw(bindir pgdata work out))
{
	last if $opt{self_test};
	die "--$_ is required\n" unless defined $opt{$_};
}
$opt{seed} = (int(time() * 1000) ^ ($$ << 8)) & 0x7fffffff
  unless defined $opt{seed};
$| = 1;

my $SLOW = $opt{valgrind} ? 20 : 1;
my $STMT_TIMEOUT_S = 20 * $SLOW;
my $ROUND_TIMEOUT_S = 300 * $SLOW;
my $RESOURCE_RE = qr/invalid memory alloc request size|out of memory|regular expression is too complex|statement timeout/;
my $CRASH_RE = qr/TRAP:|terminated by signal|PANIC|server process \(PID \d+\) (?:was terminated|exited with exit code)|terminating any other active server processes|server closed the connection|connection to server was lost/;
my @RESULT_KEYS = sort qw(tags ntags tagset_bytes footer heuristic oom stmt_start
  stmt_end invalid_tags dropped_tags heuristic_scans regex_compile_failures
  normalized_tags normalize_failures capped_tags);
my $JSON = JSON::PP->new->utf8;

print "regex_fuzz: seed $opt{seed}\n" unless $opt{self_test};

# ---------------- PRNG (xorshift32; deterministic across platforms) ----------------

package Rng;

sub new
{
	my ($class, $seed) = @_;
	my $s = ($seed ^ 0x9e3779b9) & 0xffffffff;
	$s = 0x2545f491 if $s == 0;
	my $self = bless { s => $s }, $class;
	$self->next for 1 .. 8;
	return $self;
}

sub next
{
	my $self = shift;
	my $x = $self->{s};
	$x ^= ($x << 13) & 0xffffffff;
	$x ^= $x >> 17;
	$x ^= ($x << 5) & 0xffffffff;
	return $self->{s} = $x;
}

# 0 .. n-1
sub int { my ($self, $n) = @_; return $n <= 1 ? 0 : $self->next % $n; }
sub chance { my ($self, $pct) = @_; return $self->int(100) < $pct; }
sub pick { my ($self, @a) = @_; return $a[ $self->int(scalar @a) ]; }
sub shuffle
{
	my ($self, @a) = @_;
	for (my $i = $#a; $i > 0; $i--)
	{
		my $j = $self->int($i + 1);
		@a[ $i, $j ] = @a[ $j, $i ];
	}
	return @a;
}
sub range { my ($self, $lo, $hi) = @_; return $lo + $self->int($hi - $lo + 1); }

package main;

# ---------------- character sets ----------------

# Characters as Perl strings; UTF-8 encoded when written to SQL.
my @ASCII_TEXT = (('a' .. 'f'), qw(x y z k v 0 1 2 3 = : _ -), ' ', ',', ';', "\t");
my @UNI_TEXT = ("\x{e9}", "\x{df}", "\x{65e5}", "\x{672c}", "\x{20ac}", "\x{1f600}");
my @PAT_LIT = (('a' .. 'f'), qw(x y z k v 0 1 2 = : _ -), ' ', ',');
my @META = split //, '\\^$.|?*+()[]{}';
my %IS_META = map { $_ => 1 } @META;

# bytes -> E'' literal; control bytes, quotes and backslashes escaped
sub sql_lit_bytes
{
	my ($b) = @_;
	my $s = '';
	for my $c (split //, $b)
	{
		my $o = ord $c;
		if ($c eq "'" || $c eq '\\') { $s .= "\\$c"; }
		elsif ($o < 0x20 || $o == 0x7f) { $s .= sprintf '\\x%02x', $o; }
		else { $s .= $c; }
	}
	return "E'$s'";
}

sub sql_std_lit
{
	my ($b) = @_;
	(my $s = $b) =~ s/'/''/g;
	return "'$s'";
}

sub enc { my ($s) = @_; my $b = $s; utf8::encode($b); return $b; }

# ---------------- regex generator ----------------
#
# Builds a pattern string and a sampler that returns a string the pattern
# probably matches (used to seed comment bodies so that matches are common).
# Tracks the capture groups the engine will report (parentheses inside
# lookaround constraints do not capture) and whether a back-reference was used.

sub gen_char
{
	my ($r, $st) = @_;
	return $st->{utf8} && $r->chance(15) ? $r->pick(@UNI_TEXT) : $r->pick(@PAT_LIT);
}

sub lit_node
{
	my ($c) = @_;
	return { pat => ($IS_META{$c} ? "\\$c" : $c), sample => sub { $c } };
}

sub gen_atom
{
	my ($r, $st) = @_;
	my $k = $r->int(100);
	if ($k < 40)
	{
		return lit_node(gen_char($r, $st));
	}
	if ($k < 47)
	{
		return { pat => '.', sample => sub { $_[0]->pick(@ASCII_TEXT) } };
	}
	if ($k < 67)
	{
		# bracket expression
		my $neg = $r->chance(15);
		my @items;
		my @samp;
		for (1 .. $r->range(1, 4))
		{
			my $t = $r->int(10);
			if ($t < 4)
			{
				my $c = gen_char($r, $st);
				next if $c eq '-' || $c eq '^';
				push @items, $c;
				push @samp, $c;
			}
			elsif ($t < 6)
			{
				my ($lo, $hi) = @{ $r->pick(['a', 'f'], ['0', '9'], ['x', 'z'], ['A', 'Z']) };
				push @items, "$lo-$hi";
				push @samp, map { chr } ord($lo) .. ord($hi);
			}
			elsif ($t < 8)
			{
				my ($cls, @s) = @{ $r->pick([alpha => 'a', 'Z'], [digit => '1'], [space => ' '],
						[alnum => 'b', '7'], [upper => 'Q'], [punct => '=', ':']) };
				push @items, "[:$cls:]";
				push @samp, @s;
			}
			else
			{
				my ($e, @s) = @{ $r->pick(['\\w', 'a', '_'], ['\\d', '5'], ['\\s', ' ']) };
				push @items, $e;
				push @samp, @s;
			}
		}
		@samp = ('a') unless @samp;
		@items = ('a') unless @items;
		return {
			pat => '[' . ($neg ? '^' : '') . join('', @items) . ']',
			sample => $neg ? sub { $_[0]->pick(@ASCII_TEXT) } : sub { $_[0]->pick(@samp) },
		};
	}
	if ($k < 82)
	{
		my ($e, @s) = @{ $r->pick(['\\w', 'a', 'k', '_'], ['\\d', '0', '9'], ['\\s', ' '],
				['\\W', '=', ' '], ['\\D', 'x'], ['\\S', 'v', '='], ['\\t', "\t"],
				['\\u00e9', "\x{e9}"], ['\\u65e5', "\x{65e5}"], ['\\U0001F600', "\x{1f600}"],
				['\\.', '.'], ['\\*', '*'], ['\\/', '/']) };
		return { pat => $e, sample => sub { $_[0]->pick(@s) } };
	}
	if ($k < 92)
	{
		my $a = $r->pick('^', '$', '\\A', '\\Z', '\\m', '\\M', '\\y', '\\Y');
		return { pat => $a, sample => sub { '' } };
	}
	if ($k < 95 && $st->{ngroups} > 0 && !$st->{in_look})
	{
		$st->{backref} = 1;
		my $n = $r->range(1, $st->{ngroups});
		# wrapped so a following digit cannot turn "\1" into octal "\11"
		return { pat => "(?:\\$n)", sample => sub { '' } };
	}
	return lit_node($r->pick(@META));
}

sub gen_node
{
	my ($r, $st, $depth) = @_;
	my $k = $r->int(100);
	return gen_atom($r, $st) if $depth <= 0 || $k < 30;
	if ($k < 50)
	{
		my @kids = map { gen_node($r, $st, $depth - 1) } 1 .. $r->range(2, 4);
		return {
			pat => join('', map { $_->{pat} } @kids),
			sample => sub { my $g = shift; join '', map { $_->{sample}->($g) } @kids },
			alt => (grep { $_->{alt} } @kids) ? 1 : 0,
		};
	}
	if ($k < 58)
	{
		my @kids = map { gen_node($r, $st, $depth - 1) } 1 .. $r->range(2, 3);
		return {
			pat => join('|', map { $_->{pat} } @kids),
			sample => sub { my $g = shift; $g->pick(@kids)->{sample}->($g) },
			alt => 1,
		};
	}
	if ($k < 72)
	{
		$st->{ngroups}++ unless $st->{in_look};
		my $kid = gen_node($r, $st, $depth - 1);
		return { pat => "($kid->{pat})", sample => $kid->{sample} };
	}
	if ($k < 78)
	{
		my $kid = gen_node($r, $st, $depth - 1);
		return { pat => "(?:$kid->{pat})", sample => $kid->{sample} };
	}
	if ($k < 84)
	{
		my $saved = $st->{in_look};
		$st->{in_look} = 1;
		my $kid = gen_node($r, $st, $depth - 1);
		$st->{in_look} = $saved;
		my $op = $r->pick('?=', '?!', '?<=', '?<!');
		return { pat => "($op$kid->{pat})", sample => sub { '' } };
	}
	my $kid = gen_node($r, $st, $depth - 1);
	$kid = { pat => "(?:$kid->{pat})", sample => $kid->{sample} } if $kid->{alt};
	my ($q, $lo, $hi) = @{ $r->pick(['*', 0, 3], ['+', 1, 3], ['?', 0, 1], ['{2}', 2, 2],
			['{1,}', 1, 3], ['{0,2}', 0, 2], ['{1,3}', 1, 3], ['{3,5}', 3, 5],
			['{0,255}', 0, 3], ['{256}', 1, 1], ['{3,1}', 1, 1]) };
	$q .= '?' if $r->chance(20);
	return {
		pat => $kid->{pat} . $q,
		sample => sub { my $g = shift; join '', map { $kid->{sample}->($g) } 1 .. $g->range($lo, $hi) },
	};
}

# A "house format" pattern: key literals, a separator and a captured value.
sub gen_structured
{
	my ($r, $st) = @_;
	my @parts;
	for (1 .. $r->range(1, 4))
	{
		my $name = join '', map { $r->pick('a' .. 'f', 'x', 'k', 'v') } 1 .. $r->range(1, 4);
		my $sep = $r->pick('=', ':', '\\s*=\\s*', '=\\s*', ': ');
		my ($val, @vs) = @{ $r->pick(['\\w+', 'abc', 'x_1'], ['[^,\\s]+', 'v.1', "\x{e9}t\x{e9}"],
				['[a-f0-9]{1,8}', 'beef', '0'], ['.*?', '', 'q'], ['[^ ]*', 'zz'], ['\\S+', "\x{65e5}"]) };
		my $opt = $r->chance(20);
		$st->{ngroups}++;
		push @parts, {
			pat => ($opt ? "(?:$name$sep($val))?" : "$name$sep($val)"),
			sample => sub {
				my $g = shift;
				(my $s = $sep) =~ s/\\s\*/$g->pick('', ' ')/ge;
				return "$name$s" . $g->pick(@vs);
			},
		};
	}
	my $join = $r->pick('\\s+', ',\\s*', '.*?', ' ');
	return {
		pat => join($join, map { $_->{pat} } @parts),
		sample => sub {
			my $g = shift;
			join($join eq ',\\s*' ? ', ' : ' ', map { $_->{sample}->($g) } @parts);
		},
	};
}

sub gen_pattern
{
	my ($r, $utf8) = @_;
	my $st = { ngroups => 0, backref => 0, in_look => 0, utf8 => $utf8 };
	my $node = $r->chance(45) ? gen_structured($r, $st) : gen_node($r, $st, $r->range(1, 4));
	if ($st->{ngroups} == 0 && $r->chance(85))
	{
		$st->{ngroups}++;
		$node = { pat => "($node->{pat})", sample => $node->{sample} };
	}
	my $pat = $node->{pat};
	my $k = $r->int(100);
	if ($k < 8)
	{
		$pat = $r->pick('(?i)', '(?c)', '(?n)', '(?s)', '(?w)', '(?p)', '(?t)', '(?x)', '(?in)', '***:') . $pat;
	}
	elsif ($k < 10)
	{
		$pat = $r->pick('(?q)', '***=') . $pat;	# the rest is a literal: no groups
		$st->{ngroups} = 0;
		$st->{backref} = 0;
	}
	elsif ($k < 13)
	{
		# long patterns around the 1 kB limit
		my $pad = join '', map { $r->pick('a' .. 'f') } 1 .. $r->range(950, 1050);
		$pat = "(?:$pad)?" . $pat;
	}
	elsif ($k < 16)
	{
		# probably invalid; a prefix such as '[' or '\\' may also change
		# what the rest means, so the groups are no longer known
		$pat = $r->pick('(', ')', '[', '\\', '*', '{', 'a{1', '[[:nope:]]', '(?', '(?<x>a)') . $pat;
		$st->{fuzzy} = 1;
	}
	return ($pat, $st, $node->{sample});
}

# ---------------- comment and query generator ----------------

sub rand_text
{
	my ($r, $n) = @_;
	my $s = '';
	for (1 .. $n)
	{
		$s .= $r->chance(12) ? $r->pick(@UNI_TEXT) : $r->pick(@ASCII_TEXT);
	}
	return $s;
}

# A comment body as bytes. In SQL_ASCII some non-ASCII bytes are replaced by
# random ones, so bodies may be invalid UTF-8 there.
sub gen_body
{
	my ($r, $utf8, $sample) = @_;
	my $s = '';
	for (1 .. $r->range(1, 4))
	{
		my $k = $r->int(10);
		if ($k < 5) { $s .= $sample->($r); }
		elsif ($k < 8) { $s .= rand_text($r, $r->range(0, 12)); }
		elsif ($k < 9) { $s .= rand_text($r, $r->range(100, 600)); }
		else { $s .= ' '; }
	}
	$s = enc($s);
	$s =~ s/([\x80-\xff])/$r->chance(30) ? chr($r->range(0x80, 0xff)) : $1/ge unless $utf8;
	$s =~ tr/\0//d;
	return $s;
}

# No comment open or close inside a block comment body.
sub block_safe
{
	my ($b) = @_;
	$b =~ s{\*/|/\*}{* /}g while $b =~ m{\*/|/\*};
	$b .= ' ' if $b =~ m{/$};
	return $b;
}

# Noise: statements, literals and comments the scanner must not confuse.
sub gen_noise_query
{
	my ($r, $utf8, $sample) = @_;
	my @tok = (
		'SELECT 1', 'SELECT $$ /* x */ $$', "SELECT '/* not a comment */'", "SELECT E'\\' /*'",
		'SELECT $q$ -- $q$', 'SELECT "a/*b"', ';', ' ', "\n", "\r\n", "\t", '/* a /* nested */ b */',
		'-- line', "--x\r", '/**/', '/*/', 'SELECT 1 /*', "SELECT '", 'SELECT $a$ ', 'x', '$1',
		'U&"d\\0061t"', "B'01'", '1.5e3', '::int', 'WITH a AS (SELECT 1) SELECT * FROM a',
	);
	my $q = '';
	for (1 .. $r->range(1, 10))
	{
		my $k = $r->int(10);
		if ($k < 4) { $q .= $r->pick(@tok); }
		elsif ($k < 7) { $q .= ' /*' . block_safe(gen_body($r, $utf8, $sample)) . '*/ '; }
		elsif ($k < 8) { (my $b = gen_body($r, $utf8, $sample)) =~ tr/\n\r/  /; $q .= " --$b\n"; }
		elsif ($k < 9) { $q .= ' ' x $r->range(50, 3000); }
		else { $q .= gen_body($r, $utf8, $sample); }
	}
	return $q;
}

# ---------------- server ----------------

my $log_path;
my $log_pos = 0;
my $epoch = -1;
my %limits;

sub pg_ctl
{
	my @args = @_;
	return system("$opt{bindir}/pg_ctl", '-D', $opt{pgdata}, @args) == 0;
}

sub server_stop
{
	pg_ctl('-m', 'fast', '-w', '-t', 60 * $SLOW, 'stop') if -e "$opt{pgdata}/postmaster.pid";
}

sub new_log
{
	open my $fh, '<:raw', $log_path or return '';
	seek $fh, $log_pos, 0;
	local $/;
	my $s = <$fh> // '';
	$log_pos += length $s;
	close $fh;
	return $s;
}

sub psql_file
{
	my ($db, $enc, $file, $outf, $errf) = @_;
	my $cmd = "PGCLIENTENCODING=$enc timeout $ROUND_TIMEOUT_S '$opt{bindir}/psql' -X -q -At "
	  . "-h '$opt{work}' -p $opt{port} -d $db -f '$file' > '$outf' 2> '$errf'";
	my $rc = system('/bin/sh', '-c', $cmd);
	return $rc == -1 ? 255 : $rc >> 8;
}

sub slurp
{
	my ($f) = @_;
	open my $fh, '<:raw', $f or return '';
	local $/;
	my $s = <$fh>;
	close $fh;
	return $s // '';
}

sub spit
{
	my ($f, $s) = @_;
	open my $fh, '>:raw', $f or die "$f: $!";
	print $fh $s;
	close $fh;
}

# ---------------- failures ----------------

my %stats = (rounds => 0, calls => 0, oracle => 0, rejected => 0, oracle_err => 0,
	timeouts => 0, arg_errors => 0, tags => 0, repeat => 0, resource => 0,
	oracle_drop => 0);
my %reject_reasons;
my $cur_round = 0;
my @problems;

sub problem { push @problems, join '', @_; }

sub fail_run
{
	my ($why, $files) = @_;
	my $dir = "$opt{out}/fail-round-$cur_round";
	make_path($dir);
	for my $k (sort keys %{ $files // {} })
	{
		system('cp', $files->{$k}, "$dir/") if -e $files->{$k};
	}
	system('cp', $log_path, "$dir/server.log") if $log_path && -e $log_path;
	spit("$dir/params.txt", "seed $opt{seed}\nround $cur_round\nepoch $epoch\n"
		  . join('', map { "$_ = $limits{$_}\n" } sort keys %limits)
		  . "reproduce: fuzz/sql/run.sh [flavor] -- --seed $opt{seed} --rounds " . ($cur_round + 1) . "\n"
		  . "\n$why\n");
	print "FAIL (seed $opt{seed}, round $cur_round): $why\n";
	print "  saved to fail-round-$cur_round/\n";
	server_stop();
	exit 1;
}

sub server_start
{
	$epoch++;
	$log_path = "$opt{work}/epoch-$epoch.log";
	$log_pos = 0;
	my $o = join ' ', "-p $opt{port}", "-c listen_addresses=''",
	  "-c unix_socket_directories=$opt{work}",
	  map { "-c pg_stat_statement_context.$_=$limits{$_}" } sort keys %limits;
	pg_ctl('-l', $log_path, '-w', '-t', 60 * $SLOW, '-o', $o, 'start')
	  or fail_run("server did not start (epoch $epoch)", undef);
}

# ---------------- checks ----------------

sub unescape_sql_ascii
{
	my ($v) = @_;
	$v =~ s/\\(\\|x([0-9a-fA-F]{2}))/defined $2 ? chr(hex $2) : '\\'/ge;
	return $v;
}

# The longest prefix of v of at most max bytes on a character boundary.
sub clip
{
	my ($v, $max, $utf8) = @_;
	return $v if length $v <= $max;
	return substr($v, 0, $max) unless $utf8;
	my $n = 0;
	while ($n < length $v)
	{
		my $c = ord substr($v, $n, 1);
		my $l = $c < 0x80 ? 1 : $c < 0xe0 ? 2 : $c < 0xf0 ? 3 : 4;
		last if $n + $l > $max;
		$n += $l;
	}
	return substr($v, 0, $n);
}

sub check_result
{
	my ($c, $json_text, $cfg) = @_;
	my $res = eval { $JSON->decode($json_text) };
	my $where = "call $c->{id} ($c->{desc})";
	unless (ref $res eq 'HASH')
	{
		problem("$where: bad JSON: $json_text");
		return;
	}
	my @keys = sort keys %$res;
	if ("@keys" ne "@RESULT_KEYS")
	{
		problem("$where: result keys @keys");
		return;
	}
	my $tags = $res->{tags};
	if (ref $tags ne 'HASH')
	{
		problem("$where: tags is not an object");
		return;
	}
	my $ntags = scalar keys %$tags;
	my $qlen = length $c->{q};
	problem("$where: ntags $res->{ntags} but $ntags tags") if $res->{ntags} != $ntags;
	problem("$where: ntags $ntags > max_tags $limits{max_tags}") if $ntags > $limits{max_tags};
	problem("$where: tagset_bytes $res->{tagset_bytes} > max_tagset_bytes")
	  if $res->{tagset_bytes} > $limits{max_tagset_bytes};
	problem("$where: tagset_bytes $res->{tagset_bytes} without tags")
	  if $ntags == 0 && $res->{tagset_bytes} != 0;
	problem("$where: oom") if $res->{oom};
	problem("$where: stmt range $res->{stmt_start}..$res->{stmt_end} of $qlen bytes")
	  unless 0 <= $res->{stmt_start} && $res->{stmt_start} <= $res->{stmt_end}
	  && $res->{stmt_end} <= $qlen;
	problem("$where: regex_compile_failures $res->{regex_compile_failures}")
	  if $res->{regex_compile_failures} != 0;
	# the driver pins normalize to '' (no rules)
	for my $k (qw(normalized_tags normalize_failures))
	{
		problem("$where: $k $res->{$k} without normalize rules") if $res->{$k} != 0;
	}
	problem("$where: heuristic on a $qlen-byte query with scan_window $cfg->{window}")
	  if $res->{heuristic} && $qlen <= $cfg->{window};
	# the driver sets no cardinality_cap GUCs (default 0: no cap)
	problem("$where: capped_tags $res->{capped_tags} without a cardinality cap")
	  if $res->{capped_tags} > 0;
	problem("$where: negative counter")
	  if grep { $res->{$_} < 0 } qw(invalid_tags dropped_tags heuristic_scans capped_tags);

	my %vals;
	for my $k (sort keys %$tags)
	{
		my $v = $tags->{$k};
		if (ref $v || !defined $v)
		{
			problem("$where: value of $k is not a string");
			next;
		}
		$v = enc($v);
		$v = unescape_sql_ascii($v) unless $cfg->{utf8};
		$vals{$k} = $v;
		problem("$where: value of $k is ", length $v, " bytes > max_tag_value_len")
		  if length $v > $limits{max_tag_value_len};
		next unless $cfg->{regex_only};
		problem("$where: key $k is not an extractor key") unless $cfg->{keyset}{$k};
		problem("$where: key $k is not allowed by tags/exclude_tags") unless $cfg->{allowed}{$k};
		problem("$where: value of $k is not a slice of the query") if index($c->{q}, $v) < 0;
	}
	$stats{tags} += $ntags;
	return ($res, \%vals);
}

sub check_oracle
{
	my ($c, $res, $vals, $ostr, $cfg) = @_;
	my $where = "call $c->{id} ($c->{desc})";
	my %exp;
	for my $item (split /,/, $ostr)
	{
		my ($g, $hex) = split /:/, $item, 2;
		my $k = $cfg->{keys}[ $g - 1 ];
		next unless defined $k && $cfg->{allowed}{$k};
		$exp{$k} = clip(pack('H*', $hex // ''), $limits{max_tag_value_len}, $cfg->{utf8});
	}
	$stats{oracle}++;

	# DESIGN.md §6.11 step 9 (src/tagset.c): one tag per key, considered in
	# priority order (allowlist position, or sorted keys with tags = '*');
	# a tag is kept if max_tags is not yet reached and key\0value\0 fits the
	# bytes left, otherwise dropped (the fill stops at max_tags).
	my @order = defined $cfg->{prio}
	  ? sort { $cfg->{prio}{$a} <=> $cfg->{prio}{$b} } keys %exp
	  : sort keys %exp;
	my (%keep, $used, $kept);
	($used, $kept) = (0, 0);
	for my $k (@order)
	{
		last if $kept >= $limits{max_tags};
		my $need = length($k) + 1 + length($exp{$k}) + 1;
		next if $need > $limits{max_tagset_bytes} - $used;
		$keep{$k} = 1;
		$used += $need;
		$kept++;
	}
	my $ndrop = scalar(keys %exp) - $kept;
	$stats{oracle_drop}++ if $ndrop > 0;
	for my $k (sort keys %$vals)
	{
		if (!exists $exp{$k})
		{
			problem("$where: oracle: unexpected tag $k");
		}
		elsif (!$keep{$k})
		{
			problem("$where: oracle: tag $k kept, but the limits drop it");
		}
		elsif ($vals->{$k} ne $exp{$k})
		{
			problem("$where: oracle: $k = '", unpack('H*', $vals->{$k}), "' (hex), expected '",
				unpack('H*', $exp{$k}), "'");
		}
	}
	for my $k (sort keys %keep)
	{
		problem("$where: oracle: missing tag $k") unless exists $vals->{$k};
	}
	problem("$where: oracle: dropped_tags $res->{dropped_tags}, expected $ndrop")
	  if $res->{dropped_tags} != $ndrop;
	problem("$where: oracle: tagset_bytes $res->{tagset_bytes}, expected $used")
	  if $res->{tagset_bytes} != $used;
}

# Verdict on a rejected configuration: ($ok, $reason, $detail). Only the
# check hook's own error is classified: its SQLSTATE from the @@REJECTED
# line, its message and DETAIL from stderr after the last @@HOOK marker
# (the pre-check before it may have failed too). The pre-check's timeout is
# only the condition for accepting a timeout from the check hook.
sub rejection_verdict
{
	my ($out, $err, $want, $fuzzy) = @_;
	my ($inv) = $out =~ /^\@\@INVALID\|\s*(.*)$/m;
	$inv //= '';
	my ($state) = $out =~ /^\@\@REJECTED\|\s*(\w*)$/m;
	$state //= '';
	my $pos = rindex $err, "\@\@HOOK\n";
	my $herr = $pos >= 0 ? substr($err, $pos + 7) : '';
	my ($msg) = $herr =~ /ERROR:\s*(.*)/;
	$msg //= '(no error message)';
	my ($detail) = $herr =~ /DETAIL:\s*(.*)/;
	$detail //= '(no detail)';
	my $reason = $pos < 0 ? 'other: no check-hook marker on stderr'
	  : $state eq '57014' && $msg =~ /statement timeout/ ? 'timeout'
	  : $state ne '22023' ? "other: SQLSTATE $state: $msg; $detail"
	  : $detail =~ /longer than \d+ bytes/ ? 'long'
	  : $detail =~ /took longer than \d+ ms/ ? 'slow'
	  : $detail =~ /is invalid:/ ? 'invalid'
	  : $detail =~ /back-references/ ? 'backref'
	  : $detail =~ /more than max_tags/ ? 'max_tags'
	  : $detail =~ /has \d+ keys? but its pattern has (\d+) capture/ ? "nkeys($1)"
	  : "other: $msg; $detail";
	# the compile time limit applies before every other compile verdict
	my $ok = $reason eq $want || ($fuzzy && $want ne 'long' && $reason !~ /^other/)
	  || ($want eq 'resource' && $reason ne 'long' && $reason !~ /^other/)
	  || ($reason eq 'slow' && $want ne 'long');
	# a pattern whose compile outlasts statement_timeout in core regexp_matches
	# may also outlast it in the check hook (compiling is interruptible)
	$ok = 0 if $reason eq 'timeout' && !($want eq 'resource' && $inv =~ /statement timeout/);
	return ($ok, $reason, $detail);
}

# --self-test: check_oracle on synthetic results (no server), including
# results a regression could produce that must be reported.
if ($opt{self_test})
{
	my $hex = sub { join ',', map { "$_->[0]:" . unpack('H*', $_->[1]) } @_ };
	my @cases = (
		# [name, limits, keys, prio, oracle groups, result tags, dropped, bytes, ok]
		['one tag kept', [64, 256, 512], ['k'], undef, [[1, 'a']], { k => 'a' }, 0, 4, 1],
		['one tag discarded and counted as dropped', [64, 256, 512], ['k'], undef,
			[[1, 'a']], {}, 1, 0, 0],
		['oversized tag dropped, smaller later one kept', [64, 256, 20], ['a_1', 'b_2'], undef,
			[[1, 'v' x 100], [2, 'x']], { b_2 => 'x' }, 1, 6, 1],
		['both dropped although the second fits', [64, 256, 20], ['a_1', 'b_2'], undef,
			[[1, 'v' x 100], [2, 'x']], {}, 2, 0, 0],
		['max_tags keeps the first by allowlist order', [1, 256, 512], ['a_1', 'b_2'],
			{ b_2 => 0, a_1 => 1 }, [[1, 'p'], [2, 'q']], { b_2 => 'q' }, 1, 6, 1],
		['max_tags keeps the wrong one', [1, 256, 512], ['a_1', 'b_2'],
			{ b_2 => 0, a_1 => 1 }, [[1, 'p'], [2, 'q']], { a_1 => 'p' }, 1, 6, 0],
		['value truncated to max_tag_value_len', [64, 2, 512], ['k'], undef,
			[[1, 'abc']], { k => 'ab' }, 0, 5, 1],
	);
	my $bad = 0;
	for my $t (@cases)
	{
		my ($name, $lim, $keys, $prio, $groups, $tags, $drop, $bytes, $ok) = @$t;
		@limits{qw(max_tags max_tag_value_len max_tagset_bytes)} = @$lim;
		my $cfg = { utf8 => 1, keys => $keys, allowed => { map { $_ => 1 } @$keys }, prio => $prio };
		my $res = { ntags => scalar keys %$tags, dropped_tags => $drop, tagset_bytes => $bytes };
		@problems = ();
		check_oracle({ id => 1, desc => 'self-test' }, $res, $tags, $hex->(@$groups), $cfg);
		my $got = @problems ? 0 : 1;
		printf "%s: %s%s\n", $got == $ok ? 'ok' : 'FAILED', $name,
		  @problems ? " (reported: $problems[0])" : '';
		$bad++ if $got != $ok;
	}

	# rejection_verdict on synthetic round output: the pre-check's error
	# must not be read as the check hook's.
	my $pre_to = "psql:round.sql:3: ERROR:  canceling statement due to statement timeout\n";
	my $inv_to = "\@\@INVALID| canceling statement due to statement timeout\n";
	my $hook = sub {
		my ($state, $msg, $detail) = @_;
		return ("\@\@HOOK\npsql:round.sql:6: ERROR:  $msg\n" . (defined $detail ? "DETAIL:  $detail\n" : ''),
			"\@\@REJECTED| $state\n");
	};
	my $gucmsg = 'invalid value for parameter "pg_stat_statement_context.extractors": "regex(...)"';
	my @vcases = (
		# [name, pre-check stdout, pre-check stderr, hook [sqlstate, message, detail], want, fuzzy, ok]
		['pre-check and check hook time out', $inv_to, $pre_to,
			['57014', 'canceling statement due to statement timeout'], 'resource', 0, 1],
		['pre-check timeout, unrelated check-hook error', $inv_to, $pre_to,
			['22012', 'division by zero'], 'resource', 0, 0],
		['pre-check timeout, length rejection', $inv_to, $pre_to,
			['22023', $gucmsg, 'Pattern of extractor "regex" is longer than 1024 bytes.'], 'long', 0, 1],
		['pre-check timeout, check hook times out on a long pattern', $inv_to, $pre_to,
			['57014', 'canceling statement due to statement timeout'], 'long', 0, 0],
		['check hook times out, pre-check did not', '', '',
			['57014', 'canceling statement due to statement timeout'], 'accept', 0, 0],
		['invalid pattern rejected as invalid', "\@\@INVALID| invalid regular expression: x\n",
			"psql:round.sql:3: ERROR:  invalid regular expression: x\n",
			['22023', $gucmsg, 'Pattern of extractor "regex" is invalid: x.'], 'invalid', 0, 1],
		['check-hook DETAIL with the wrong SQLSTATE', '', '',
			['XX000', $gucmsg, 'Pattern of extractor "regex" is longer than 1024 bytes.'], 'long', 0, 0],
		['compile time limit, valid pattern', '', '',
			['22023', $gucmsg, 'Compiling the pattern of extractor "regex" took longer than 100 ms.'], 'accept', 0, 1],
		['compile time limit, resource-limited pattern', $inv_to, $pre_to,
			['22023', $gucmsg, 'Compiling the pattern of extractor "regex" took longer than 100 ms.'], 'resource', 0, 1],
		['compile time limit on a pattern over the length limit', '', '',
			['22023', $gucmsg, 'Compiling the pattern of extractor "regex" took longer than 100 ms.'], 'long', 0, 0],
	);
	for my $t (@vcases)
	{
		my ($name, $pout, $perr, $h, $want, $fuzzy, $ok) = @$t;
		my ($herr, $hout) = $hook->(@$h);
		my ($got, $reason) = rejection_verdict($pout . $hout, $perr . $herr, $want, $fuzzy);
		$got = $got ? 1 : 0;
		printf "%s: %s (reason '%s', want '%s')\n", $got == $ok ? 'ok' : 'FAILED', $name, $reason, $want;
		$bad++ if $got != $ok;
	}

	# check_result on a synthetic result: the driver sets no cardinality cap,
	# so capped_tags must be 0.
	@limits{qw(max_tags max_tag_value_len max_tagset_bytes)} = (64, 256, 512);
	my %base = (tags => {}, ntags => 0, tagset_bytes => 0, footer => JSON::PP::false,
		heuristic => JSON::PP::false, oom => JSON::PP::false, stmt_start => 0, stmt_end => 8,
		map { $_ => 0 } qw(invalid_tags dropped_tags heuristic_scans regex_compile_failures
		  normalized_tags normalize_failures capped_tags));
	my @rcases = (
		# [name, overrides, ok]
		['plain result accepted', {}, 1],
		['capped_tags without a cardinality cap', { capped_tags => 1 }, 0],
		['negative capped_tags', { capped_tags => -1 }, 0],
	);
	for my $t (@rcases)
	{
		my ($name, $over, $ok) = @$t;
		@problems = ();
		check_result({ id => 1, desc => 'self-test', q => 'SELECT 1' },
			$JSON->encode({ %base, %$over }), { window => 1024, utf8 => 1 });
		my $got = @problems ? 0 : 1;
		printf "%s: %s%s\n", $got == $ok ? 'ok' : 'FAILED', $name,
		  @problems ? " (reported: $problems[0])" : '';
		$bad++ if $got != $ok;
	}

	# @RESULT_KEYS must match the keys src/extract_fn.c pushes into the
	# result object, so a new key there fails here rather than in every call.
	my $src = File::Spec->catfile(dirname(File::Spec->rel2abs(__FILE__)), '..', '..', 'src', 'extract_fn.c');
	if (open my $fh, '<', $src)
	{
		my $text = do { local $/; <$fh> };
		my @pushed = sort($text =~ /\bpush_(?:int|bool|key)\s*\(\s*&st\s*,\s*"(\w+)"/g);
		my $ok = "@pushed" eq "@RESULT_KEYS";
		printf "%s: result keys match src/extract_fn.c%s\n", $ok ? 'ok' : 'FAILED',
		  $ok ? '' : " (pushed: @pushed; expected: @RESULT_KEYS)";
		$bad++ unless $ok;
	}
	else
	{
		print "FAILED: cannot read $src: $!\n";
		$bad++;
	}
	exit($bad ? 1 : 0);
}

# ---------------- rounds ----------------

my $master = Rng->new($opt{seed});
my $t0 = time();
make_path($opt{work});
my $created_dbs = 0;

sub new_epoch_limits
{
	%limits = (
		max_tags => $master->pick(1, 2, 3, 4, 8, 16, 64),
		max_tag_value_len => $master->pick(1, 2, 3, 5, 8, 32, 64, 256, 4096),
		# small budgets twice as often, so long keys/values force greedy-fill drops
		max_tagset_bytes => $master->pick(128, 128, 160, 160, 256, 512, 2048, 8192),
	);
	print "epoch ", $epoch + 1, ": ", join(', ', map { "$_=$limits{$_}" } sort keys %limits), "\n";
}

sub setup_dbs
{
	my $f = "$opt{work}/setup.sql";
	spit($f, <<'EOS');
\set ON_ERROR_STOP 1
CREATE DATABASE fuzz_utf8 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
CREATE DATABASE fuzz_ascii ENCODING 'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c fuzz_utf8
CREATE EXTENSION pg_stat_statement_context;
\c fuzz_ascii
CREATE EXTENSION pg_stat_statement_context;
EOS
	my $rc = psql_file('postgres', 'UTF8', $f, "$opt{work}/setup.out", "$opt{work}/setup.err");
	fail_run("database setup failed: " . slurp("$opt{work}/setup.err"), undef) if $rc != 0;
}

sub gen_key
{
	my ($r, $i) = @_;
	return "k${i}_" . join('', map { $r->pick('a' .. 'z', '0' .. '9') } 1 .. 59) if $r->chance(20);
	return $r->pick('k', 'key', 'svc', 'op', 'route', 'x') . "_$i";
}

# As src/extract_fn.c validates its arguments.
sub args_ok
{
	my ($loc, $len, $n) = @_;
	return 0 if $loc < -1 || ($loc >= 0 && $loc > $n);
	return 0 if $len < 0 || ($loc >= 0 && $len > $n - $loc);
	return 1;
}

sub gen_call
{
	my ($r, $cfg, $sample, $id) = @_;
	my $utf8 = $cfg->{utf8};
	my ($q, $form, $body);
	my ($loc, $len) = (-1, 0);
	if ($r->chance(60))
	{
		$body = block_safe(gen_body($r, $utf8, $sample));
		$form = $r->pick('tail', 'head', 'line');
		if ($form eq 'line')
		{
			$body =~ tr/\n\r/  /;
			$q = 'SELECT 1 --' . $body;
		}
		elsif ($form eq 'tail') { $q = 'SELECT 1 /*' . $body . '*/' . $r->pick('', ';', ' ;  '); }
		else { $q = '/*' . $body . '*/ SELECT 1'; }
	}
	else
	{
		$form = 'noise';
		$q = gen_noise_query($r, $utf8, $sample);
	}
	my $n = length $q;
	my $a = $r->int(100);
	if ($a < 15)
	{
		$loc = $r->range(0, $n);
		$len = $r->chance(50) ? 0 : $r->range(0, $n - $loc);
		$form .= ' range';
	}
	elsif ($a < 18)
	{
		($loc, $len) = @{ $r->pick([$n + 1, 0], [-2, 0], [0, $n + 1], [$n, 1], [-1, -1], [-1, $n + 5], [$n, 0]) };
		$form .= ' edge-args';
	}
	# Oracle only where src/scan.h promises the comment to the extractor: the
	# statement fits scan_window (ANY is budgeted to scan_window comment
	# bytes too), and APPEND runs hold block comments only.
	my $p = $cfg->{position};
	my $oracle = $cfg->{regex_only} && $form !~ /noise|args|range/ && $n <= $cfg->{window}
	  && ($p eq 'any' || ($p eq 'prepend' && $form eq 'head') || ($p eq 'append' && $form eq 'tail'));
	return { id => $id, q => $q, loc => $loc, len => $len, body => $body,
		args_ok => args_ok($loc, $len, $n), oracle => $oracle, desc => "$form, $n bytes, args $loc/$len" };
}

sub run_round
{
	my ($rseed) = @_;
	my $r = Rng->new($rseed);
	my $utf8 = $r->chance(60);
	my ($db, $enc) = $utf8 ? ('fuzz_utf8', 'UTF8') : ('fuzz_ascii', 'SQL_ASCII');
	my ($pat, $st, $sample) = gen_pattern($r, $utf8);
	my $patb = enc($pat);
	my $nkeys = $st->{ngroups};
	$nkeys += $r->pick(-1, 1) if $r->chance(8);
	$nkeys = 1 if $nkeys < 1;
	my @keys = map { gen_key($r, $_) } 1 .. $nkeys;
	my $position = $r->pick('any', 'any', 'append', 'prepend');
	(my $dslpat = $patb) =~ s/'/''/g;
	my $regex = "regex(pattern='$dslpat', keys=" . join('|', @keys)
	  . ($position eq 'any' && $r->chance(70) ? '' : ", position=$position");
	my $chain = $r->int(100);
	my $ext;
	if ($chain < 80) { $ext = "$regex)"; }
	elsif ($chain < 90) { $ext = "sqlcommenter, $regex, merge=on)"; }
	else { $ext = "$regex), marginalia(position=any)"; }
	my $regex_only = $chain < 80;

	my %keyset = map { $_ => 1 } @keys;
	my ($tags, $excl) = ('*', '');
	my %allowed = %keyset;
	my $prio;	# allowlist position per key; undef: sorted keys (tags = '*')
	if ($r->chance(25))
	{
		my @sub = grep { $r->chance(50) } @keys;
		@sub = $r->shuffle(@sub);
		push @sub, 'other';
		$tags = join ', ', @sub;
		%allowed = map { $_ => 1 } grep { $keyset{$_} } @sub;
		$prio = { map { $sub[$_] => $_ } 0 .. $#sub };
	}
	elsif ($r->chance(25))
	{
		$excl = $r->pick(@keys);
		delete $allowed{$excl};
	}
	my $window = $r->pick(64, 100, 256, 1024, 2048, 65536);
	my $cfg = { utf8 => $utf8, keys => \@keys, keyset => \%keyset, allowed => \%allowed,
		regex_only => $regex_only, window => $window, position => $position, prio => $prio };

	my $dir = "$opt{work}/round";
	make_path($dir);
	my %files = map { $_ => "$dir/$_" } qw(round.sql wait.sql out.txt err.txt);
	my $extlit = sql_std_lit($ext);
	my $sql = "SET statement_timeout = '${STMT_TIMEOUT_S}s';\n"
	  . "-- seed $opt{seed} round $cur_round (round seed $rseed), $db\n"
	  . "SELECT '\@\@VALID|' || count(*) FROM regexp_matches('' COLLATE \"C\", " . sql_std_lit($patb) . ");\n"
	  . "\\if :ERROR\n\\echo '\@\@INVALID|' :'LAST_ERROR_MESSAGE'\n\\endif\n"
	  . "\\warn '\@\@HOOK'\n"
	  . "ALTER SYSTEM SET pg_stat_statement_context.extractors = $extlit;\n"
	  . "\\if :ERROR\n\\echo '\@\@REJECTED|' :LAST_ERROR_SQLSTATE\n\\quit\n\\endif\n"
	  . "ALTER SYSTEM SET pg_stat_statement_context.tags = " . sql_std_lit($tags) . ";\n"
	  . "ALTER SYSTEM SET pg_stat_statement_context.exclude_tags = " . sql_std_lit($excl) . ";\n"
	  . "ALTER SYSTEM SET pg_stat_statement_context.scan_window = $window;\n"
	  . "ALTER SYSTEM SET pg_stat_statement_context.normalize = '';\n"
	  . "SELECT pg_reload_conf() AS reloaded \\gset\n\\set polls 0\n\\i $files{'wait.sql'}\n"
	  . "\\if :cfg_ok\n\\echo '\@\@CONFIG_OK'\n\\else\n\\echo '\@\@CONFIG_TIMEOUT'\n\\quit\n\\endif\n";
	spit($files{'wait.sql'},
		"SELECT current_setting('pg_stat_statement_context.extractors') = $extlit\n"
		  . "   AND current_setting('pg_stat_statement_context.tags') = " . sql_std_lit($tags) . "\n"
		  . "   AND current_setting('pg_stat_statement_context.exclude_tags') = " . sql_std_lit($excl) . "\n"
		  . "   AND (SELECT setting FROM pg_settings WHERE name = 'pg_stat_statement_context.scan_window') = '$window'\n"
		  . "   AND current_setting('pg_stat_statement_context.normalize') = ''\n"
		  . "   AS cfg_ok, :polls + 1 AS polls, :polls >= 500 AS cfg_timeout \\gset\n"
		  . "\\if :cfg_ok\n\\elif :cfg_timeout\n\\else\nSELECT pg_sleep(0.01) \\gset\n\\i $files{'wait.sql'}\n\\endif\n");

	my @calls;
	for my $i (1 .. $opt{calls})
	{
		my $c;
		if (@calls && $r->chance(8))
		{
			my $o = $calls[ $r->int(scalar @calls) ];
			$c = { %$o, id => $i, repeat_of => $o->{id}, oracle => 0 };
		}
		else
		{
			$c = gen_call($r, $cfg, $sample, $i);
		}
		push @calls, $c;
		$sql .= "SELECT '\@\@R|$i|' || pg_stat_statement_context_extract(" . sql_lit_bytes($c->{q})
		  . ", $c->{loc}, $c->{len})::text;\n"
		  . "\\if :ERROR\n\\echo '\@\@E|$i|' :'LAST_ERROR_MESSAGE'\n\\endif\n";
		if ($c->{oracle})
		{
			$sql .= "SELECT '\@\@O|$i|' || coalesce((SELECT string_agg(g::text || ':' || "
			  . "encode(convert_to(v, getdatabaseencoding()), 'hex'), ',' ORDER BY g) FROM "
			  . "(SELECT DISTINCT ON (g) g, m[g] AS v FROM regexp_matches(" . sql_lit_bytes($c->{body})
			  . " COLLATE \"C\", " . sql_std_lit($patb) . ", 'g') WITH ORDINALITY AS t(m, o), "
			  . "generate_subscripts(m, 1) AS g WHERE m[g] IS NOT NULL ORDER BY g, o) s), '');\n"
			  . "\\if :ERROR\n\\echo '\@\@OE|$i|' :'LAST_ERROR_MESSAGE'\n\\endif\n";
		}
	}
	$sql .= "\\echo '\@\@DONE'\n";
	spit($files{'round.sql'}, $sql);

	my $rc = psql_file($db, $enc, $files{'round.sql'}, $files{'out.txt'}, $files{'err.txt'});
	my $out = slurp($files{'out.txt'});
	my $err = slurp($files{'err.txt'});
	my $log = new_log();
	fail_run("server log: $&", \%files) if $log =~ /^.*$CRASH_RE.*$/m;
	fail_run("psql timed out after ${ROUND_TIMEOUT_S}s (hang?)", \%files) if $rc == 124;
	fail_run("connection lost (psql exit $rc): $err", \%files) if $rc != 0 || $err =~ $CRASH_RE;
	$stats{rounds}++;

	# check_hook verdict, in src/guc.c's order: length, compile (the server's
	# regexp_matches is the authority on syntax), back-reference, groups >
	# max_tags, keys != groups. For a "fuzzy" pattern only the length
	# verdict is known; any other documented verdict is accepted.
	# A resource error (huge NFA/DFA from nested bounded repetition) in the
	# pre-check is not a syntax verdict: the check_hook may accept the
	# pattern (matching errors are then swallowed) or reject it as invalid.
	my ($inv) = $out =~ /^\@\@INVALID\|\s*(.*)$/m;
	my $valid = !defined $inv;
	my $resource = defined $inv && $inv =~ /$RESOURCE_RE/;
	$stats{resource}++ if $resource;
	my $want = length $patb > 1024 ? 'long'
	  : $resource ? 'resource'
	  : !$valid ? 'invalid'
	  : $st->{backref} ? 'backref'
	  : $st->{ngroups} > $limits{max_tags} ? 'max_tags'
	  : $nkeys != $st->{ngroups} ? "nkeys($st->{ngroups})"
	  : 'accept';
	if ($out =~ /^\@\@REJECTED\|/m)
	{
		my ($ok, $reason, $detail) = rejection_verdict($out, $err, $want, $st->{fuzzy});
		unless ($ok)
		{
			fail_run("configuration rejected as '$reason' but predicted '$want'\n"
				  . "  pattern: $pat\n  groups (generator): $st->{ngroups}, keys: $nkeys\n  $detail", \%files);
		}
		$reject_reasons{ $reason =~ s/\(\d+\)//r }++;
		$stats{rejected}++;
		return;
	}
	if ($want ne 'accept' && $want ne 'resource' && !($st->{fuzzy} && $want ne 'long' && $valid))
	{
		fail_run("configuration accepted but predicted '$want'\n  pattern: $pat\n"
			  . "  groups (generator): $st->{ngroups}, keys: $nkeys", \%files);
	}
	fail_run("configuration reload timed out", \%files) if $out =~ /^\@\@CONFIG_TIMEOUT$/m;
	fail_run("round did not finish", \%files) unless $out =~ /^\@\@DONE$/m;

	my (%res, %oracle, %errs, %oerrs);
	for my $line (split /\n/, $out)
	{
		if ($line =~ /^\@\@R\|(\d+)\|(.*)$/) { $res{$1} = $2; }
		elsif ($line =~ /^\@\@O\|(\d+)\|(.*)$/) { $oracle{$1} = $2; }
		elsif ($line =~ /^\@\@E\|(\d+)\| (.*)$/) { $errs{$1} = $2; }
		elsif ($line =~ /^\@\@OE\|(\d+)\| (.*)$/) { $oerrs{$1} = $2; }
	}
	my %parsed;
	for my $c (@calls)
	{
		$stats{calls}++;
		my $i = $c->{id};
		if (exists $errs{$i})
		{
			my $e = $errs{$i};
			if (!$c->{args_ok} && $e =~ /stmt_(location|len) -?\d+ is out of range/) { $stats{arg_errors}++; }
			elsif ($e =~ /canceling statement due to statement timeout/) { $stats{timeouts}++; }
			else { problem("call $i ($c->{desc}): unexpected error: $e"); }
			next;
		}
		problem("call $i ($c->{desc}): invalid arguments accepted") unless $c->{args_ok};
		unless (exists $res{$i})
		{
			problem("call $i: no result");
			next;
		}
		my ($rj, $vals) = check_result($c, $res{$i}, $cfg);
		next unless $rj;
		$parsed{$i} = $res{$i};
		if (defined $c->{repeat_of} && defined $parsed{ $c->{repeat_of} })
		{
			$stats{repeat}++;
			problem("call $i: repeat of call $c->{repeat_of} differs:\n  $res{$i}\n  $parsed{$c->{repeat_of}}")
			  if $res{$i} ne $parsed{ $c->{repeat_of} };
		}
		if ($c->{oracle})
		{
			if (exists $oerrs{$i}) { $stats{oracle_err}++; }
			elsif (exists $oracle{$i}) { check_oracle($c, $rj, $vals, $oracle{$i}, $cfg); }
			else { problem("call $i: no oracle result"); }
		}
	}
	if (@problems)
	{
		my $n = scalar @problems;
		splice @problems, 20 if @problems > 20;
		fail_run("$n problem(s):\n  " . join("\n  ", @problems) . "\n  pattern: $pat\n  extractors: $ext\n"
			  . "  tags: $tags; exclude_tags: $excl; scan_window: $window", \%files);
	}
}

$SIG{INT} = $SIG{TERM} = sub { print "interrupted\n"; server_stop(); exit 130; };

my $round = 0;
while (1)
{
	last if $opt{rounds} && $round >= $opt{rounds};
	last if !$opt{rounds} && time() - $t0 >= $opt{duration};
	if ($round % $opt{rounds_per_epoch} == 0)
	{
		server_stop() if $epoch >= 0;
		new_epoch_limits();
		server_start();
		setup_dbs() unless $created_dbs++;
	}
	$cur_round = $round;
	run_round($master->next);
	$round++;
	if ($round % 50 == 0)
	{
		printf "  %d rounds, %d calls, %d oracle checks, %.0fs\n",
		  $stats{rounds}, $stats{calls}, $stats{oracle}, time() - $t0;
	}
}
server_stop();
my $log_all = '';
$log_all .= slurp($_) for glob "$opt{work}/epoch-*.log";
if ($log_all =~ /^.*$CRASH_RE.*$/m)
{
	print "FAIL: server log: $&\n";
	exit 1;
}
printf "regex_fuzz: PASS, seed %d, %d rounds (%d epochs), %d extract calls, %d tags, "
  . "%d oracle comparisons (%d with limit drops, %d oracle errors), %d repeats, %d expected rejections (%s), "
  . "%d resource-limited patterns, %d expected argument errors, %d statement timeouts, %.0fs\n",
  $opt{seed}, $stats{rounds}, $epoch + 1, $stats{calls}, $stats{tags}, $stats{oracle},
  $stats{oracle_drop}, $stats{oracle_err}, $stats{repeat}, $stats{rejected},
  join(', ', map { "$_ $reject_reasons{$_}" } sort keys %reject_reasons) || 'none',
  $stats{resource}, $stats{arg_errors}, $stats{timeouts}, time() - $t0;
exit 0;
