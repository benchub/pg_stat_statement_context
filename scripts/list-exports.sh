#!/bin/sh
# Prints the symbols a shared library exports (defined, dynamic), one per
# line, sorted, as C names: nm -D --defined-only on ELF; on macOS nm -gU,
# with Mach-O's leading underscore removed.
# Used by scripts/check-release-exports.sh and test/perl/PsscTest.pm.
# Usage: scripts/list-exports.sh LIBRARY
set -eu

[ $# -eq 1 ] || { echo "usage: $0 LIBRARY" >&2; exit 2; }
[ -f "$1" ] || { echo "$0: no such file: $1" >&2; exit 2; }
if [ "$(uname -s)" = Darwin ]; then
	out=$(nm -gU "$1")
	echo "$out" | awk 'NF >= 3 { sub(/^_/, "", $3); print $3 }' | sort -u
else
	out=$(nm -D --defined-only "$1")
	echo "$out" | awk 'NF >= 3 { print $3 }' | sort -u
fi
