# The managed-service operator guide, docs/managed-services.md, against the
# server (backlog 20261008-065635-8). On a managed service the administrator
# is not a superuser; it is stood in for here by the role "dba": NOSUPERUSER
# with CREATEDB, CREATEROLE and pg_monitor (the closest stand-in for, e.g.,
# rds_superuser). The guide is read, not copied, so it and this test cannot
# drift apart:
#   - every privilege statement of the guide carries an HTML comment
#     <!-- check: NAME -->, and every NAME has exactly one check below (and
#     vice versa), run as dba and as plain roles;
#   - its table of settings and the reference table of
#     docs/configuration.md give every GUC's context as in pg_settings;
#   - every raw parameter-group value of a <!-- raw-example --> section, set
#     with ALTER SYSTEM and the equivalent SQL quoting, shows as itself in
#     SHOW and makes the example's _extract() query return the documented
#     tags; the section's SQL and postgresql.conf forms produce the same
#     value;
#   - every SQL block of the troubleshooting checklist runs as dba (the
#     pg_file_settings one fails as documented);
#   - the "superuser only" wording for _reset() and _extract() is gone,
#     configuration.md explains the GUC contexts once, and every relative
#     link of the Markdown documents resolves (files and anchors).
use strict;
use warnings;

use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PsscDocLinks qw(check_relative_links);

my $P = 'pg_stat_statement_context';
my $root = dirname(__FILE__) . '/../..';
my $guide_path = "$root/docs/managed-services.md";

# ------------------------------------------------------------ documents
ok(-f $guide_path, 'docs/managed-services.md exists');
my $guide = -f $guide_path ? slurp_file($guide_path) : '';
my $readme = slurp_file("$root/README.md");
my $config = slurp_file("$root/docs/configuration.md");

ok($readme =~ /\]\(docs\/managed-services\.md\)/,
	'README.md links to docs/managed-services.md');

# DOC-13: one list of the GUC contexts in configuration.md (it had two).
my @ctx_lists = $config =~ /^- (?:\*\*)?`?postmaster`?(?:\*\*)?:/mg;
is(scalar(@ctx_lists), 1, 'configuration.md explains the GUC contexts once');

# _reset() and _extract() are not "superuser only": their owner and
# granted roles can call them too.
{
	my @hits;
	for my $f ("$root/README.md", glob("$root/docs/*.md"))
	{
		my $t = slurp_file($f);
		push @hits, "$f: $1" while $t =~ /^(.*superuser[- ]only.*)$/mig;
	}
	is_deeply(\@hits, [], 'no "superuser only" claims left in README.md and docs/')
	  or diag(join("\n", @hits));
}

# ------------------------------------------------------------ the server
my $node = PostgreSQL::Test::Cluster->new('managed');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = '$P'\n");
$node->start;
my $vnum = $node->safe_psql('postgres', 'SHOW server_version_num');
note "server_version_num = $vnum";

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

# Runs $sql as $role in $db; returns (exit code, stdout, stderr).
sub run_as
{
	my ($role, $db, $sql) = @_;
	my ($out, $err) = ('', '');
	my $rc = $node->psql($db, $sql, stdout => \$out, stderr => \$err,
		extra_params => [ '-U', $role ]);
	return ($rc, $out, $err);
}

sub ok_as
{
	my ($role, $db, $sql, $name) = @_;
	my ($rc, $out, $err) = run_as($role, $db, $sql);
	ok($rc == 0, $name) or diag("as $role in $db: $sql\n$err");
	return $out;
}

sub fails_as
{
	my ($role, $db, $sql, $re, $name) = @_;
	my ($rc, $out, $err) = run_as($role, $db, $sql);
	ok($rc != 0 && $err =~ $re, $name)
	  or diag("as $role in $db: $sql\nexit $rc, stderr: $err");
}

