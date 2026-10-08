#!/bin/bash
# Compile-only test of the upper version guard in src/compat.h (backlog
# 20261008-065635-3): a server newer than the newest validated major
# (PostgreSQL 18) fails the build with a clear #error, unless
# -DPSSC_ALLOW_UNTESTED_PG is given. The headers of the installed server
# (pg_config on PATH) are preprocessed with PG_VERSION_NUM faked after
# postgres.h; the real version must preprocess cleanly too.
# Usage: scripts/check-compat-guard.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/tmp/compat-guard
rm -rf "$D"
mkdir -p "$D"
trap 'rm -rf "$D"' EXIT

# pg_config --cc can carry flags (e.g. "gcc -std=gnu99").
read -r -a CC <<< "$(pg_config --cc)"
read -r -a CPPFLAGS <<< "$(pg_config --cppflags)"
INC=(-I"$ROOT/src" -I"$(pg_config --includedir-server)")
fails=0

# probe NAME FAKE_VERSION_NUM|"" [extra cc args...]: preprocesses compat.h;
# sets $out and $rc.
probe() {
	local name=$1 fake=$2
	shift 2
	{
		echo '#include "postgres.h"'
		if [ -n "$fake" ]; then
			echo '#undef PG_VERSION_NUM'
			echo "#define PG_VERSION_NUM $fake"
		fi
		echo '#include "compat.h"'
	} > "$D/$name.c"
	rc=0
	out=$("${CC[@]}" -E "${CPPFLAGS[@]}" "${INC[@]}" "$@" "$D/$name.c" -o "$D/$name.i" 2>&1) || rc=$?
}
ok() { echo "ok - $*"; }
not_ok() { echo "not ok - $*"; fails=1; }

probe real ""
if [ $rc -eq 0 ]; then ok "PostgreSQL $(pg_config --version | awk '{print $2}'): compat.h preprocesses"
else not_ok "the installed server's version is accepted: $out"; fi

probe pg19 190000
if [ $rc -ne 0 ] && grep -q 'not yet validated on PostgreSQL 19' <<< "$out"; then
	ok "PG_VERSION_NUM 190000: #error \"not yet validated on PostgreSQL 19\""
else
	not_ok "PG_VERSION_NUM 190000 is rejected with a clear message (exit $rc): $out"
fi

probe pg20 200000
if [ $rc -ne 0 ] && grep -q 'not yet validated on PostgreSQL 19' <<< "$out"; then
	ok "PG_VERSION_NUM 200000: rejected too"
else
	not_ok "PG_VERSION_NUM 200000 is rejected (exit $rc): $out"
fi

# With a faked version the newer majors' branches of compat.h include headers
# that older servers lack, so only PostgreSQL 18 headers must preprocess
# cleanly; on every server the #error must be gone.
probe pg19_allowed 190000 -DPSSC_ALLOW_UNTESTED_PG
if grep -q 'not yet validated' <<< "$out"; then
	not_ok "-DPSSC_ALLOW_UNTESTED_PG overrides the guard: $out"
elif [ $rc -ne 0 ] && [ "$(pg_config --version | sed 's/^PostgreSQL \([0-9]*\).*/\1/')" -ge 18 ]; then
	not_ok "-DPSSC_ALLOW_UNTESTED_PG: compat.h preprocesses with PostgreSQL 18+ headers: $out"
else
	ok "-DPSSC_ALLOW_UNTESTED_PG overrides the guard"
fi

[ $fails -eq 0 ] && echo "compat.h version-guard check passed"
exit $fails
