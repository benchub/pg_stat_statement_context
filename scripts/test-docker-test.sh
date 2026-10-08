#!/bin/bash
# Self-test of scripts/docker-test.sh's image tags and --prune/--prune-stale.
# Needs no Docker: each case runs a copy of the script from its own scratch
# checkout under tmp/test-docker-test/, with a stub `docker` on PATH that
# lists made-up images and logs every rmi.
#   1. --print-image <major> gives a tag unique to the checkout path
#      (pg_stat_statement_context-test:pg<major>-<hash>), stable per path.
#   2. Source builds (--print-image 15.0, --assert) stay content-addressed:
#      the same tag in both checkouts (CI caches them by tag).
#   3. --prune removes only the images labelled with this checkout's path.
#   4. --prune-stale removes only the images whose labelled checkout no longer
#      exists, never those of existing checkouts.
#   5. Other scripts get the PGDG image through docker-test.sh --build-image,
#      so it carries the checkout label: fuzz/sql/run.sh --pgdg builds it
#      with the label, and no other tracked file builds the -test tag itself.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
T=$ROOT/tmp/test-docker-test
fails=0

ok() { echo "ok - $*"; }
not_ok() { echo "not ok - $*"; fails=1; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else not_ok "$1: got '$2', want '$3'"; fi; }

rm -rf "$T"
for c in a b; do
	mkdir -p "$T/$c/scripts" "$T/$c/docker" "$T/$c/fuzz/sql"
	cp "$ROOT/scripts/docker-test.sh" "$T/$c/scripts/"
	cp "$ROOT/fuzz/sql/run.sh" "$T/$c/fuzz/sql/"
	cp "$ROOT/docker/Dockerfile" "$ROOT/docker/Dockerfile.source" \
		"$ROOT/docker/build-postgres.sh" "$ROOT/docker/run-tests.sh" "$T/$c/docker/"
done
A=$T/a B=$T/b GONE=$T/gone

# The stub docker. Images: "<id> <repo:tag> <checkout label>" in $T/images.
mkdir -p "$T/stub"
cat > "$T/images" <<EOF
sha256:aaa1 pg_stat_statement_context-test:pg18-1111 $A
sha256:aaa2 <none>:<none> $A
sha256:bbb1 pg_stat_statement_context-test:pg18-2222 $B
sha256:ggg1 pg_stat_statement_context-test:pg18-3333 $GONE
sha256:ggg2 <none>:<none> $GONE
EOF
cat > "$T/stub/docker" <<'EOF'
#!/bin/bash
T=${0%/stub/docker}
case "$1 $2" in
"image ls"|"images "*)
	# Only the labelled images are listed (the script must filter on the label).
	case " $* " in *" --filter label=pssc.checkout "*|*" --filter=label=pssc.checkout "*) ;; *) exit 0 ;; esac
	while read -r id ref path; do echo "$id $ref"; done < "$T/images" ;;
"image inspect")
	id=${*: -1}
	while read -r i ref path; do
		if [ "$i" = "$id" ] || [ "$ref" = "$id" ]; then echo "$path"; exit 0; fi
	done < "$T/images"
	exit 1 ;;
build*)
	echo "$*" >> "$T/built" ;;
run*) ;;
rmi*|"image rm")
	shift; [ "$1" = rm ] && shift
	for x in "$@"; do case $x in -*) ;; *) echo "$x" >> "$T/removed" ;; esac; done ;;
*) echo "unexpected docker $*" >&2; exit 1 ;;
esac
EOF
chmod 755 "$T/stub/docker"
run() { PATH="$T/stub:$PATH" "$@"; }

# 1. Per-checkout tags.
ta=$(run "$A/scripts/docker-test.sh" --print-image 18)
tb=$(run "$B/scripts/docker-test.sh" --print-image 18)
ta2=$(run "$A/scripts/docker-test.sh" --print-image 18)
if [[ $ta =~ ^pg_stat_statement_context-test:pg18-[0-9a-f]{8,}$ ]]; then ok "tag format ($ta)"; else not_ok "tag format: '$ta'"; fi
if [ "$ta" != "$tb" ]; then ok "two checkouts get different tags"; else not_ok "two checkouts share tag '$ta'"; fi
check "a checkout's tag is stable" "$ta2" "$ta"
t14=$(run "$A/scripts/docker-test.sh" --print-image 14)
if [ "$t14" != "$ta" ] && [[ $t14 == pg_stat_statement_context-test:pg14-* ]]; then ok "majors get different tags"; else not_ok "pg14 tag '$t14'"; fi

# 2. Source builds keep their content-addressed tags.
check "source tag shared by both checkouts" \
	"$(run "$A/scripts/docker-test.sh" --print-image 15.0)" "$(run "$B/scripts/docker-test.sh" --print-image 15.0)"
check "assert tag shared by both checkouts" \
	"$(run "$A/scripts/docker-test.sh" --print-image --assert 17.2)" "$(run "$B/scripts/docker-test.sh" --print-image --assert 17.2)"

# 3. --prune: this checkout's images only (tag by name, dangling by ID).
rm -f "$T/removed"
run "$A/scripts/docker-test.sh" --prune >/dev/null
check "--prune removes this checkout's images" \
	"$(sort "$T/removed" 2>/dev/null | tr '\n' ' ')" "pg_stat_statement_context-test:pg18-1111 sha256:aaa2 "

# 4. --prune-stale: only images of checkouts that no longer exist.
rm -f "$T/removed"
run "$A/scripts/docker-test.sh" --prune-stale >/dev/null
check "--prune-stale removes only missing checkouts' images" \
	"$(sort "$T/removed" 2>/dev/null | tr '\n' ' ')" "pg_stat_statement_context-test:pg18-3333 sha256:ggg2 "

# 5. The fuzz PGDG image is built with this checkout's label.
rm -f "$T/built"
run "$A/fuzz/sql/run.sh" --pgdg --pg 18 >/dev/null 2>&1 || true
if grep -q -- "--label pssc.checkout=$A " "$T/built" 2>/dev/null \
	&& grep -q -- "-t $ta " "$T/built"; then
	ok "fuzz/sql/run.sh --pgdg builds the labelled image"
else
	not_ok "fuzz/sql/run.sh --pgdg build: '$(cat "$T/built" 2>/dev/null)'"
fi
direct=$(cd "$ROOT" && git ls-files -z | xargs -0 grep -lE 'docker build' 2>/dev/null \
	| grep -vx -e scripts/docker-test.sh -e scripts/test-docker-test.sh \
	| while read -r f; do
		if grep -qE 'pg_stat_statement_context-test|--print-image|"\$ROOT/docker" *(>|$)' "$ROOT/$f"; then echo "$f"; fi
	done || true)
check "no other file builds the PGDG image directly" "$direct" ""

if [ $fails -eq 0 ]; then echo "docker-test self-test passed"; rm -rf "$T"; else exit 1; fi
