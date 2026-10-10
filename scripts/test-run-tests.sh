#!/bin/bash
# Self-test of docker/run-tests.sh as run on the host (the macOS CI cells).
# Needs a PostgreSQL installation (pg_config, initdb, pg_ctl, psql) on PATH.
# Builds nothing: `make` is stubbed out (and fails for installcheck), so each
# harness run fails with a server running: at "LOAD without
# shared_preload_libraries" if the extension is not installed, else at
# "make installcheck".
#   1. A source tree that contains worktrees/ (other checkouts, with their
#      own src/compat.h) passes the version-guard step. The copy to
#      $PSSC_BUILD skips worktrees/ and the top-level build output, but not
#      nested directories of the same names (bench/results/).
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
for d in bench/results x/log x/tmp x/tmp_check x/worktrees results log tmp_check; do
	mkdir -p "$T/worktrees/src/$d" && touch "$T/worktrees/src/$d/.keep"
done
touch "$T/worktrees/src/regression.diffs"
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
# The top-level excludes are anchored: bsdtar (macOS) matched --exclude=./results
# against any path component and dropped bench/results/.
missing=
for f in bench/results/.keep x/log/.keep x/tmp/.keep x/tmp_check/.keep x/worktrees/.keep; do
	[ -f "$T/worktrees/build/$f" ] || missing="$missing $f"
done
if [ -z "$missing" ]; then
	ok "nested results/, log/, tmp/, tmp_check/ and worktrees/ are copied"
else
	not_ok "nested results/, log/, tmp/, tmp_check/ and worktrees/ are copied (missing:$missing)"
fi
leaked=
for f in results log tmp_check regression.diffs; do
	[ -e "$T/worktrees/build/$f" ] && leaked="$leaked $f"
done
if [ -z "$leaked" ]; then
	ok "top-level results/, log/, tmp_check/ and regression.diffs are not copied"
else
	not_ok "top-level results/, log/, tmp_check/ and regression.diffs are not copied (copied:$leaked)"
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
