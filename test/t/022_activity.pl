# The activity view pg_stat_statement_context_activity (DESIGN.md §6.11, §7,
# §8; backlog 20261005-091225-39): one row per backend that has run a
# top-level statement with a frame, showing the tags of the running
# top-level statement (state 'active') or of the last one (state 'idle'),
# joinable on pid to pg_stat_activity:
#   - catalog columns, attributes and the docs' column table and example;
#   - a backend's own running statement, another backend's running (blocked)
#     statement, its idle state afterwards, and updates across statements;
#   - nested statements (PL/pgSQL, CALL) with nested_tags = scan: the view
#     keeps the top-level statement's tags and queryid, while the nested
#     statement is recorded with its own tags;
#   - multi-statement strings, errors, idle in transaction after FETCH,
#     statements without a frame (enabled = off), and backend exit (the
#     slot is cleared, read by a session that was already connected so the
#     exiting backend's slot is not reused);
#   - visibility (§6.11): other roles' queryid, state and tags are NULL
#     unless the caller has the privileges of pg_read_all_stats (also not
#     for a NOINHERIT member); a role's own rows are complete.
use strict;
use warnings;

use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);
use Time::HiRes qw(usleep);

my $P = 'pg_stat_statement_context';
my $V = "${P}_activity";
my $root = dirname(__FILE__) . '/../..';

my $node = PostgreSQL::Test::Cluster->new('activity');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$P'
max_connections = 20
autovacuum = off
max_parallel_workers_per_gather = 0
});
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sql(qq{CREATE EXTENSION $P;
       CREATE ROLE alice LOGIN;
       CREATE ROLE bob LOGIN;
       CREATE ROLE stats LOGIN IN ROLE pg_read_all_stats;
       CREATE ROLE noinh LOGIN NOINHERIT IN ROLE pg_read_all_stats;
       CREATE ROLE carol LOGIN;
       GRANT bob TO carol;
       CREATE FUNCTION nested_wait() RETURNS int LANGUAGE plpgsql AS \$f\$
       DECLARE r int;
       BEGIN
         SELECT 1 INTO r FROM pg_advisory_lock(42) /*controller='inner',action='nested'*/;
         PERFORM pg_advisory_unlock(42);
         RETURN r;
       END \$f\$;
       CREATE PROCEDURE proc_wait() LANGUAGE plpgsql AS \$f\$
       BEGIN
         PERFORM nested_wait();
       END \$f\$;
       CREATE FUNCTION fold_wait() RETURNS int LANGUAGE plpgsql IMMUTABLE AS \$f\$
       DECLARE r int;
       BEGIN
         SELECT 1 INTO r FROM pg_advisory_lock(42) /*controller='fold',action='inner'*/;
         PERFORM pg_advisory_unlock(42);
         RETURN r;
       END \$f\$;
       CREATE FUNCTION fold_fail() RETURNS int LANGUAGE plpgsql IMMUTABLE AS \$f\$
       BEGIN
         PERFORM 1 /*controller='foldfail',action='inner'*/;
         RAISE EXCEPTION 'fold failed';
       END \$f\$;
       GRANT EXECUTE ON FUNCTION nested_wait() TO PUBLIC;
       GRANT EXECUTE ON PROCEDURE proc_wait() TO PUBLIC;});

