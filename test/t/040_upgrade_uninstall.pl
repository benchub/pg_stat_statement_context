# The lifecycle procedures of docs/upgrading.md (backlog 20261008-065635-9),
# on the testing and the release build alike (no TEST-ONLY module or hook):
#   - a clean restart keeps the statistics (as after a library-only upgrade
#     that keeps default_version), and ALTER EXTENSION ... UPDATE is a no-op
#     at the current version;
#   - a dump with a different format version (PSSC_DUMP_FORMAT, as an older
#     or newer library writes it) is discarded, even with a valid checksum;
#     so are a different PostgreSQL major and extension version;
#   - DROP EXTENSION / CREATE EXTENSION does not touch the statistics, which
#     are collected for every database while the library is preloaded;
#   - uninstalling: once the library is out of shared_preload_libraries the
#     functions fail, DROP EXTENSION still works, and the dump file stays in
#     pg_stat/ (and is loaded again if the library comes back) until it is
#     deleted.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscTest;

my $P = 'pg_stat_statement_context';

my $node = PostgreSQL::Test::Cluster->new('lifecycle');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = '$P'\ncompute_query_id = auto\n");
$node->start;
my $DUMP = $node->data_dir . "/pg_stat/$P.stat";

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

# Runs a statement tagged controller=$c $n times in database $db.
sub run_tagged
{
	my ($c, $n, $db) = @_;
	sql(join('', map { "SELECT $_ /*controller:$c,action:a*/;\n" } 1 .. $n),
		$db);
}

# Calls recorded for controller $c (in the instance, every database).
sub calls
{
	my ($c) = @_;
	return sql(
		qq{SELECT coalesce(sum(calls_total), 0) FROM ${P}_totals
		   WHERE tags->>'controller' = '$c'});
}

# Stops (fast), runs $between while the server is down, starts it, and
# returns the server log written since before the stop.
sub restart
{
	my ($between) = @_;
	my $pos = -s $node->logfile;
	$node->stop('fast');
	$between->() if $between;
	$node->start;
	return substr(slurp_file($node->logfile), $pos);
}

# CRC-32C (Castagnoli, reflected, as PostgreSQL's pg_crc32c).
my @crc_table = map {
	my $c = $_;
	$c = ($c & 1) ? (($c >> 1) ^ 0x82F63B78) : ($c >> 1) for 1 .. 8;
	$c;
} 0 .. 255;

sub crc32c
{
	my $crc = 0xFFFFFFFF;
	$crc = $crc_table[($crc ^ $_) & 0xFF] ^ ($crc >> 8) for unpack('C*', $_[0]);
	return $crc ^ 0xFFFFFFFF;
}

sub read_dump
{
	open my $fh, '<:raw', $DUMP or die "open $DUMP: $!";
	local $/;
	my $d = <$fh>;
	close $fh;
	return $d;
}

sub write_dump
{
	open my $fh, '>:raw', $DUMP or die "open $DUMP: $!";
	print $fh $_[0] or die "write: $!";
	close $fh or die "close: $!";
}

# Header layout (src/store.c, PsscDumpHeader): magic uint32 at 0, format
# version uint32 at 4, PostgreSQL major uint32 at 8, extension version
# char[20] at 12; the file ends with the CRC-32C (native uint32) of the rest.
# Overwrites bytes at $offset and recomputes the checksum, so that only the
# version check can reject the file.
sub patch_dump_keep_crc
{
	my ($offset, $bytes) = @_;
	my $d = read_dump();
	my $body = substr($d, 0, length($d) - 4);
	substr($body, $offset, length($bytes)) = $bytes;
	write_dump($body . pack('L', crc32c($body)));
}

$node->safe_psql('postgres', "CREATE EXTENSION $P");
is(sql(qq{SELECT extversion FROM pg_extension WHERE extname = '$P'}),
	'1.0', 'extversion is the installed SQL version');

# ------------------------------------ clean restart (library-only upgrade)
run_tagged('keep', 3);
is(calls('keep'), 3, 'recorded before the restart');
{
	my $log = restart(sub {
		ok(-f $DUMP, 'a clean shutdown writes the dump');
		my $d = read_dump();
		is( unpack('L', substr($d, length($d) - 4)),
			crc32c(substr($d, 0, length($d) - 4)),
			'the CRC-32C of the test matches the one the library wrote');
	});
	like($log, qr/loaded \d+ of \d+ saved entries/, 'restart: the dump is loaded');
	is(calls('keep'), 3, 'restart: the statistics are kept');
	ok(!-e $DUMP, 'restart: the dump is removed once loaded');
}

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "ALTER EXTENSION $P UPDATE");
is($ret, 0, 'ALTER EXTENSION ... UPDATE succeeds');
like($stderr, qr/version "1\.0" of extension "$P" is already installed/,
	'ALTER EXTENSION ... UPDATE at the current version is a no-op');

