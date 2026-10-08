#!/bin/bash
# Checks the symbols the release build of the library exports (backlog
# 20261008-065635-3, DESIGN.md §9). The release variant (plain "make", no
# PSSC_TESTING) must export only what PostgreSQL looks up: _PG_init,
# Pg_magic_func, the SQL-callable functions and their pg_finfo_* records,
# and the reclaim worker's entry point. The allowlist is
# test/release-exports.txt; any symbol outside it, or any test-hook symbol
# (the testing build's pssc_*_test_*, *_test_hook, pssc_*_debug_*), fails.
#
# The library is built from a copy of src/, sql/, the Makefile and the
# control file under tmp/release-exports/, so the caller's build is not
# touched. pg_config on PATH selects the server.
#
# Usage:
#   scripts/check-release-exports.sh               build the release variant, check it
#   scripts/check-release-exports.sh --lib FILE    check an already built library
#   scripts/check-release-exports.sh --self-test   build the testing variant
#                                                  (make PSSC_TESTING=1) and
#                                                  check that it is rejected
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALLOW=$ROOT/test/release-exports.txt
EXT=pg_stat_statement_context
HOOK_RE='_test_|_test$|^pssc_.*_debug_'

# check_lib LIB: 0 if LIB exports exactly a subset of the allowlist (and the
# entry points PostgreSQL needs), else 1 with the offending symbols listed.
check_lib() {
	local lib=$1 syms allowed extra hooks missing s status=0
	syms=$("$ROOT/scripts/list-exports.sh" "$lib")
	allowed=$(grep -v -e '^#' -e '^[[:space:]]*$' "$ALLOW" | sort -u)
	extra=$(comm -23 <(echo "$syms") <(echo "$allowed"))
	hooks=$(echo "$syms" | grep -E "$HOOK_RE" || true)
	if [ -n "$extra" ]; then
		echo "symbols exported by $lib but not in test/release-exports.txt:"
		echo "$extra" | sed 's/^/  /'
		status=1
	fi
	if [ -n "$hooks" ]; then
		echo "test-hook symbols exported by $lib:"
		echo "$hooks" | sed 's/^/  /'
		status=1
	fi
	# Non-vacuous: what PostgreSQL looks up is really there.
	for s in _PG_init Pg_magic_func pssc_reclaim_worker_main \
		pg_finfo_pg_stat_statement_context_1_0 pg_stat_statement_context_1_0; do
		if ! echo "$syms" | grep -qx -- "$s"; then
			echo "$lib does not export $s"
			status=1
		fi
	done
	missing=$(comm -13 <(echo "$syms") <(echo "$allowed") |
		grep -E '^(_PG_init|Pg_magic_func|pg_finfo_.*|pg_stat_statement_context_.*|pssc_.*)$' || true)
	if [ -n "$missing" ]; then
		echo "symbols in test/release-exports.txt that $lib does not export:"
		echo "$missing" | sed 's/^/  /'
		status=1
	fi
	[ $status -eq 0 ] && echo "$lib: $(echo "$syms" | wc -l | tr -d ' ') exported symbols, all allowed"
	return $status
}

# build VARIANT_ARGS...: builds a fresh copy; prints the library path.
build() {
	local d=$ROOT/tmp/release-exports lib
	rm -rf "$d"
	mkdir -p "$d/src"
	cp "$ROOT"/src/*.c "$ROOT"/src/*.h "$d/src/"
	cp -R "$ROOT/sql" "$d/"
	cp "$ROOT/Makefile" "$ROOT/$EXT.control" "$d/"
	make -C "$d" -s "$@" >&2
	for lib in "$d/$EXT.so" "$d/$EXT.dylib"; do
		[ -f "$lib" ] && { echo "$lib"; return 0; }
	done
	echo "no $EXT library built in $d" >&2
	return 1
}

case "${1:-}" in
"")
	lib=$(build PG_CFLAGS=-Werror)
	check_lib "$lib"
	;;
--lib)
	[ $# -eq 2 ] || { echo "usage: $0 --lib FILE" >&2; exit 2; }
	check_lib "$2"
	;;
--self-test)
	lib=$(build PG_CFLAGS=-Werror PSSC_TESTING=1)
	if out=$(check_lib "$lib"); then
		echo "not ok - the testing build was accepted:"
		echo "$out"
		exit 1
	fi
	if ! echo "$out" | grep -q '^test-hook symbols exported' ||
		! echo "$out" | grep -qx '  pssc_store_debug_set_clock'; then
		echo "not ok - the testing build was rejected, but not for its test hooks:"
		echo "$out"
		exit 1
	fi
	echo "ok - the testing build (PSSC_TESTING=1) is rejected for its test hooks"
	echo "release-exports self-test passed"
	;;
*)
	echo "usage: $0 [--lib FILE | --self-test]" >&2
	exit 2
	;;
esac
