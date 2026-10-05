#!/bin/bash
# Fails if PG_VERSION_NUM appears in a C source/header outside src/compat.h
# without a justifying comment (DESIGN.md §6.10, backlog 20261005-091225-2).
#
# A use is justified by a marker with a non-empty reason on the same line or
# the line directly above:
#     /* version-guard-ok: <why this can't live in compat.h> */
#
# Usage:
#   scripts/check-version-guards.sh [--root DIR]   check a tree (default: repo)
#   scripts/check-version-guards.sh --self-test    check the checker itself
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
ROOT=$(cd "$(dirname "$0")/.." && pwd)

check_tree() {
	local root=$1 status=0 f rel
	while IFS= read -r -d '' f; do
		rel=${f#"$root"/}
		[ "$rel" = "src/compat.h" ] && continue
		awk -v file="$rel" '
			function justified(s) { return s ~ /version-guard-ok:[[:space:]]*[^[:space:]*]/ }
			/PG_VERSION_NUM/ && !justified($0) && !justified(prev) {
				printf "%s:%d: PG_VERSION_NUM outside src/compat.h without a version-guard-ok: comment\n", file, NR
				bad = 1
			}
			{ prev = $0 }
			END { exit bad }
		' "$f" || status=1
	done < <(find "$root" \
		\( -name .git -o -name tmp -o -name tmp_check -o -name results -o -name log \) -prune -o \
		-type f \( -name '*.c' -o -name '*.h' \) -print0)
	return $status
}

self_test() {
	local dir out fails=0
	mkdir -p "$ROOT/tmp"
	dir=$(mktemp -d "$ROOT/tmp/version-guards.XXXXXX")
	trap 'rm -rf "$dir"' RETURN
	mkdir -p "$dir/src" "$dir/test/sub"

	expect() {
		local want=$1 desc=$2 got=0
		out=$("$SELF" --root "$dir" 2>&1) || got=$?
		if { [ "$want" = pass ] && [ $got -eq 0 ]; } ||
		   { [ "$want" = fail ] && [ $got -ne 0 ]; }; then
			echo "ok - $desc"
		else
			echo "not ok - $desc (exit $got): $out"
			fails=1
		fi
	}

	printf '#if PG_VERSION_NUM >= 160000\n#endif\n' > "$dir/src/compat.h"
	printf 'int x;\n' > "$dir/src/a.c"
	expect pass "PG_VERSION_NUM only in src/compat.h"

	printf '#if PG_VERSION_NUM >= 160000\nint y;\n#endif\n' > "$dir/src/b.c"
	expect fail "unjustified use in a .c file"
	echo "$out" | grep -q '^src/b.c:1:' || { echo "not ok - location reported ($out)"; fails=1; }
	rm "$dir/src/b.c"

	printf '/* x */\n#if PG_VERSION_NUM < 150000\n#endif\n' > "$dir/test/sub/c.h"
	expect fail "unjustified use in a nested header"
	printf '/* version-guard-ok: */\n#if PG_VERSION_NUM < 150000\n#endif\n' > "$dir/test/sub/c.h"
	expect fail "marker without a reason does not justify"
	printf '/* version-guard-ok: test needs it */\n\n#if PG_VERSION_NUM < 150000\n#endif\n' > "$dir/test/sub/c.h"
	expect fail "marker two lines above does not justify"
	printf '/* version-guard-ok: test needs it */\n#if PG_VERSION_NUM < 150000\n#endif\n' > "$dir/test/sub/c.h"
	expect pass "marker on the line above justifies"
	printf '#if PG_VERSION_NUM < 150000 /* version-guard-ok: reason */\n#endif\n' > "$dir/test/sub/c.h"
	expect pass "marker on the same line justifies"

	printf '#if PG_VERSION_NUM >= 1\n#endif\n' > "$dir/src/compat.c"
	expect fail "only src/compat.h is exempt (not src/compat.c)"
	rm "$dir/src/compat.c"

	mkdir -p "$dir/tmp" && printf '#if PG_VERSION_NUM\n#endif\n' > "$dir/tmp/x.c"
	expect pass "tmp/ is ignored"

	[ $fails -eq 0 ] && echo "version-guard self-test passed"
	return $fails
}

case "${1:-}" in
	--self-test) self_test ;;
	--root) check_tree "$(cd "$2" && pwd)" ;;
	"") check_tree "$ROOT" && echo "version guards ok" ;;
	*) echo "usage: $0 [--root DIR | --self-test]" >&2; exit 2 ;;
esac