# The managed-service administrator, and the application and monitoring
# roles it creates. dba creates appdb; the extension there is created by
# dba (made superuser only for that statement, as the provider's mechanism
# would), so dba owns its functions. In postgres the extension is created
# by a real superuser.
sql(q{CREATE ROLE dba LOGIN NOSUPERUSER CREATEDB CREATEROLE;
	GRANT pg_monitor TO dba});
ok_as('dba', 'postgres', q{CREATE DATABASE appdb;
	CREATE ROLE app LOGIN; CREATE ROLE monitoring LOGIN; CREATE ROLE other LOGIN},
	'dba creates the database and the roles');
sql('ALTER ROLE dba SUPERUSER');
ok_as('dba', 'appdb', "CREATE EXTENSION $P", 'dba creates the extension in appdb');
sql('ALTER ROLE dba NOSUPERUSER');
sql("CREATE EXTENSION $P");
is(sql(q{SELECT rolsuper::text || rolcreatedb::text || rolcreaterole::text
	  || pg_has_role('dba', 'pg_monitor', 'USAGE')::text
	  || pg_has_role('dba', 'pg_read_all_stats', 'USAGE')::text
	FROM pg_roles WHERE rolname = 'dba'}),
	'falsetruetruetruetrue',
	'dba: NOSUPERUSER, CREATEDB, CREATEROLE, pg_monitor (so pg_read_all_stats)');
is(sql(qq{SELECT string_agg(DISTINCT pg_get_userbyid(p.proowner), ',')
	FROM pg_depend d JOIN pg_proc p ON p.oid = d.objid
	WHERE d.classid = 'pg_proc'::regclass AND d.deptype = 'e'
	  AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = '$P')},
		'appdb'),
	'dba', 'in appdb the extension functions are owned by dba');

# A tagged statement of each of app, other and the superuser in appdb
# (untagged statements are not recorded: untagged = skip).
my $super = sql('SELECT current_user');
sub workload
{
	for my $r ('app', 'other', $super)
	{
		ok_as($r, 'appdb', "SELECT 42 /*controller:c_$r,action:show*/",
			"tagged statement of $r");
	}
}

my $restricted = "'$P' || '_reset()', '$P' || '_extract(text, int, int)'";
my $call_reset = "SELECT ${P}_reset()";
my $call_extract = "SELECT ${P}_extract('SELECT 1 /*controller:x*/') -> 'tags'";

