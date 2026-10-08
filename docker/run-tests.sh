#!/bin/bash
# Runs inside the docker/Dockerfile and docker/Dockerfile.source images
# (scripts/docker-test.sh), and directly on the host in the macOS CI cells.
# Expects the repo at $PSSC_SRC (read-only is fine) and a writable $PSSC_OUT
# for logs and regression diffs. pg_config on PATH selects the server.
#
# Environment:
#   PSSC_SRC, PSSC_BUILD, PSSC_OUT, PSSC_WORK
#       repo, build copy, log output, and cluster/log directory
#       (default /src, /build, /out, /var/lib/postgresql). As root, builds and
#       servers run as the postgres user (gosu); otherwise as the caller.
#   PSSC_TEST_MODE
#       unset/pgdg/release  everything: unit tests, build, installcheck + TAP
#       assert    as release, and checks the server has debug_assertions = on
#       valgrind  runs the server under Valgrind with PostgreSQL's
#                 valgrind.supp for the LOAD checks and the pg_regress suite;
#                 TAP tests are skipped (they start their own clusters). Fails
#                 on any Valgrind error.
set -euo pipefail

EXT=pg_stat_statement_context
SRC=${PSSC_SRC:-/src}
BUILD=${PSSC_BUILD:-/build}
OUT=${PSSC_OUT:-/out}
WORK=${PSSC_WORK:-/var/lib/postgresql}
MODE=${PSSC_TEST_MODE:-pgdg}
CRASH_RE="PANIC|terminated by signal"
PGDATA_DIR=$WORK/pssc-data
VGLOG=$WORK/valgrind
PGBIN=$(pg_config --bindir)