# ------------------------------------------------- persistent sessions
# IPC::Run directly: PostgreSQL::Test::BackgroundPsql is missing from some
# supported releases (e.g. 15.0) and PG14's background_psql differs.
my $marker = 0;
sub session_open
{
	my ($user) = @_;
	my %s = (in => '', out => '', err => '');
	$s{h} = IPC::Run::start(
		[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=0', '-d',
		  $node->connstr('postgres') . " user=$user" ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err},
		IPC::Run::timeout(300));
	$s{pid} = sq(\%s, 'SELECT pg_backend_pid()');
	return \%s;
}
# Send statements without waiting for them; sq_wait() collects the result.
sub sq_send
{
	my ($s, $q) = @_;
	$s->{m} = '__pssc_done_' . ++$marker . '__';
	$s->{out} = '';
	$s->{err} = '';
	$s->{in} .= "$q;\n\\echo $s->{m}\n";
	$s->{h}->pump_nb;
}
# Wait for the statements sent last; returns (output, error output).
sub sq_wait
{
	my ($s) = @_;
	my $m = $s->{m};
	$s->{h}->pump until $s->{out} =~ /^\Q$m\E$/m;
	(my $r = $s->{out}) =~ s/^\Q$m\E\n\z//m;
	chomp $r;
	return ($r, $s->{err});
}
# Run statements; dies on any error output.
sub sq
{
	my ($s, $q) = @_;
	sq_send($s, $q);
	my ($r, $err) = sq_wait($s);
	die "session error for <$q>: $err" if $err ne '';
	return $r;
}
sub session_close
{
	my ($s) = @_;
	$s->{in} .= "\\q\n";
	$s->{h}->finish;
}

# ---------------------------------------- extended protocol, raw messages
# A minimal frontend over the node's Unix socket (trust authentication), to
# send Parse/Bind/Execute/Close/Sync in any order on every supported
# release (psql's \bind is PG16+, \bind_named PG18+, DBD::Pg is optional).
sub pmsg
{
	my ($type, $body) = @_;
	return $type . pack('N', length($body) + 4) . $body;
}
sub p_parse   { return pmsg('P', "$_[0]\0$_[1]\0" . pack('n', 0)); }
sub p_bind    { return pmsg('B', "$_[0]\0$_[1]\0" . pack('nnn', 0, 0, 0)); }
sub p_execute { return pmsg('E', "$_[0]\0" . pack('N', 0)); }
sub p_close   { return pmsg('C', "P$_[0]\0"); }
sub p_sync    { return pmsg('S', ''); }
sub p_query   { return pmsg('Q', "$_[0]\0"); }

sub p_readn
{
	my ($c, $n) = @_;
	my $buf = '';
	while (length($buf) < $n)
	{
		my $got = sysread($c->{sock}, $buf, $n - length($buf), length($buf));
		die "protocol read: " . (defined $got ? 'EOF' : $!) unless $got;
	}
	return $buf;
}

# Send messages, read until every ReadyForQuery they produce (one per Query
# or Sync message; one for the startup packet); returns the error messages.
sub p_run
{
	my ($c, @msgs) = @_;
	my $out = join('', @msgs);
	my $expect = grep { /^[QS]/ } @msgs;
	$expect = 1 if $expect == 0;
	local $SIG{ALRM} = sub { die "protocol timeout\n" };
	alarm($PostgreSQL::Test::Utils::timeout_default);
	while (length($out) > 0)
	{
		my $n = syswrite($c->{sock}, $out);
		die "protocol write: $!" unless defined $n;
		substr($out, 0, $n) = '';
	}
	my $err = '';
	while (1)
	{
		my ($type, $len) = unpack('a N', p_readn($c, 5));
		my $body = $len > 4 ? p_readn($c, $len - 4) : '';
		$err .= ($body =~ /(?:^|\0)M([^\0]*)/)[0] . "\n" if $type eq 'E';
		$c->{pid} = unpack('N', $body) if $type eq 'K';
		last if $type eq 'Z' && --$expect == 0;
	}
	alarm(0);
	return $err;
}

sub p_open
{
	my ($user) = @_;
	my $path = $node->host . '/.s.PGSQL.' . $node->port;
	my $sock = IO::Socket::UNIX->new(Type => SOCK_STREAM(), Peer => $path)
	  or die "connect $path: $!";
	my $c = { sock => $sock };
	my $body = pack('N', 196608) . "user\0$user\0database\0postgres\0\0";
	my $err = p_run($c, pack('N', length($body) + 4) . $body);
	die "startup: $err" if $err ne '';
	return $c;
}

sub p_close_conn
{
	my ($c) = @_;
	syswrite($c->{sock}, pmsg('X', ''));
	close($c->{sock});
}