# ------------------------------------------------- privilege statements
my %check = (
	'views-public' => sub {
		for my $o ("${P}", "${P}_totals", "${P}_last_bucket", "${P}_activity",
			"${P}_info()", "${P}_counters()")
		{
			ok_as('app', 'appdb', "SELECT count(*) FROM $o",
				"any role reads $o");
		}
	},
	'own-rows' => sub {
		workload();
		my $out = ok_as('app', 'appdb', qq{SELECT
			  count(*) FILTER (WHERE tags->>'controller' = 'c_app' AND queryid IS NOT NULL),
			  count(*) FILTER (WHERE userid <> 'app'::regrole AND tags IS NOT NULL),
			  count(*) FILTER (WHERE userid <> 'app'::regrole AND queryid IS NOT NULL),
			  count(*) FILTER (WHERE userid IN ('other'::regrole, '$super'::regrole)
			                     AND calls > 0)
			FROM ${P}_totals}, 'app reads the totals');
		is($out, '1|0|0|2',
			"app sees its own row in full, other roles' with queryid and tags NULL");
	},
	'read-all-stats' => sub {
		workload();
		my $out = ok_as('dba', 'appdb', qq{SELECT count(*) FROM ${P}_totals
			WHERE tags->>'controller' LIKE 'c\\_%' AND queryid IS NOT NULL},
			'dba reads the totals');
		is($out, '3', "dba (pg_monitor) sees every role's queryid and tags");
		$out = ok_as('monitoring', 'appdb', qq{SELECT count(*) FROM ${P}_totals
			WHERE tags->>'controller' LIKE 'c\\_%'}, 'monitoring reads the totals');
		is($out, '0', "monitoring, without pg_read_all_stats, sees no other role's tags");
	},
	'grant-read-all-stats' => sub {
		workload();
		if ($vnum >= 160000)
		{
			fails_as('dba', 'appdb', 'GRANT pg_read_all_stats TO monitoring',
				qr/permission denied to grant role "pg_read_all_stats"/,
				'PG16+: dba without ADMIN OPTION cannot grant pg_read_all_stats');
			sql('GRANT pg_read_all_stats TO dba WITH ADMIN OPTION');
			ok_as('dba', 'appdb', 'GRANT pg_read_all_stats TO monitoring',
				'PG16+: with ADMIN OPTION on it dba grants pg_read_all_stats');
		}
		else
		{
			ok_as('dba', 'appdb', 'GRANT pg_read_all_stats TO monitoring',
				'PG14-15: CREATEROLE is enough to grant pg_read_all_stats');
		}
		my $out = ok_as('monitoring', 'appdb', qq{SELECT count(*) FROM ${P}_totals
			WHERE tags->>'controller' LIKE 'c\\_%'}, 'monitoring reads the totals');
		is($out, '3', 'with pg_read_all_stats monitoring sees every role\'s tags');
		ok_as('dba', 'appdb', 'REVOKE pg_read_all_stats FROM monitoring', 'revoke');
		sql('REVOKE pg_read_all_stats FROM dba') if $vnum >= 160000;
	},
	'functions-owner' => sub {
		is(ok_as('dba', 'appdb', $call_extract, 'the owner calls _extract()'),
			'{"controller": "x"}', '_extract() works for its owner');
		ok_as('dba', 'appdb', $call_reset, 'the owner calls _reset()');
	},
	'functions-revoked' => sub {
		for my $call ($call_reset, $call_extract)
		{
			fails_as('app', 'appdb', $call, qr/permission denied for function/,
				"a role without EXECUTE cannot run: $call");
		}
		fails_as('dba', 'postgres', $call_reset, qr/permission denied for function/,
			'dba cannot call _reset() where a superuser created the extension');
		fails_as('dba', 'postgres', $call_extract, qr/permission denied for function/,
			'dba cannot call _extract() where a superuser created the extension');
		my ($rc, $out, $err) = run_as('dba', 'postgres',
			"GRANT EXECUTE ON FUNCTION ${P}_reset() TO monitoring");
		fails_as('monitoring', 'postgres', $call_reset, qr/permission denied for function/,
			'nor can dba grant EXECUTE on it there');
	},
	'grant-execute' => sub {
		my ($block) = grep { /GRANT EXECUTE ON FUNCTION/ }
		  ($guide =~ /^```sql\n(.*?)^```$/msg);
		ok(defined $block, 'the guide has a GRANT EXECUTE example') or return;
		like($block, qr/_reset\(\)/, 'it grants _reset()');
		like($block, qr/_extract\(text, int, int\)/, 'it grants _extract()');
		ok_as('dba', 'appdb', $block, 'the owner runs the GRANT EXECUTE example');
		is(ok_as('monitoring', 'appdb', $call_extract, 'the grantee calls _extract()'),
			'{"controller": "x"}', 'a granted role runs _extract()');
		ok_as('monitoring', 'appdb', $call_reset, 'a granted role runs _reset()');
		fails_as('app', 'appdb', $call_reset, qr/permission denied for function/,
			'other roles still cannot');
	},
	'suset-denied' => sub {
		for my $stmt ("SET $P.track = 'all'",
			"ALTER DATABASE appdb SET $P.track = 'all'",
			"ALTER ROLE app SET $P.track = 'all'",
			"ALTER ROLE app IN DATABASE appdb SET $P.tags = 'job'")
		{
			fails_as('dba', 'appdb', $stmt, qr/permission denied to set parameter/,
				"dba (not superuser): $stmt");
		}
		is(sql("SELECT count(*) FROM pg_settings WHERE name LIKE '$P.%' AND context = 'superuser'"),
			'8', 'eight settings are superuser-context');
	},
	'grant-set' => sub {
	  SKIP: {
			skip 'GRANT SET ON PARAMETER is PG15+', 6 if $vnum < 150000;
			my ($block) = grep { /GRANT SET ON PARAMETER/ }
			  ($guide =~ /^```sql\n(.*?)^```$/msg);
			ok(defined $block, 'the guide has a GRANT SET example') or return;
			sql($block);
			my ($param) = $block =~ /GRANT SET ON PARAMETER (\S+) TO dba/;
			is($param, "$P.untagged", 'the example grants SET on untagged to dba');
			is(ok_as('dba', 'appdb', "SET $P.untagged = 'record'; SHOW $P.untagged",
					'dba sets it per session'), 'record', 'per session');
			ok_as('dba', 'appdb', "ALTER DATABASE appdb SET $P.untagged = 'record'",
				'dba sets it per database');
			is(ok_as('app', 'appdb', "SHOW $P.untagged", 'new session'), 'record',
				'a new session of another role gets the per-database value');
			ok_as('dba', 'appdb', "ALTER DATABASE appdb RESET $P.untagged", 'reset');
			sql("REVOKE SET ON PARAMETER $P.untagged FROM dba");
		}
	},
	'userset' => sub {
		is(ok_as('app', 'appdb',
				"SET $P.tags_override = 'job=''nightly'''; SHOW $P.tags_override",
				'any role sets tags_override'),
			"job='nightly'", 'tags_override is set per session by any role');
		ok_as('dba', 'appdb', "ALTER ROLE app SET $P.tags_override = 'job=''x'''",
			'dba sets it for a role it created');
		ok_as('dba', 'appdb', "ALTER ROLE app RESET $P.tags_override", 'reset');
	},
	'sighup-not-settable' => sub {
		fails_as('dba', 'appdb', "SET $P.extractors = 'marginalia'",
			qr/cannot be changed now/, 'a sighup setting cannot be SET');
		fails_as('dba', 'appdb', "ALTER DATABASE appdb SET $P.normalize = ''",
			qr/cannot be changed now/, 'nor set per database');
		fails_as('dba', 'appdb', "SET $P.max_entries = 100",
			qr/cannot be changed without restarting the server/,
			'a postmaster setting cannot be SET');
	},
	'alter-system-denied' => sub {
		fails_as('dba', 'appdb', "ALTER SYSTEM SET $P.extractors = 'marginalia'",
			qr/(must be superuser|permission denied)/, 'dba cannot ALTER SYSTEM');
	},
	'reload-denied' => sub {
		fails_as('dba', 'appdb', 'SELECT pg_reload_conf()',
			qr/permission denied for function pg_reload_conf/,
			'dba cannot call pg_reload_conf()');
	},
	'settings-readable' => sub {
		my $all = sql("SELECT count(*) FROM pg_settings WHERE name LIKE '$P.%'");
		is(ok_as('app', 'appdb', qq{SELECT count(*) FROM pg_settings
				WHERE name LIKE '$P.%' AND setting IS NOT NULL
				  AND pending_restart IS NOT NULL},
				'app reads pg_settings'),
			$all, 'any role sees every setting in pg_settings');
		ok_as('app', 'appdb', "SHOW $P.extractors; SHOW $P.normalize; SHOW $P.tags",
			'any role can SHOW them');
		like($guide, qr/its settings exist \($all rows\)/,
			'the checklist gives the number of settings');
	},
	'pending-restart' => sub {
		sql("ALTER SYSTEM SET $P.max_entries = 200");
		sql('SELECT pg_reload_conf()');
		$node->poll_query_until('appdb', qq{SELECT pending_restart FROM pg_settings
			WHERE name = '$P.max_entries'}, 't')
		  or fail('pending_restart did not become true');
		is(ok_as('dba', 'appdb', qq{SELECT setting || ' ' || pending_restart::text
				FROM pg_settings WHERE name = '$P.max_entries'}, 'dba reads it'),
			'10000 true', 'a changed postmaster setting: old value, pending_restart');
		sql("ALTER SYSTEM RESET $P.max_entries");
		$node->restart;
	},
	'preload-visible' => sub {
		is(ok_as('dba', 'appdb', 'SHOW shared_preload_libraries', 'dba'), $P,
			'pg_monitor (pg_read_all_settings) shows shared_preload_libraries');
		fails_as('app', 'appdb', 'SHOW shared_preload_libraries',
			qr/pg_read_all_settings/, 'a plain role cannot');
	},
	'file-settings-denied' => sub {
		fails_as('dba', 'appdb', 'SELECT count(*) FROM pg_file_settings',
			qr/permission denied/, 'dba (pg_monitor) cannot read pg_file_settings');
	},
);