# ------------------------------------ a dump with another format version
# A valid checksum, so the file is rejected for its version and nothing
# else. The control case patches the format with its own value: it loads.
my $format = unpack('L', substr(read_dump_after_stop(), 4, 4));

sub read_dump_after_stop
{
	# the dump exists only while the server is down
	my $d;
	restart(sub { $d = read_dump(); });
	return $d;
}

{
	my $log = restart(sub { patch_dump_keep_crc(4, pack('L', $format)); });
	like($log, qr/loaded \d+ of \d+ saved entries/,
		'same format, recomputed checksum: the dump is loaded');
	is(calls('keep'), 3, 'same format: the statistics are kept');
}

for my $c ([ 'an older format version', 4, pack('L', $format - 1) ],
	[ 'a newer format version', 4, pack('L', $format + 1) ],
	[ 'another PostgreSQL major', 8, pack('L', 13) ],
	[ 'another extension version', 12, "0.9\0" ])
{
	my ($what, $off, $bytes) = @$c;
	run_tagged('keep', 1);
	ok(calls('keep') > 0, "$what: statistics before the restart");
	my $log = restart(sub {
		ok(-f $DUMP, "$what: dump written");
		patch_dump_keep_crc($off, $bytes);
	});
	like($log,
		qr/discarding saved statistics in ".*": written by a different version/,
		"$what: the dump is discarded");
	if ($off == 4)
	{
		my $f = unpack('L', $bytes);
		like($log, qr/The file has format $f, .*expected format $format,/,
			"$what: the LOG detail gives both formats");
	}
	no_crash_ok($log, "$what: no crash");
	is(calls('keep'), 0, "$what: the store starts empty");
	ok(!-e $DUMP, "$what: the file is removed");
}

# ------------------------------------ DROP / CREATE EXTENSION
$node->safe_psql('postgres', 'CREATE DATABASE other');
run_tagged('dropped', 2);
$node->safe_psql('postgres', "DROP EXTENSION $P");
run_tagged('dropped', 2);
run_tagged('dropped', 1, 'other');
$node->safe_psql('postgres', "CREATE EXTENSION $P");
is(calls('dropped'), 5,
	'DROP EXTENSION keeps the statistics, and every database is recorded '
	  . 'while the library is preloaded');

# ------------------------------------ uninstall
run_tagged('stale', 4);
{
	my $log = restart(sub {
		ok(-f $DUMP, 'uninstall: the last clean shutdown with the library writes the dump');
		$node->append_conf('postgresql.conf', "shared_preload_libraries = ''\n");
	});
	my ($ret, $o, $err) = $node->psql('postgres', "SELECT * FROM ${P}_totals");
	isnt($ret, 0, 'uninstall: the views fail without the library');
	like($err, qr/$P must be loaded via "shared_preload_libraries"/,
		'uninstall: the error names shared_preload_libraries');
	$node->safe_psql('postgres', "DROP EXTENSION $P");
	is(sql(qq{SELECT count(*) FROM pg_extension WHERE extname = '$P'}),
		0, 'uninstall: DROP EXTENSION works without the library');
	ok(-f $DUMP, 'uninstall: the dump file stays in pg_stat/ without the library');
	$node->restart;
	ok(-f $DUMP, 'uninstall: and across further restarts');

	# put back without deleting the file: the old statistics come back
	$log = restart(sub {
		$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
	});
	$node->safe_psql('postgres', "CREATE EXTENSION $P");
	like($log, qr/loaded \d+ of \d+ saved entries/,
		'reinstall: a dump left behind is loaded');
	is(calls('stale'), 4, 'reinstall: with the old statistics');

	# deleting the file while the server is down starts it empty
	$log = restart(sub {
		ok(-f $DUMP, 'delete: dump written');
		unlink($DUMP) or die "unlink $DUMP: $!";
	});
	unlike($log, qr/loaded \d+ of \d+ saved entries|discarding/,
		'delete: nothing is loaded or discarded');
	is(calls('stale'), 0, 'delete: the store starts empty');
}

$node->stop;
no_crash_ok(slurp_file($node->logfile), 'the server log is clean');

done_testing();