# Poll until $q returns 't' (as the superuser, new connection each time).
sub wait_for
{
	my ($q) = @_;
	for (1 .. 600)
	{
		return 1 if sql($q) eq 't';
		usleep(50_000);
	}
	return 0;
}
sub wait_lock
{
	my ($pid) = @_;
	return wait_for(qq{SELECT EXISTS (SELECT FROM pg_stat_activity
	                   WHERE pid = $pid AND wait_event_type = 'Lock')});
}

# "state|tags|queryid matches pg_stat_activity.query_id" of $pid's row,
# read as role $as (superuser if undef); '' if there is no row.
sub act
{
	my ($pid, $as, $s) = @_;
	my $q = qq{SELECT concat_ws('|', coalesce(c.state, 'NULL'), coalesce(c.tags::text, 'NULL'),
	                  CASE WHEN c.queryid IS NULL THEN 'NULL'
	                       WHEN c.queryid = a.query_id THEN 'qid' ELSE 'qid-mismatch' END)
	             FROM $V c LEFT JOIN pg_stat_activity a USING (pid) WHERE c.pid = $pid};
	return $s ? sq($s, $q) : sql(($as ? "SET ROLE $as; " : '') . $q);
}

# ------------------------------------------------------------- catalog
{
	is(sql(qq{SELECT string_agg(attname || ':' || format_type(atttypid, atttypmod), ','
	                            ORDER BY attnum)
	            FROM pg_attribute WHERE attrelid = '$V'::regclass AND attnum > 0}),
		'pid:integer,userid:oid,dbid:oid,queryid:bigint,state:text,tags:jsonb',
		'view columns');
	is(sql(qq{SELECT concat(provolatile, proparallel, proretset, proisstrict)
	            FROM pg_proc WHERE oid = '$V()'::regprocedure}),
		'vstt', 'function: volatile, parallel safe, set-returning, strict');
	is(sql(qq{SET ROLE bob; SELECT count(*) > 0 FROM $V}), 't',
		'readable by PUBLIC');

	# docs/sql-interface.md: the column table and the example join
	my $docs = slurp_file("$root/docs/sql-interface.md");
	my ($sec) = $docs =~ /^## `$V`\n(.*?)^## /ms
	  or die "docs: section $V not found";
	my @c = $sec =~ /^\| `(\w+)` \| `(\w+)` \|/mg;
	my @out;
	push @out, shift(@c) . ':' . shift(@c) while @c;
	is(join(',', @out) =~ s/:int\b/:integer/gr,
		'pid:integer,userid:oid,dbid:oid,queryid:bigint,state:text,tags:jsonb',
		'docs: the column table matches the catalog');
	my @blocks = grep { /JOIN $V c USING \(pid\)/ } ($sec =~ /^```sql\n(.*?)^```$/msg);
	is(scalar(@blocks), 1, 'docs: one example join on pid');
	$node->psql('postgres', $blocks[0], stdout => \my $o, stderr => \my $e);
	is($e, '', 'docs: the example join runs');
	like($o, qr/\S/, 'docs: the example join returns rows (at least the reading statement)');
}

# ------------------------------------------- own statement, other backend
is(sql(qq{SELECT concat_ws('|', c.state, c.tags::text, c.queryid = a.query_id)
            FROM $V c JOIN pg_stat_activity a USING (pid)
           WHERE pid = pg_backend_pid() /*controller='self',action='look'*/}),
	'active|{"action": "look", "controller": "self"}|t',
	'own running statement: active, its tags and queryid');

my $locker = session_open('postgres');
my $obs = session_open('postgres');   # stays connected: reads after exits
my $a = session_open('alice');
sq($locker, 'SELECT pg_advisory_lock(42)');

sq_send($a, q{SELECT pg_advisory_lock(42) /*controller='blocked',action='wait'*/});
ok(wait_lock($a->{pid}), 'alice is blocked');
is(act($a->{pid}), 'active|{"action": "wait", "controller": "blocked"}|qid',
	'running statement of another backend: active, its tags, queryid = pg_stat_activity.query_id');
is(sql(qq{SELECT c.userid = 'alice'::regrole AND c.dbid = a.datid
            FROM $V c JOIN pg_stat_activity a USING (pid) WHERE pid = $a->{pid}}), 't',
	'userid and dbid');