# Every HTML comment in the guide must be a well-formed marker, and the
# raw-example markers must pair up (open, close, open, close, ...), so a
# typo or a missing marker fails rather than silently dropping a check.
my (@marks, @opened, @closed, @marker_errors);
{
	my $open;
	for my $c ($guide =~ /<!--(.*?)-->/sg)
	{
		if ($c =~ /^ check: ([\w-]+) $/) { push @marks, $1; }
		elsif ($c =~ /^ raw-example: ([\w-]+) $/)
		{
			push @opened, $1;
			push @marker_errors, "raw-example $1 opened inside $open" if defined $open;
			$open = $1;
		}
		elsif ($c eq ' /raw-example ')
		{
			push @closed, $open // '(none)';
			push @marker_errors, 'closing raw-example marker without an opening one'
			  unless defined $open;
			undef $open;
		}
		else { push @marker_errors, "unrecognized comment <!--$c-->"; }
	}
	push @marker_errors, "raw-example $open is not closed" if defined $open;
}
is_deeply(\@marker_errors, [], 'every marker in the guide is well formed and paired');
my @all_checks = $guide =~ /<!--\s*check\b/g;
is(scalar(@marks), scalar(@all_checks), 'every check marker is well formed');
my %seen;
my @dups = grep { $seen{$_}++ } @marks;
is_deeply(\@dups, [], 'each check marker appears once in the guide');
is_deeply([ sort keys %seen ], [ sort keys %check ],
	'the guide marks exactly the privilege statements checked here');
