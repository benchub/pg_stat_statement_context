#!/bin/bash
# Runs inside the docker/Dockerfile image. Expects the repo mounted read-only
# at /src and a writable /out for logs and regression diffs.
set -euo pipefail

EXT=pg_stat_statement_context
BUILD=/build
PGDATA_DIR=/var/lib/postgresql/pssc-data
PGBIN=$(pg_config --bindir)
OUT=/out

step() { printf '\n=== %s\n' "$*"; }
fail() {
	echo "FAIL: $*" >&2
	cp -f "$BUILD"/regression.diffs "$BUILD"/regression.out "$OUT"/ 2>/dev/null || true
	cp -rf "$BUILD"/tmp_check/log "$OUT"/tap-log 2>/dev/null || true
	cp -f /var/lib/postgresql/*.log "$OUT"/ 2>/dev/null || true
	exit 1
}
as_pg() { gosu postgres "$@"; }
pg_start() { as_pg "$PGBIN/pg_ctl" -D "$PGDATA_DIR" -l "/var/lib/postgresql/$1.log" -w start >/dev/null; }
pg_stop() { as_pg "$PGBIN/pg_ctl" -D "$PGDATA_DIR" -m fast -w stop >/dev/null; }

rm -rf "$OUT"/* 2>/dev/null || true
echo "PostgreSQL: $(pg_config --version)"

step "copy sources"
rm -rf "$BUILD"
mkdir -p "$BUILD"
# Skip host build/test output so artifacts from another PG version (or the
# host OS) are never reused; make clean below is a second safeguard.
tar -C /src --exclude=./.git --exclude=./tmp \
	--exclude='*.o' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' \
	--exclude='*.dSYM' --exclude=./results --exclude=./tmp_check \
	--exclude=./log --exclude=./regression.diffs --exclude=./regression.out \
	--exclude=./test/unit/test_scan --exclude=./test/unit/test_scan_checked \
	--exclude=./test/unit/test_stmt --exclude=./test/unit/test_stmt_checked \
	--exclude=./test/unit/corpus \
	-cf - . | tar -C "$BUILD" -xf -
chown -R postgres:postgres "$BUILD"
cd "$BUILD"
as_pg make clean >/dev/null
as_pg make -C test/modules/pssc_compat_test clean >/dev/null

step "version-guard check"
scripts/check-version-guards.sh --self-test >/dev/null || fail "version-guard self-test"
scripts/check-version-guards.sh || fail "version-guard check"

step "unit tests (src/scan.c lexer + statement scans, ASan/UBSan)"
as_pg make unittest || fail "unit tests"

step "make"
as_pg make PG_CFLAGS="-Werror" || fail "make"
step "make install"
make install || fail "make install"

step "make install-test-modules (TEST-ONLY compat.h exerciser)"
as_pg make -C test/modules/pssc_compat_test PG_CFLAGS="-Werror" || fail "make test modules"
make -C test/modules/pssc_compat_test install || fail "install test modules"

step "initdb"
rm -rf "$PGDATA_DIR"
as_pg "$PGBIN/initdb" -D "$PGDATA_DIR" --no-sync -A trust >/dev/null || fail "initdb"

step "LOAD without shared_preload_libraries"
pg_start nopreload || fail "start without preload"
res=$(as_pg "$PGBIN/psql" -X -At -v ON_ERROR_STOP=1 -d postgres \
	-c "LOAD '$EXT'" -c "SELECT 42") || fail "LOAD without preload"
[ "$(echo "$res" | tail -n1)" = "42" ] || fail "session did not survive LOAD (got: $res)"
as_pg "$PGBIN/psql" -X -At -d postgres -c "SELECT 1" >/dev/null || fail "server down after LOAD"
pg_stop
if grep -E "PANIC|terminated by signal" /var/lib/postgresql/nopreload.log; then
	fail "crash in no-preload log"
fi
echo "ok"

step "start with shared_preload_libraries = '$EXT'"
echo "shared_preload_libraries = '$EXT'" >> "$PGDATA_DIR/postgresql.conf"
pg_start preload || fail "start with preload"
if grep -E "ERROR|FATAL|PANIC|WARNING" /var/lib/postgresql/preload.log; then
	fail "preloaded server log not clean"
fi
echo "ok"

step "make installcheck"
as_pg make installcheck || fail "make installcheck"

pg_stop
step "ALL PASSED ($(pg_config --version))"