# --------------------------------------------------------- visibility
is(act($a->{pid}, 'bob'), 'NULL|NULL|NULL',
	'unprivileged role: state, tags and queryid of another role are NULL');
is(sql(qq{SET ROLE bob; SELECT pid = $a->{pid} AND userid = 'alice'::regrole AND dbid IS NOT NULL
            FROM $V WHERE pid = $a->{pid}}), 't',
	'unprivileged role: pid, userid and dbid are shown');
is(sql(qq{SET ROLE bob; SELECT concat_ws('|', state, tags::text, queryid IS NOT NULL)
            FROM $V WHERE pid = pg_backend_pid() /*controller='mine',action='bob'*/}),
	'active|{"action": "bob", "controller": "mine"}|t',
	'unprivileged role: its own row is complete');
is(act($a->{pid}, 'stats'), 'active|{"action": "wait", "controller": "blocked"}|qid',
	'pg_read_all_stats member sees other roles\' tags');
is(act($a->{pid}, 'noinh'), 'NULL|NULL|NULL',
	'NOINHERIT member of pg_read_all_stats does not');
is(act($a->{pid}, 'alice'), 'active|{"action": "wait", "controller": "blocked"}|qid',
	'the same role in another session sees its rows');

# ------------------------------------------------ idle, next statements
sq($locker, 'SELECT pg_advisory_unlock(42)');
my ($r, $err) = sq_wait($a);
is($err, '', 'alice got the lock');
sq($a, 'SELECT pg_advisory_unlock(42)');
is(act($a->{pid}), 'idle|{}|qid',
	'idle after the statement: the last top-level statement (untagged: {}) and its queryid');
sq($a, q{SELECT 1 /*controller='next',action='one'*/});
is(act($a->{pid}), 'idle|{"action": "one", "controller": "next"}|qid',
	'updated by the next statement');
is(act($a->{pid}, 'bob'), 'NULL|NULL|NULL', 'idle row of another role: still hidden');

# multi-statement string (psql \; sends one query string)
sq($locker, 'SELECT pg_advisory_lock(42)');
sq_send($a, q{SELECT 1 /*controller='first',action='a'*/ \; SELECT pg_advisory_lock(42) /*controller='second',action='b'*/});
ok(wait_lock($a->{pid}), 'multi-statement string: blocked in the second statement');
is(act($a->{pid}), 'active|{"action": "b", "controller": "second"}|qid',
	'multi-statement string: the running statement\'s own tags');
sq($locker, 'SELECT pg_advisory_unlock(42)');
($r, $err) = sq_wait($a);
is($err, '', 'multi-statement string done');
sq($a, 'SELECT pg_advisory_unlock(42)');

# an error in the executor ends the statement: idle, its tags kept
sq_send($a, q{SELECT 1 / g FROM generate_series(1, 0, -1) g /*controller='err',action='fail'*/});
($r, $err) = sq_wait($a);
like($err, qr/division by zero/, 'statement failed in the executor');
is(act($a->{pid}), 'idle|{"action": "fail", "controller": "err"}|qid',
	'failed statement: idle with its tags');

# idle in transaction after FETCH
sq($a, q{BEGIN; DECLARE cur CURSOR FOR SELECT g FROM generate_series(1, 10) g /*controller='cursor',action='query'*/});
sq($a, q{FETCH 1 FROM cur /*controller='fetch',action='cursor'*/});
is(sql(qq{SELECT state FROM pg_stat_activity WHERE pid = $a->{pid}}), 'idle in transaction',
	'alice is idle in transaction');
is(act($a->{pid}), 'idle|{"action": "cursor", "controller": "fetch"}|qid',
	'idle in transaction: the tags of the last statement (FETCH)');
sq($a, q{COMMIT /*controller='cursor',action='commit'*/});
is(act($a->{pid}), 'idle|{"action": "commit", "controller": "cursor"}|qid',
	'COMMIT is the row, not the cursor dropped at commit');

