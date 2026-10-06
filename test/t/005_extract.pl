# Tag-set pipeline in the backend (DESIGN.md §3.1 item 2, §4.2, §6.5, §6.11;
# backlog 20261005-091225-9): src/extract.c runs src/tagset.c with the live
# GUCs, the database encoding (pg_verify_mbstr, pg_mbcliplen) and the
# tag-set hash. Driven through the TEST-ONLY module
# test/modules/pssc_extract_test (make install-test-modules), which also
# checks every result's invariants (layout, strict key order, value and set
# bounds, encoding validity, hash, no buffer overrun) and errors out if one
# breaks. The pure pipeline is covered in depth by test/unit/test_tagset.c.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('extract');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
$node->start;

for my $db ([ 'u8', 'UTF8' ], [ 'l1', 'LATIN1' ], [ 'sa', 'SQL_ASCII' ])
{
	$node->safe_psql('postgres',
		"CREATE DATABASE $db->[0] TEMPLATE template0 ENCODING '$db->[1]' "
		  . "LC_COLLATE 'C' LC_CTYPE 'C'");
	$node->safe_psql($db->[0], 'CREATE EXTENSION pssc_extract_test');
}

# SQL string literal (standard_conforming_strings: only quotes are doubled).
sub sqlq { my ($s) = @_; $s =~ s/'/''/g; return "'$s'"; }

# Raw bytes as a bytea literal.
sub bq { return "'\\x" . unpack('H*', $_[0]) . "'::bytea"; }

# ALTER SYSTEM statements, then reload and wait until new backends see it.
my $sentinel = 1000;
sub alter_and_reload
{
	$sentinel++;
	$node->safe_psql('postgres',
		join('', map { "ALTER SYSTEM $_;\n" } @_)
		  . "ALTER SYSTEM SET $P.scan_window = $sentinel;");
	$node->reload;
	my $tries = 0;
	until ($node->safe_psql('postgres',
			"SELECT setting FROM pg_settings WHERE name = '$P.scan_window'") eq $sentinel)
	{
		die "reload not processed" if ++$tries > 300;
		usleep(100_000);
	}
}
sub config
{
	my (%c) = @_;
	alter_and_reload(map {
		defined $c{$_} ? "SET $P.$_ = " . sqlq($c{$_}) : "RESET $P.$_"
	} qw(extractors tags exclude_tags));
}

# Run the pipeline. $q is a SQL expression (text or bytea); extra args are
# appended (stmt_location, stmt_len, bufsize). $pre is SQL run first in the
# same backend. Returns a hash of the result columns.
my @COLS = qw(tags serialized nbytes hash ntags footer oom invalid dropped heuristic regex_fail);
sub ex
{
	my ($q, %o) = @_;
	my $db = $o{db} // 'u8';
	my $args = join('', map { ", $_" } @{ $o{args} // [] });
	my $pre = $o{pre} // '';
	my $out = $node->safe_psql($db,
		"$pre SELECT concat_ws(E'\\t', array_to_string(tags, ','), "
		  . "encode(serialized, 'hex'), nbytes, hash, ntags, footer, oom, "
		  . "invalid_tags, dropped_tags, heuristic_scans, regex_compile_failures) "
		  . "FROM pssc_extract_test($q$args)");
	my @l = split /\n/, $out;
	my @v = split /\t/, $l[-1], -1;
	die "unexpected output <$out>" unless @v == @COLS;
	my %r;
	@r{@COLS} = @v;
	return \%r;
}
sub tags { return ex(@_)->{tags}; }
sub tq { return tags(sqlq($_[0]), @_[ 1 .. $#_ ]); }

# --- defaults: sqlcommenter, marginalia; tags = action, controller, job ---

is(tq(q{SELECT 1 /*controller='users',action='show',other='x'*/}),
	'action=show,controller=users', 'default config: sqlcommenter, allowlist');
is(tq(q{SELECT 1 /*controller:users,job:j1*/}),
	'controller=users,job=j1', 'default config: marginalia');
is(tq(q{SELECT 1}), '', 'no comment: empty tag set');
{
	my $r = ex(sqlq(q{SELECT 1 /*action='a'*/}));
	is($r->{serialized}, unpack('H*', "action\0a\0"), 'serialized as key NUL value NUL');
	my $h = $node->safe_psql('u8',
		"SELECT pssc_extract_test_hash('\\x$r->{serialized}')");
	is($r->{hash}, $h, 'hash is pssc_tagset_hash of the serialized set');
	isnt(ex(sqlq(q{SELECT 1 /*action='b'*/}))->{hash}, $r->{hash},
		'different tag sets hash differently');
	my $e = ex(sqlq('SELECT 1'));
	is($e->{hash},
		$node->safe_psql('u8', "SELECT pssc_extract_test_hash(''::bytea)"),
		'empty tag set: hash of no bytes');
}

# --- order independence: byte-identical serialization and hash ---

config(tags => '*', exclude_tags => '',
	extractors => 'sqlcommenter(position=any), marginalia(position=any, merge=on)');
{
	my @p = ([ 'b', 'two' ], [ 'a', 'one' ], [ 'c', 'x y' ], [ 'ab', '3' ], [ 'd', 'é' ]);
	my %seen;
	my $n = 0;
	my $ref = ex(sqlq(q{SELECT 1 /*a='one',ab='3',b='two',c='x%20y',d='%C3%A9'*/}));
	is($ref->{tags}, 'a=one,ab=3,b=two,c=x y,d=é', 'reference tag set');
	my @perms = ([ 0 .. 4 ]);
	# every rotation of every adjacent swap: enough distinct orders
	for my $i (0 .. 3)
	{
		for my $rot (0 .. 4)
		{
			my @o = (0 .. 4);
			@o[ $i, $i + 1 ] = @o[ $i + 1, $i ];
			push @perms, [ @o[ $rot .. 4 ], @o[ 0 .. $rot - 1 ] ];
		}
	}
	for my $o (@perms)
	{
		my @q = map { $p[$_] } @$o;
		my $enc = sub { my $v = $_[0]; $v =~ s/ /+/g; $v };
		my @variants = (
			# one sqlcommenter comment
			'SELECT 1 /*' . join(',', map { "$_->[0]='" . $enc->($_->[1]) . "'" } @q) . '*/',
			# one comment per pair, mixed formats and spacing
			'SELECT 1 '
			  . join(' ', map {
				  $_->[1] =~ / / || $_->[1] =~ /é/
					? "/* $_->[0] = '" . $enc->($_->[1]) . "' */"
					: "/*$_->[0]:$_->[1]*/"
			  } @q),
			# prepend
			join('', map { "/*$_->[0]='" . $enc->($_->[1]) . "'*/\n" } @q) . 'SELECT 1');
		for my $v (@variants)
		{
			my $r = ex(sqlq($v));
			$n++;
			$seen{"$r->{serialized}/$r->{hash}"}++;
		}
	}
	is(scalar(keys %seen), 1, "$n orderings give one serialization and hash");
	is((keys %seen)[0], "$ref->{serialized}/$ref->{hash}", 'and it is the reference');
}

# --- keys (original names), rename, allowlist priority, denylist ---

config(tags => '*', exclude_tags => 'secret',
	extractors => 'sqlcommenter(keys=route|app|secret, rename=route:endpoint)');
is(tq(q{SELECT 1 /*route='r',endpoint='e',app='z',other='w',secret='s'*/}),
	'app=z,endpoint=r',
	'keys match original names before rename; exclude_tags after rename');
config(tags => 'endpoint, app', extractors => 'sqlcommenter(rename=route:endpoint|app:application)');
is(tq(q{SELECT 1 /*route='r',app='z',application='y'*/}), 'endpoint=r',
	'the global allowlist matches renamed keys');

# rename targets are config bytes: verified against each database's encoding.
# Set from the LATIN1 database, so the target is the single byte 0xE9.
$sentinel++;
$node->safe_psql('l1', "SET client_encoding = 'LATIN1'; "
	  . "ALTER SYSTEM SET $P.extractors = 'sqlcommenter(rename=a:\xe9)'; "
	  . "ALTER SYSTEM SET $P.tags = '*'; ALTER SYSTEM SET $P.exclude_tags = ''; "
	  . "ALTER SYSTEM SET $P.scan_window = $sentinel");
$node->reload;
until ($node->safe_psql('postgres',
		"SELECT setting FROM pg_settings WHERE name = '$P.scan_window'") eq $sentinel)
{
	usleep(100_000);
}
{
	my $r = ex(sqlq(q{SELECT 1 /*a='1',b='2'*/}));
	is("$r->{tags} $r->{invalid}", 'b=2 1', 'UTF8: rename target invalid in this encoding is rejected');
	$r = ex(bq(q{SELECT 1 /*a='1',b='2'*/}), db => 'l1');
	is($r->{serialized}, unpack('H*', "b\0" . "2\0\xe9\0" . "1\0"), 'LATIN1: the same target is valid');
}

# --- malformed segments: counted only next to a well-formed pair ---

config(tags => '*', exclude_tags => '', extractors => 'sqlcommenter(position=any), marginalia(position=any, merge=on)');
{
	my $r = ex(sqlq(q{SELECT 1 /*a='1',b=2*/ /*c:3*/}));
	is("$r->{tags} $r->{invalid}", 'a=1,c=3 1', 'malformed segment counted; cross-format probing is not');
}

# --- extractor chain: first wins, merge, produces = after filtering ---

config(extractors => 'sqlcommenter(position=any), marginalia(position=any)');
is(tq(q{SELECT 1 /*action='sc'*/ /*action:mg,job:j*/}), 'action=sc',
	'first extractor that produces tags wins; later ones are skipped');
is(tq(q{SELECT 1 /*other='sc'*/ /*action:mg,job:j*/}), 'action=mg,job=j',
	'an extractor whose pairs are all filtered out produces nothing');
config(extractors => 'sqlcommenter(position=any), marginalia(position=any, merge=on)');
is(tq(q{SELECT 1 /*action='sc'*/ /*action:mg,job:j*/}), 'action=sc,job=j',
	'merge=on adds tags; the earlier extractor wins a duplicate key');
config(extractors => 'marginalia(position=any, kv_sep=\'=>\'), sqlcommenter(position=any), '
	  . 'marginalia(position=any), marginalia(position=any, kv_sep=\'=\', merge=on)');
is(tq(q{SELECT 1 /*action='sc'*/ /*controller:c*/ /*job=j*/}), 'action=sc,job=j',
	'a merge=on extractor still runs after a skipped non-merge one');
config(extractors => 'sqlcommenter(position=any)', tags => '*', exclude_tags => '');
is(tq(q{SELECT 1 /*a='1',a='2'*/ /*a='3',b='4'*/}), 'a=1,b=4',
	'within an extractor the first occurrence of a key wins');

# --- regex hook ---

config(extractors => q{regex(pattern='(\w+) (\w+)', keys=k1|k2), sqlcommenter(position=any, merge=on)},
	tags => '*');
is(tq(q{SELECT 1 /*w1 w2*/ /*k1='s'*/}, pre => 'SELECT pssc_extract_test_no_regex();'),
	'k1=s', 'without a regex runtime regex extractors produce nothing');
is(tq(q{SELECT 1 /*w1+w2*/ /*k1='s'*/},
		pre => 'SELECT pssc_extract_test_fake_regex(true);'),
	'k1=w1+w2', 'the installed regex hook supplies pairs; it wins the chain');
is(tq(q{SELECT 1 /*w1+w2*/ /*k1='s'*/},
		pre => 'SELECT pssc_extract_test_fake_regex(true); SELECT pssc_extract_test_no_regex();'),
	'k1=s', 'and can be removed');
is(tq(q{SELECT 1 /*w1 w2*/ /*k1='s'*/}), 'k1=w1,k2=w2',
	'the real regex runtime is installed by default (test/t/006_regex.pl)');

# The backend caches the appname extractors' result; replacing the regex
# hook (another function or argument) must not reuse the old hook's result.
config(extractors => q{appname(format=regex, pattern='^(.*)$', keys=app)}, tags => '*');
{
	my $t = q{SELECT array_to_string(tags, ',') FROM pssc_extract_test('SELECT 1');};
	my $out = $node->safe_psql('u8', qq{
		SET application_name = 'billing 1.2';
		$t
		SELECT pssc_extract_test_fake_regex(true);
		$t
		SELECT pssc_extract_test_fake_regex(false);
		$t
		SELECT pssc_extract_test_no_regex();
		$t
		SELECT pssc_extract_test_fake_regex(false);
		$t});
	is(join('|', grep { $_ ne '' } split /\n/, $out),
		'app=billing 1.2|app=billing|app=billing 1.2|app=billing 1.2',
		'appname cache: each change of the regex hook is seen by the next statement');
	# the empty result (no hook) is filtered out above; check it on its own
	$out = $node->safe_psql('u8', qq{
		SET application_name = 'billing 1.2';
		$t
		SELECT pssc_extract_test_no_regex();
		SELECT count(*) FROM pssc_extract_test('SELECT 1') WHERE cardinality(tags) = 0;});
	is((split /\n/, $out)[-1], '1', 'appname cache: removing the regex hook is seen');
}

# --- footer fallback and statement ownership (§6.5) ---

config(tags => '*', exclude_tags => '');
{
	my $q = sqlq(q{SELECT 1; /*action='foot'*/});
	my $r = ex($q, args => [ 0, 8 ]);
	is("$r->{tags} $r->{footer}", 'action=foot t', 'trailing footer of the last statement');
	my $s = q{SELECT 1 /*action='own'*/; /*action='foot'*/};
	$r = ex(sqlq($s), args => [ 0, index($s, ';') ]);
	is("$r->{tags} $r->{footer}", 'action=own f', 'own comment beats the footer');
	$s = q{SELECT 1; SELECT 2 /*action='b'*/;};
	$r = ex(sqlq($s), args => [ 0, 8 ]);
	is("$r->{tags} $r->{footer}", ' f', 'no footer for a statement that is not last');
	$r = ex(sqlq($s), args => [ 9, rindex($s, ';') - 9 ]);
	is("$r->{tags} $r->{footer}", 'action=b f', 'second statement keeps its own comment');
}
config(tags => '*', exclude_tags => '', extractors => 'sqlcommenter(position=prepend)');
{
	my $s = q{SELECT 1; /*action='lead'*/ SELECT 2};
	my $loc = index($s, 'SELECT 2');
	# PG18 reports the first token; PG14-17 the start after ';'. Both own it.
	is(tq($s, args => [ $loc, 0 ]), 'action=lead', 'leading comment owned (PG18-style location)');
	is(tq($s, args => [ 9, 0 ]), 'action=lead', 'leading comment owned (PG14-17 location)');
	is(tq($s, args => [ 0, 8 ]), '', 'and not taken by the previous statement');
}

# --- encoding: invalid bytes, NUL, truncation on character boundaries ---

config(tags => '*', exclude_tags => '', extractors => 'sqlcommenter(position=any), marginalia(position=any, merge=on)');
{
	my $r = ex(bq("SELECT 1 /*a='ok',b='bad\xff',c\xfe='x',d='\xc3\xa9'*/"));
	is($r->{tags}, 'a=ok,d=é', 'UTF8: invalid key or value is rejected');
	is($r->{invalid}, 2, 'and counted in invalid_tags');
	$r = ex(bq("SELECT 1 /*a='ok',b='%FF',c='x%00y'*/"));
	is("$r->{tags} $r->{invalid}", 'a=ok 2', 'decoded invalid byte and %00 are rejected');
	$r = ex(bq("SELECT 1 /*a:ok,b:x\0y*/"));
	is("$r->{tags} $r->{invalid}", 'a=ok 1', 'raw NUL in a value is rejected');
	$r = ex(bq("SELECT 1 /*a='ok',b='bad\xff'*/"), db => 'l1');
	is($r->{serialized}, unpack('H*', "a\0ok\0b\0bad\xff\0"), 'LATIN1: high bytes are valid');
	$r = ex(bq("SELECT 1 /*a='ok',b='bad\xff'*/"), db => 'sa');
	is($r->{ntags}, 2, 'SQL_ASCII: high bytes are kept');
	my $k63 = 'k' x 63;
	$r = ex(sqlq("SELECT 1 /*${k63}='a',${k63}x='b'*/"));
	is("$r->{tags} $r->{invalid}", "$k63=a 1", 'keys over 63 bytes are rejected');
}

# --- hostile input never errors, results always within bounds ---

config(tags => '*', exclude_tags => '',
	extractors => q{sqlcommenter(position=any), marginalia(position=any, merge=on), }
	  . q{sqlcommenter(position=prepend, url_decode=off, merge=on), }
	  . q{marginalia(position=append, kv_sep='=', pair_sep=' ', merge=on), }
	  . q{regex(pattern='(\w+)', keys=rk, merge=on)});
{
	my @frag = ('/*', '*/', '--', "\n", "'", '"', '=', ',', ':', ' ', ';', '%', '%00',
		'%C3', '%a9', '+', '\\', "\0", "\xff", "\xc3\xa9", "\xe2\x82", 'action',
		'k' x 70, 'v' x 300, 'SELECT 1', '$$', '$x$', 'E\'', 'a', 'b');
	my $arr = 'ARRAY[' . join(',', map { bq($_) } @frag) . ']';
	my $nfrag = scalar @frag;
	for my $db (qw(u8 l1 sa))
	{
		my $out = $node->safe_psql($db, qq{
			SELECT pssc_extract_test_fake_regex(true);
			SELECT setseed(0.42);
			SELECT count(*), max(r.nbytes) <= 512, sum(r.invalid_tags) > 0,
			       max(r.ntags) > 1, bool_or(r.oom)
			FROM generate_series(1, 3000) g,
			     LATERAL (SELECT string_agg(($arr)[1 + floor(random() * $nfrag)::int], ''::bytea) AS q
			              FROM generate_series(1, 1 + (g % 200)) i
			              WHERE g > 0) s,
			     LATERAL pssc_extract_test(s.q) r});
		my @l = split /\n/, $out;
		is($l[-1], '3000|t|t|t|f', "$db: 3000 random inputs: no error, bounded, counted");
	}
	# huge comment counts and sizes
	my $out = $node->safe_psql('u8', q{
		SELECT ntags, nbytes <= 512, footer, oom, dropped_tags > 0
		FROM pssc_extract_test('SELECT 1 ' || (SELECT string_agg(format('/*k%s=''%s''*/', i, repeat('v', i % 100)), ' ')
		                                      FROM generate_series(1, 100000) i))});
	is($out, '8|t|f|f|t', '100000 comments: bounded tag set');
	$out = $node->safe_psql('u8', q{
		SELECT ntags, oom FROM pssc_extract_test('SELECT 1 /*' || repeat('a=''b'',', 200000) || '*/')});
	is($out, '0|f', '200000 pairs in one comment (beyond scan_window: no tags)');
	$out = $node->safe_psql('u8', q{
		SELECT ntags, oom FROM pssc_extract_test('/*' || repeat('a=''b'',', 150) || '*/ SELECT 1 /*' || repeat('a=''b'',', 200000) || '*/')});
	# a (sqlcommenter) and rk (the regex extractor's first word)
	is($out, '2|f', 'and a leading comment of the same statement still counts');
	$out = $node->safe_psql('u8', q{
		SELECT ntags FROM pssc_extract_test('SELECT 1 /*' || repeat('/*', 100000) || 'a=''b''' || repeat('*/', 100000) || '*/')});
	like($out, qr/^\d+$/, 'deeply nested comment');
}

# --- size limits: max_tagset_bytes, max_tags, max_tag_value_len ---

config(tags => '*', exclude_tags => '', extractors => 'sqlcommenter(position=any)');
{
	# bufsize stands in for a smaller max_tagset_bytes
	my $q = sqlq(q{SELECT 1 /*e='5',a='1',d='4',b='2',c='3'*/});
	my $r = ex($q, args => [ -1, 0, 13 ]);
	is("$r->{tags} $r->{nbytes} $r->{dropped}", 'a=1,b=2,c=3 12 2',
		'tags = *: overflow dropped in reverse sorted-key order');
	config(tags => 'e, c, a, b, d', extractors => 'sqlcommenter(position=any)');
	$r = ex($q, args => [ -1, 0, 13 ]);
	is("$r->{tags} $r->{nbytes} $r->{dropped}", 'a=1,c=3,e=5 12 2',
		'allowlist: overflow dropped in reverse list order');
	$r = ex(sqlq(q{SELECT 1 /*e='5',a='1',c='3333333'*/}), args => [ -1, 0, 13 ]);
	is("$r->{tags} $r->{dropped}", 'a=1,e=5 1',
		'greedy: a tag that does not fit is dropped, later ones that fit are kept');
}
$node->append_conf('postgresql.conf',
	"$P.max_tags = 2\n$P.max_tag_value_len = 4\n$P.max_tagset_bytes = 128\n");
$node->restart;
config(tags => '*', exclude_tags => '', extractors => 'sqlcommenter(position=any)');
{
	my $r = ex(sqlq(q{SELECT 1 /*c='3',a='1',b='2'*/}));
	is("$r->{tags} $r->{dropped}", 'a=1,b=2 1', 'max_tags');
	$r = ex(sqlq(q{SELECT 1 /*a='abcdef',b='ééé',c='aéé'*/}), args => [ -1, 0, 8192 ]);
	is($r->{tags}, 'a=abcd,b=éé', 'UTF8: values truncated on character boundaries');
	$r = ex(sqlq(q{SELECT 1 /*c='aéé'*/}));
	is($r->{serialized}, unpack('H*', "c\0a\xc3\xa9\0"), 'multibyte character not split');
	$r = ex(bq("SELECT 1 /*b='\xe9\xe9\xe9\xe9\xe9'*/"), db => 'l1');
	is($r->{serialized}, unpack('H*', "b\0\xe9\xe9\xe9\xe9\0"), 'LATIN1: 4 one-byte characters');
	my $k = 'k' x 60;
	$r = ex(sqlq("SELECT 1 /*${k}1='vvvv',${k}2='vvvv'*/"));
	is("$r->{ntags} $r->{nbytes} $r->{dropped}", '1 67 1',
		'max_tagset_bytes = 128 holds one 67-byte tag of two');
}

$node->stop;
done_testing();
