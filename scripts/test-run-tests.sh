#!/bin/bash
# Self-test of docker/run-tests.sh as run on the host (the macOS CI cells).
# Needs a PostgreSQL installation (pg_config, initdb, pg_ctl, psql) on PATH.
# Builds nothing: `make` is stubbed out (and fails for installcheck), so each
# harness run fails with a server running: at "LOAD without
# shared_preload_libraries" if the extension is not installed, else at
# "make installcheck".
#   1. A source tree that contains worktrees/ (other checkouts, with their
#      own src/compat.h) passes the version-guard step.
#   2. After that failure, no postmaster is left running in $PSSC_WORK.
# Uses PGPORT (default 55190) and scratch space under tmp/test-run-tests/.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$ROOT/tmp/test-run-tests
export PGPORT=${PGPORT:-55190}
PGBIN=$(pg_config --bindir)
fails=0

ok() { echo "ok - $*"; }
not_ok() { echo "not ok - $*"; fails=1; }
stop_server() { "$PGBIN/pg_ctl" -D "$1/pssc-data" -m immediate -w stop >/dev/null 2>&1 || true; }

rm -rf "$T"
mkdir -p "$T/stub"
printf '#!/bin/sh\ncase " $* " in *" installcheck "*) exit 1 ;; esac\n' > "$T/stub/make"
chmod 755 "$T/stub/make"

# run_harness NAME: run docker/run-tests.sh on $T/NAME/src; sets $out and $rc.
run_harness() {
	local d=$T/$1
	trap 'stop_server "$d/work"' RETURN
	rc=0
	out=$(PATH="$T/stub:$PATH" PSSC_SRC="$d/src" PSSC_OUT="$d/out" \
		PSSC_BUILD="$d/build" PSSC_WORK="$d/work" PSSC_TEST_MODE=release \
		"$d/src/docker/run-tests.sh" 2>&1) || rc=$?
	echo "$out" > "$d/output.log"
	# What the harness left behind, before the trap cleans up.
	server=stopped
	if "$PGBIN/pg_ctl" -D "$d/work/pssc-data" status >/dev/null 2>&1; then server=running; fi
}

copy_tree() {
	mkdir -p "$1"
	git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 tar cf -) | tar -C "$1" -xf -
}

# 1. worktrees/ in the source tree.
copy_tree "$T/worktrees/src"
w=$T/worktrees/src/worktrees/item
mkdir -p "$w/src"
cp "$ROOT/src/compat.h" "$w/src/compat.h"
printf '#if PG_VERSION_NUM >= 160000\n#endif\n' > "$w/src/unguarded.c"
run_harness worktrees
if echo "$out" | grep -q '^version guards ok$' && ! echo "$out" | grep -q 'FAIL: version-guard'; then
	ok "a source tree with worktrees/ passes the version-guard step"
else
	not_ok "a source tree with worktrees/ passes the version-guard step (see $T/worktrees/output.log)"
fi
if [ -d "$T/worktrees/build/worktrees" ]; then
	not_ok "worktrees/ is not copied to PSSC_BUILD"
else
	ok "worktrees/ is not copied to PSSC_BUILD"
fi

# 2. A failing run with a server up.
copy_tree "$T/fail/src"
run_harness fail
if [ $rc -ne 0 ] && echo "$out" | grep -Eq '^FAIL: (LOAD without preload|make installcheck)$' &&
	grep -q 'database system is ready' "$T/fail/work/nopreload.log" 2>/dev/null; then
	ok "the harness failed with a server running"
else
	not_ok "the harness failed with a server running (exit $rc; see $T/fail/output.log)"
fi
if [ "$server" = stopped ]; then
	ok "no postmaster is left running after a failed run"
else
	not_ok "no postmaster is left running after a failed run"
fi

[ $fails -eq 0 ] && echo "run-tests.sh self-test passed"
exit $fails