# ------------------------------------------------------ nested statements
my $b = session_open('postgres');
sq($b, qq{SET $P.nested_tags = scan; SET $P.track = 'all'; SELECT ${P}_reset()});
sq($locker, 'SELECT pg_advisory_lock(42)');
sq_send($b, q{SELECT nested_wait() /*controller='outer',action='top'*/});
ok(wait_lock($b->{pid}), 'nested statement blocked');
is(act($b->{pid}), 'active|{"action": "top", "controller": "outer"}|qid',
	'nested statement running: the top-level statement\'s tags and queryid');
sq($locker, 'SELECT pg_advisory_unlock(42)');
($r, $err) = sq_wait($b);
is($err, '', 'nested statement done');
is(act($b->{pid}), 'idle|{"action": "top", "controller": "outer"}|qid',
	'after the nested statement ended: still the top-level statement\'s tags');
is(sql(qq{SELECT count(*) > 0 FROM $P
           WHERE NOT toplevel AND tags = '{"action": "nested", "controller": "inner"}'}), 't',
	'the nested statement had its own tags (nested_tags = scan)');

sq($locker, 'SELECT pg_advisory_lock(42)');
sq_send($b, q{CALL proc_wait() /*controller='proc',action='call'*/});
ok(wait_lock($b->{pid}), 'CALL blocked in a nested statement');
is(act($b->{pid}), 'active|{"action": "call", "controller": "proc"}|qid',
	'CALL running: the utility\'s tags and queryid');
sq($locker, 'SELECT pg_advisory_unlock(42)');
($r, $err) = sq_wait($b);
is($err, '', 'CALL done');
is(act($b->{pid}), 'idle|{"action": "call", "controller": "proc"}|qid',
	'after CALL: idle with its tags');

# statements run while planning (constant folding of an IMMUTABLE function)
# are not top level on any version: the row stays the previous statement's
sq($locker, 'SELECT pg_advisory_lock(42)');
sq_send($b, q{SELECT fold_wait() /*controller='fold',action='outer'*/});
ok(wait_lock($b->{pid}), 'blocked in a statement run while planning');
is(act($b->{pid}), 'idle|{"action": "call", "controller": "proc"}|qid-mismatch',
	'while planning: still the previous statement\'s row');
sq($locker, 'SELECT pg_advisory_unlock(42)');
($r, $err) = sq_wait($b);
is($err, '', 'plan-time statement done');
is(act($b->{pid}), 'idle|{"action": "outer", "controller": "fold"}|qid',
	'after planning, the executor publishes the top-level statement');
sq_send($b, q{SELECT fold_fail() /*controller='foldfail',action='outer'*/});
($r, $err) = sq_wait($b);
like($err, qr/fold failed/, 'statement failed while planning');
is(act($b->{pid}), 'idle|{"action": "outer", "controller": "fold"}|qid-mismatch',
	'failed while planning: the previous statement\'s row, not the plan-time statement\'s');

# PREPARE keeps the row, EXECUTE shows the prepared statement's tags,
# DEALLOCATE is a utility statement with its own tags
sq($b, q{PREPARE p AS SELECT 1 /*controller='prep',action='stmt'*/});
is(act($b->{pid}), 'idle|{"action": "outer", "controller": "fold"}|qid-mismatch',
	'PREPARE leaves the row unchanged');
sq($b, q{EXECUTE p});
# pg_stat_activity.query_id is the EXECUTE's, the row has the prepared statement's
like(act($b->{pid}), qr/^idle\|\{"action": "stmt", "controller": "prep"\}\|qid/,
	'EXECUTE: the prepared statement\'s tags');
sq($b, q{DEALLOCATE p /*controller='dealloc',action='d'*/});
like(act($b->{pid}), qr/^idle\|\{"action": "d", "controller": "dealloc"\}\|/,
	'DEALLOCATE: its own tags');

# ------------------------------------------------- extended protocol
my $x = p_open('carol');
is(p_run($x, p_query(q{SELECT 1 /*controller='ext',action='a'*/})), '', 'carol: statement A');
is(act($x->{pid}), 'idle|{"action": "a", "controller": "ext"}|qid', 'carol: A is the row');