step() { printf '\n=== %s\n' "$*"; }
fail() {
	echo "FAIL: $*" >&2
	stop_server
	cp -f "$BUILD"/regression.diffs "$BUILD"/regression.out "$OUT"/ 2>/dev/null || true
	cp -rf "$BUILD"/results "$OUT"/ 2>/dev/null || true
	cp -rf "$BUILD"/tmp_check/log "$OUT"/tap-log 2>/dev/null || true
	cp -f "$WORK"/*.log "$OUT"/ 2>/dev/null || true
	cp -rf "$VGLOG" "$OUT"/ 2>/dev/null || true
	exit 1
}
# As root (in Docker), the copies in $OUT are root's, and the server and TAP
# logs keep pg_ctl's 0600: give them to the owner of the bind-mounted $OUT
# (the host user) so the host and CI can read them.
own_out() {
	[ "$(id -u)" = 0 ] && [ -d "$OUT" ] || return 0
	chown -R "$(stat -c %u:%g "$OUT")" "$OUT" && chmod -R u+rwX "$OUT" || true
}
if [ "$(id -u)" = 0 ]; then
	as_pg() { gosu postgres "$@"; }
else
	as_pg() { "$@"; }
fi
pg_start() { as_pg "$PGBIN/pg_ctl" -D "$PGDATA_DIR" -l "$WORK/$1.log" -w start >/dev/null; }
pg_stop() { as_pg "$PGBIN/pg_ctl" -D "$PGDATA_DIR" -m fast -w stop >/dev/null; }
# On any failure or early exit. In Docker the server dies with the container,
# but on the host it would stay up on PGPORT and break the next run.
stop_server() {
	[ -f "$PGDATA_DIR/postmaster.pid" ] || return 0
	as_pg "$PGBIN/pg_ctl" -D "$PGDATA_DIR" -m immediate -w stop >/dev/null 2>&1 || true
}
trap 'stop_server; own_out' EXIT

case $MODE in
pgdg | release | assert | valgrind) ;;
*) echo "unknown PSSC_TEST_MODE '$MODE'" >&2; exit 2 ;;
esac

mkdir -p "$OUT" "$WORK"
rm -rf "${OUT:?}"/* 2>/dev/null || true
echo "PostgreSQL: $(pg_config --version) (mode: $MODE)"

step "copy sources"
rm -rf "$BUILD"
mkdir -p "$BUILD"
# Skip host build/test output so artifacts from another PG version (or the
# host OS) are never reused; make clean below is a second safeguard. Skip
# worktrees/ (other checkouts of the repo, CLAUDE.md §7) too.
tar -C "$SRC" --exclude=./.git --exclude=./tmp --exclude=./worktrees \
	--exclude='*.o' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' \
	--exclude='*.dSYM' --exclude=./results --exclude=./tmp_check \
	--exclude=./log --exclude=./regression.diffs --exclude=./regression.out \
	--exclude=./test/unit/test_scan --exclude=./test/unit/test_scan_checked \
	--exclude=./test/unit/test_stmt --exclude=./test/unit/test_stmt_checked \
	--exclude=./test/unit/test_pairs --exclude=./test/unit/test_pairs_checked \
	--exclude=./test/unit/test_tagset --exclude=./test/unit/test_tagset_checked \
	--exclude=./test/unit/test_counters \
	--exclude=./test/unit/corpus \
	--exclude=./fuzz/fuzz_scan --exclude=./fuzz/fuzz_tagset \
	--exclude=./fuzz/fuzz_sqlcommenter --exclude=./fuzz/fuzz_marginalia \
	--exclude='./fuzz/*_standalone' --exclude=./fuzz/corpus \
	-cf - . | tar -C "$BUILD" -xf -
if [ "$(id -u)" = 0 ]; then chown -R postgres:postgres "$BUILD"; fi
cd "$BUILD"
as_pg make clean >/dev/null
for m in test/modules/*/; do as_pg make -C "$m" clean >/dev/null; done

step "version-guard check"
scripts/check-version-guards.sh --self-test >/dev/null || fail "version-guard self-test"
scripts/check-version-guards.sh || fail "version-guard check"

step "frozen SQL check"
scripts/check-frozen-sql.sh --self-test >/dev/null || fail "frozen-sql self-test"
scripts/check-frozen-sql.sh || fail "frozen-sql check"

step "unit tests (src/scan.c lexer + statement scans, src/pairs.c parsers, src/tagset.c pipeline, src/counters.c slot + fuzz entry points, ASan/UBSan)"
as_pg make unittest || fail "unit tests"

step "make"
as_pg make PG_CFLAGS="-Werror" || fail "make"
step "make install"
make install || fail "make install"

step "make install-test-modules (TEST-ONLY compat.h exerciser, GUC inspector, extract and store drivers, context hooks)"
for m in test/modules/*/; do
	as_pg make -C "$m" PG_CFLAGS="-Werror" || fail "make test module $m"
	make -C "$m" install || fail "install test module $m"
done

step "initdb"
rm -rf "$PGDATA_DIR"
as_pg "$PGBIN/initdb" -D "$PGDATA_DIR" --no-sync -A trust >/dev/null || fail "initdb"

if [ "$MODE" = valgrind ]; then
	step "run the server under Valgrind"
	command -v valgrind >/dev/null || fail "valgrind not installed"
	SUPP="$(pg_config --sharedir)/valgrind.supp"
	[ -f "$SUPP" ] || fail "missing $SUPP (docker/build-postgres.sh valgrind flavor installs it)"
	pg_config --cppflags | grep -q -- -DUSE_VALGRIND || fail "server not built with -DUSE_VALGRIND"
	rm -rf "$VGLOG"
	mkdir -p "$VGLOG"
	[ "$(id -u)" = 0 ] && chown postgres:postgres "$VGLOG"
	# initdb ran without Valgrind (bootstrap under it is very slow).
	[ -e "$PGBIN/postgres.orig" ] || mv "$PGBIN/postgres" "$PGBIN/postgres.orig"
	cat > "$PGBIN/postgres" <<-WRAPPER
	#!/bin/sh
	exec valgrind --quiet --trace-children=yes --track-origins=yes \\
	  --read-var-info=no --num-callers=40 --leak-check=no --error-limit=no \\
	  --gen-suppressions=all --suppressions="$SUPP" \\
	  --error-markers=VALGRINDERROR-BEGIN,VALGRINDERROR-END --error-exitcode=128 \\
	  --log-file="$VGLOG/%p.log" "$PGBIN/postgres.orig" "\$@"
	WRAPPER
	chmod 755 "$PGBIN/postgres"
	export PGCTLTIMEOUT=600
	# A process exiting through --error-exitcode is a crash too.
	CRASH_RE="$CRASH_RE|exited with exit code 128"
	valgrind_check() {
		local n bad
		n=$(find "$VGLOG" -name '*.log' | wc -l)
		# Non-vacuous: the postmaster and backends really ran under Valgrind.
		[ "$n" -ge 3 ] || fail "only $n Valgrind log files: server not under Valgrind?"
		bad=$(grep -l VALGRINDERROR-BEGIN "$VGLOG"/*.log || true)
		if [ -n "$bad" ]; then
			for f in $bad; do echo "--- $f"; cat "$f"; done
			fail "Valgrind reported errors in $(echo "$bad" | wc -l) process(es)"
		fi
		echo "Valgrind: no errors in $n processes ($1)"
	}
fi

step "LOAD without shared_preload_libraries"
pg_start nopreload || fail "start without preload"
res=$(as_pg "$PGBIN/psql" -X -At -v ON_ERROR_STOP=1 -d postgres \
	-c "LOAD '$EXT'" -c "SELECT 42") || fail "LOAD without preload"
[ "$(echo "$res" | tail -n1)" = "42" ] || fail "session did not survive LOAD (got: $res)"
as_pg "$PGBIN/psql" -X -At -d postgres -c "SELECT 1" >/dev/null || fail "server down after LOAD"
pg_stop
if grep -E "$CRASH_RE" "$WORK/nopreload.log"; then
	fail "crash in no-preload log"
fi
[ "$MODE" = valgrind ] && valgrind_check "LOAD without preload"
echo "ok"

step "start with shared_preload_libraries = '$EXT'"
echo "shared_preload_libraries = '$EXT'" >> "$PGDATA_DIR/postgresql.conf"
pg_start preload || fail "start with preload"
if grep -E "ERROR|FATAL|PANIC|WARNING" "$WORK/preload.log"; then
	fail "preloaded server log not clean"
fi
if [ "$MODE" = assert ] || [ "$MODE" = valgrind ]; then
	[ "$(as_pg "$PGBIN/psql" -X -At -d postgres -c 'SHOW debug_assertions')" = on ] \
		|| fail "server not built with --enable-cassert"
	echo "debug_assertions = on"
fi
echo "ok"

if [ "$MODE" = valgrind ]; then
	step "make installcheck (pg_regress suite, server under Valgrind; TAP skipped)"
	as_pg make installcheck TAP_TESTS= || fail "make installcheck"
	pg_stop
	valgrind_check "LOAD + pg_regress suite"
	if grep -E "$CRASH_RE" "$WORK/preload.log"; then
		fail "backend crash or Valgrind error exit in server log"
	fi
else
	# The TAP tests use the PG15+ module names. PG14 ships them as
	# aliases of PostgresNode/TestLib from 14.3 but only installs them from 14.6.
	tapdir="$(dirname "$(pg_config --pgxs)")/../../src/test/perl"
	[ -f "$tapdir/PostgreSQL/Test/Cluster.pm" ] && [ -f "$tapdir/PostgreSQL/Test/Utils.pm" ] \
		|| fail "TAP tests need PostgreSQL::Test::Cluster/Utils in $tapdir (PG 14.6+)"
	step "make installcheck"
	# Every harness server (PGDG images, docker/build-postgres.sh builds)
	# installs pg_stat_statements: the TAP tests fail rather than skip
	# their pgss parity checks if it is missing (test/perl/PsscTest.pm).
	# Likewise auto_explain (contrib), and the third-party modules the PGDG
	# image installed (docker/Dockerfile) for test/t/036_hook_coexistence.pl.
	require_modules=auto_explain
	if [ -r /usr/local/share/pssc-test-modules ]; then
		require_modules=$(cat /usr/local/share/pssc-test-modules)
	fi
	echo "modules the coexistence test requires: $require_modules"
	as_pg env PSSC_REQUIRE_PGSS=1 PSSC_REQUIRE_MODULES="$require_modules" \
		make installcheck || fail "make installcheck"
	pg_stop
fi
step "ALL PASSED ($(pg_config --version), mode: $MODE)"
