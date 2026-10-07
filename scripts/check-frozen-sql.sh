#!/bin/bash
# Fails if a released extension script was edited in place (backlog
# 20261005-091225-29). Released install and upgrade scripts are frozen: once a
# version ships, schema changes go into a new upgrade script
# (sql/pg_stat_statement_context--1.0--1.1.sql, ...), never into an old one.
#
# sql/frozen.sha256 lists one "<sha256>  <path relative to the repo>" line per
# frozen script. Every listed file must exist and match. The check also fails
# if the install script for the .control default_version is missing.
#
# To release a new version, add its scripts to sql/frozen.sha256:
#     shasum -a 256 sql/pg_stat_statement_context--1.0--1.1.sql >> sql/frozen.sha256
#
# Usage:
#   scripts/check-frozen-sql.sh [--root DIR]   check a tree (default: repo)
#   scripts/check-frozen-sql.sh --self-test    check the checker itself
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXT=pg_stat_statement_context

sha256() {
	if command -v sha256sum >/dev/null; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

check_tree() {
	local root=$1 status=0 n=0 want path got ver
	local manifest=$root/sql/frozen.sha256
	if [ ! -f "$manifest" ]; then
		echo "sql/frozen.sha256: missing"
		return 1
	fi
	while read -r want path || [ -n "$want" ]; do
		case $want in '' | '#'*) continue ;; esac
		n=$((n + 1))
		if [ ! -f "$root/$path" ]; then
			echo "$path: frozen script is missing"
			status=1
			continue
		fi
		got=$(sha256 "$root/$path")
		if [ "$got" != "$want" ]; then
			echo "$path: frozen script was modified (sha256 $got, expected $want); put schema changes in a new upgrade script"
			status=1
		fi
	done <"$manifest"
	if [ $n -eq 0 ]; then
		echo "sql/frozen.sha256: lists no scripts"
		status=1
	fi
	ver=$(sed -n "s/^default_version *= *'\([^']*\)'.*/\1/p" "$root/$EXT.control")
	if [ -z "$ver" ]; then
		echo "$EXT.control: no default_version"
		status=1
	elif [ ! -f "$root/sql/$EXT--$ver.sql" ] && ! ls "$root"/sql/"$EXT"--*--"$ver".sql >/dev/null 2>&1; then
		echo "$EXT.control: default_version $ver has no install or upgrade script in sql/"
		status=1
	fi
	return $status
}

self_test() {
	local dir fails=0
	mkdir -p "$ROOT/tmp"
	dir=$(mktemp -d "$ROOT/tmp/frozen-sql.XXXXXX")
	trap 'rm -rf "$dir"' RETURN

	mkdir -p "$dir/sql"
	printf "default_version = '1.0'\n" >"$dir/$EXT.control"
	printf 'CREATE FUNCTION f() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql;\n' >"$dir/sql/$EXT--1.0.sql"
	printf '%s  sql/%s--1.0.sql\n' "$(sha256 "$dir/sql/$EXT--1.0.sql")" "$EXT" >"$dir/sql/frozen.sha256"

	expect() {
		local want=$1 what=$2 rc=0
		"$SELF" --root "$dir" >"$dir/out" 2>&1 || rc=$?
		if { [ "$want" = pass ] && [ $rc -ne 0 ]; } || { [ "$want" = fail ] && [ $rc -eq 0 ]; }; then
			echo "self-test: expected $want: $what"
			cat "$dir/out"
			fails=$((fails + 1))
		fi
	}

	expect pass "unchanged frozen script"

	printf -- '-- 1.1\n' >"$dir/sql/$EXT--1.0--1.1.sql"
	printf "default_version = '1.1'\n" >"$dir/$EXT.control"
	expect pass "new upgrade script with default_version bump"

	printf "default_version = '1.2'\n" >"$dir/$EXT.control"
	expect fail "default_version without a script"
	printf "default_version = '1.0'\n" >"$dir/$EXT.control"

	cp "$dir/sql/$EXT--1.0.sql" "$dir/orig"
	printf -- '-- tweak\n' >>"$dir/sql/$EXT--1.0.sql"
	expect fail "frozen script edited in place"
	mv "$dir/orig" "$dir/sql/$EXT--1.0.sql"
	expect pass "frozen script restored"

	mv "$dir/sql/$EXT--1.0.sql" "$dir/orig"
	expect fail "frozen script deleted"
	mv "$dir/orig" "$dir/sql/$EXT--1.0.sql"

	cp "$dir/sql/frozen.sha256" "$dir/manifest"
	printf '%s  sql/%s--1.0.sql\n%s  sql/%s--1.0--1.1.sql' \
		"$(sha256 "$dir/sql/$EXT--1.0.sql")" "$EXT" "$(printf '0%.0s' {1..64})" "$EXT" >"$dir/sql/frozen.sha256"
	expect fail "mismatch in a final manifest line without a newline"
	printf '%s  sql/%s--1.0.sql\n%s  sql/%s--9.9.sql' \
		"$(sha256 "$dir/sql/$EXT--1.0.sql")" "$EXT" "$(sha256 "$dir/sql/$EXT--1.0.sql")" "$EXT" >"$dir/sql/frozen.sha256"
	expect fail "missing file in a final manifest line without a newline"
	printf '# no entries\n' >"$dir/sql/frozen.sha256"
	expect fail "empty manifest"
	rm "$dir/sql/frozen.sha256"
	expect fail "missing manifest"
	mv "$dir/manifest" "$dir/sql/frozen.sha256"

	[ $fails -eq 0 ] && echo "frozen-sql self-test passed"
	return $fails
}

case "${1:-}" in
	--self-test) self_test ;;
	--root) check_tree "$(cd "$2" && pwd)" ;;
	"") check_tree "$ROOT" && echo "frozen sql ok" ;;
	*) echo "usage: $0 [--root DIR | --self-test]" >&2; exit 2 ;;
esac