# a portal bound but never executed doesn't become the row when it is
# dropped: by Close, or by Sync ending the implicit transaction
is(p_run($x, p_query(q{BEGIN /*controller='ext',action='begin'*/}),
		 p_parse('', q{SELECT 2 /*controller='ext',action='closed'*/}),
		 p_bind('', ''), p_close(''), p_sync()), '', 'Bind, then Close without Execute');
is(act($x->{pid}), 'idle|{"action": "begin", "controller": "ext"}|qid-mismatch',
	'a portal closed without Execute leaves the row unchanged');
is(p_run($x, p_query('COMMIT /*controller=\'ext\',action=\'commit\'*/')), '', 'COMMIT');
is(act($x->{pid}), 'idle|{"action": "commit", "controller": "ext"}|qid', 'COMMIT is the row');
is(p_run($x, p_parse('', q{SELECT 3 /*controller='ext',action='synced'*/}),
		 p_bind('', ''), p_sync()), '', 'Bind, then Sync without Execute');
is(act($x->{pid}), 'idle|{"action": "commit", "controller": "ext"}|qid-mismatch',
	'a portal dropped by Sync without Execute leaves the row unchanged');

# a portal bound as carol and executed after SET ROLE bob runs as bob:
# the row belongs to bob (hidden from carol, complete for bob)
is(p_run($x, p_query('BEGIN'),
		 p_parse('s1', q{SELECT 4 /*controller='ext',action='asbob'*/}),
		 p_bind('p1', 's1'),
		 p_parse('', 'SET ROLE bob'), p_bind('', ''), p_execute(''),
		 p_execute('p1'), p_sync()), '', 'portal bound as carol, executed as bob');
is(sql(qq{SELECT userid::regrole FROM $V WHERE pid = $x->{pid}}), 'bob',
	'the row\'s userid is the role the statement executed as');
is(act($x->{pid}, 'carol'), 'NULL|NULL|NULL', 'carol does not see bob\'s row');
like(act($x->{pid}, 'bob'), qr/^idle\|\{"action": "asbob", "controller": "ext"\}\|qid/,
	'bob sees his own row');
is(p_run($x, p_query('COMMIT; RESET ROLE')), '', 'carol: COMMIT');
p_close_conn($x);

# ------------------------------------- statements without a frame clear
sq($b, qq{SET $P.enabled = off});
sq($b, q{SELECT 2 /*controller='off',action='x'*/});
is(act($b->{pid}), '', 'a top-level statement without a frame (enabled = off) clears the row');
sq($b, qq{SET $P.enabled = on});
sq($b, q{SELECT 3 /*controller='on',action='y'*/});
is(act($b->{pid}), 'idle|{"action": "y", "controller": "on"}|qid', 'and the next one sets it again');

# -------------------------------------------------------------- exit
my $apid = $a->{pid};
session_close($a);
my $gone = 0;
for (1 .. 600)
{
	if (sq($obs, qq{SELECT count(*) FROM pg_stat_activity WHERE pid = $apid}) eq '0')
	{
		$gone = 1;
		last;
	}
	usleep(50_000);
}
ok($gone, 'alice\'s backend exited');
is(sq($obs, qq{SELECT count(*) FROM $V WHERE pid = $apid}), '0',
	'backend exit clears its row (read by an already connected session)');
is(sq($obs, qq{SELECT count(*) FROM $V WHERE pid NOT IN (SELECT pid FROM pg_stat_activity)}), '0',
	'no rows of exited backends');

# ------------------------------------------- microbenchmark smoke check
sql('CREATE EXTENSION pssc_context_test');
is(sql(qq{SELECT concat_ws('|', write_ns > 0, read_ns > 0, nslots >= 20,
                          (SELECT concat_ws('|', state, tags::text) FROM $V
                            WHERE pid = pg_backend_pid()))
            FROM pssc_context_test_activity_bench(1000, 30) /*controller='after',action='bench'*/}),
	't|t|t|active|{"action": "bench", "controller": "after"}',
	'microbenchmark (docs/benchmarks.md) runs, then publishes the calling statement again');

session_close($_) for ($b, $locker, $obs);
$node->stop;
done_testing();