my $checks_run = 0;
for my $name (@marks)
{
	note "check: $name";
	next unless $check{$name};
	$check{$name}->();
	$checks_run++;
}
is($checks_run, scalar(keys %check), 'every privilege check ran');

# ------------------------------------------------------- GUC contexts
my %ctx = map { split /\|/ } split /\n/,
  sql("SELECT substr(name, length('$P.') + 1) || '|' || context
		FROM pg_settings WHERE name LIKE '$P.%'");
my @guide_rows = $guide =~ /^\| \[`\w+`\]\(configuration\.md#\w+\) \|/mg;
my @guide_any = $guide =~ /^\| \[`[^`]*`\]\(configuration\.md/mg;
is(scalar(@guide_rows), scalar(keys %ctx), 'the guide has one context row per GUC');
is(scalar(@guide_any), scalar(@guide_rows), 'every context row in the guide is well formed');
my %guide_ctx = $guide =~ /^\| \[`(\w+)`\]\(configuration\.md#\w+\) \| (\w+) \|/mg;
is_deeply(\%guide_ctx, \%ctx, 'the guide gives every GUC its pg_settings context');
my @conf_rows = $config =~ /^\| \[`\w+`\]\(#\w+\) \|.*\| \w+ \|$/mg;
my @conf_any = $config =~ /^\| \[`[^`]*`\]\(#/mg;
is(scalar(@conf_rows), scalar(keys %ctx), 'configuration.md has one Reference row per GUC');
is(scalar(@conf_any), scalar(@conf_rows), 'every Reference row is well formed');
my %conf_ctx = $config =~ /^\| \[`(\w+)`\]\(#\w+\) \|.*\| (\w+) \|$/mg;
is_deeply(\%conf_ctx, \%ctx,
	'configuration.md gives every GUC its pg_settings context');

# --------------------------------------------------- raw parameter values
my @examples = $guide =~ /<!-- raw-example: ([\w-]+) -->\n(.*?)<!-- \/raw-example -->/msg;
my @want_examples = qw(normalize quoted-pattern svc-op);
is_deeply([ sort @opened ], \@want_examples, 'the guide opens each raw-value example');
is_deeply([ sort @closed ], \@want_examples, 'the guide closes each raw-value example');
is_deeply([ sort map { $examples[ 2 * $_ ] } 0 .. $#examples / 2 ],
	\@want_examples, 'every raw-value example is parsed');
my %exercised;
# Broken markers merge examples; running them would only wait on SHOW.
@examples = () if @marker_errors;
my $conf_file = $node->data_dir . '/postgresql.conf';
my $conf_orig = slurp_file($conf_file);

sub reload_until
{
	my ($want, $what) = @_;
	sql('SELECT pg_reload_conf()');
	for my $name (sort keys %$want)
	{
		$node->poll_query_until('appdb', "SHOW $name", $want->{$name})
		  or fail("$what: SHOW $name did not become $want->{$name}");
	}
}

sub reset_all
{
	my ($raw) = @_;
	sql("ALTER SYSTEM RESET $_") for keys %$raw;
	sql('SELECT pg_reload_conf()');
	for my $name (keys %$raw)
	{
		my $d = sql("SELECT boot_val FROM pg_settings WHERE name = '$name'");
		$node->poll_query_until('appdb', "SHOW $name", $d)
		  or die "$name did not return to its default";
	}
}

while (my ($name, $body) = splice(@examples, 0, 2))
{
	my %raw = $body =~ /^`($P\.\w+)`:\n\n```text\n(.*?)\n```$/mg;
	ok(%raw, "$name: has raw values") or next;
	my @blocks = $body =~ /^```(\w+)\n(.*?)^```$/msg;
	my (@sqlform, @conf, @checks);
	while (my ($lang, $text) = splice(@blocks, 0, 2))
	{
		push @sqlform, $text if $lang eq 'sql' && $text =~ /ALTER SYSTEM/;
		push @conf, $text if $lang eq 'ini';
		push @checks, $text if $lang eq 'sql' && $text =~ /_extract\(/;
	}
	is(scalar(@sqlform), 1, "$name: one SQL form");
	is(scalar(@conf), 1, "$name: one postgresql.conf form");
	is(scalar(@checks), 1, "$name: one _extract() check");
	my ($query, $expected) = $checks[0] =~ /^(SELECT.*?);\n-- (\{.*\})$/ms
	  or (fail("$name: check query and expected output"), next);
	isnt($expected, '{}', "$name: the expected tags are not empty");
	is(ok_as('dba', 'appdb', "SELECT ($query) = '$expected'::jsonb",
			"$name: check under the defaults"), 'f',
		"$name: the defaults do not give the documented tags");

	# The raw value, quoted for SQL by doubling its quotes.
	for my $guc (sort keys %raw)
	{
		(my $lit = $raw{$guc}) =~ s/'/''/g;
		sql("ALTER SYSTEM SET $guc = '$lit'");
	}
	reload_until(\%raw, "$name raw");
	my $got = ok_as('dba', 'appdb', $query, "$name: check query runs");
	is(sql("SELECT '$got'::jsonb = '$expected'::jsonb"), 't',
		"$name: the raw values give the documented tags")
	  or diag("got $got, expected $expected");
	reset_all(\%raw);

	# The SQL form.
	sql($sqlform[0]);
	reload_until(\%raw, "$name SQL form");
	is(sql('SELECT ' . join(' || chr(10) || ', map { "current_setting('$_')" } sort keys %raw)),
		join("\n", map { $raw{$_} } sort keys %raw),
		"$name: the SQL form sets the raw values");
	reset_all(\%raw);

	# The postgresql.conf form.
	$node->append_conf('postgresql.conf', $conf[0]);
	reload_until(\%raw, "$name conf form");
	is(sql('SELECT ' . join(' || chr(10) || ', map { "current_setting('$_')" } sort keys %raw)),
		join("\n", map { $raw{$_} } sort keys %raw),
		"$name: the postgresql.conf form sets the raw values");
	open my $fh, '>', $conf_file or die "$conf_file: $!";
	print $fh $conf_orig;
	close $fh;
	reset_all(\%raw);
	$exercised{$name} = 1;
}
is_deeply([ sort keys %exercised ], \@want_examples,
	'every raw-value example ran to the end');

# ------------------------------------------------ troubleshooting checklist
my ($trouble) = $guide =~ /^## Troubleshooting\b.*?\n(.*?)(?=^## |\z)/ms;
ok(defined $trouble, 'the guide has a troubleshooting section');
# Blocks may be indented under list items.
my @tsql = map { s/^ +//mgr } ($trouble // '') =~ /^ *```sql\n(.*?)^ *```$/msg;
my @fences = ($trouble // '') =~ /^ *```(\w*) *$/mg;
my @fence_errors;
for (my $i = 0; $i < @fences; $i += 2)
{
	push @fence_errors, "fence $i opens without a language" if $fences[$i] eq '';
	push @fence_errors, "fence $i is not closed"
	  unless defined $fences[ $i + 1 ] && $fences[ $i + 1 ] eq '';
}
is_deeply(\@fence_errors, [], 'the checklist code blocks are paired');
is(scalar(@tsql), scalar(grep { $_ eq 'sql' } @fences),
	'every SQL block of the checklist is parsed');
my @symptoms = ($trouble // '') =~ /^### (.*)$/mg;
is_deeply(\@symptoms,
	[ 'The views are empty', 'My utility statements are missing',
		q{My settings change didn't apply}, 'Statistics vanished after a restart' ],
	'the checklist covers the four symptoms');
for my $sym (split /^(?=### )/m, $trouble // '')
{
	next unless $sym =~ /^### (.*)$/m;
	my $title = $1;
	ok($sym =~ /^ *```sql\n/m, "symptom has SQL: $title");
}
ok(@tsql >= 8, 'the checklist has SQL for each symptom');
my $tsql_run = 0;
for my $q (@tsql)
{
	if ($q =~ /pg_file_settings/)
	{
		fails_as('dba', 'appdb', $q, qr/permission denied/,
			'pg_file_settings needs more than pg_monitor');
		ok($node->psql('appdb', $q) == 0, 'a superuser reads pg_file_settings');
	}
	else
	{
		(my $first) = $q =~ /^(.*)$/m;
		ok_as('dba', 'appdb', $q, "checklist query runs as dba: $first");
	}
	$tsql_run++;
}
is($tsql_run, scalar(@tsql), 'every checklist query ran');
for my $needle ('SHOW', 'pg_settings', 'pg_file_settings', 'regex_compile_failures',
	'utility_missing_queryid', "${P}_counters()", 'pending_restart',
	'stats_reset', 'server log', 'is loaded after pg_stat_statement_context',
	'plan cache')
{
	like($trouble // '', qr/\Q$needle\E/, "the checklist mentions $needle");
}

# ------------------------------------------------------- relative links
{
	my $r = check_relative_links($root);
	ok($r->{nlinks} > 100, "relative links found ($r->{nlinks})");
	is_deeply($r->{bad}, [], 'every relative link resolves')
	  or diag(join("\n", @{ $r->{bad} }));
	note("$r->{nomitted} links into paths left out of release tarballs not checked")
	  if $r->{nomitted};
}

$node->stop;
done_testing();
