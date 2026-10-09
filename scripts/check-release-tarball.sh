#!/bin/bash
# Checks the release tarball, i.e. what `git archive` makes of HEAD with the
# export-ignore rules of .gitattributes (commit first: uncommitted changes
# are not in it).
#   scripts/check-release-tarball.sh --list-only   contents only (no Docker)
#   scripts/check-release-tarball.sh [<major>]     contents, then extract it under
#                                                  tmp/release-tarball/ and run
#                                                  scripts/docker-test.sh <major>
#                                                  (default 18) from there: make,
#                                                  make install, installcheck,
#                                                  TAP, both builds
# Contents: the agent and planning files and research/ are left out; what
# building, testing, packaging and regenerating docs/benchmarks.md need is
# kept. With python3, docs/benchmarks.md is regenerated from the tarball's
# bench/results/ and must match the shipped one.
# The extracted tree's Docker images are pruned afterwards; on failure the
# tree and its logs (tmp/docker-<major>/) are kept for inspection.
set -euo pipefail

LIST_ONLY=0
if [ "${1:-}" = --list-only ]; then LIST_ONLY=1; shift; fi
[ $# -le 1 ] || { echo "usage: $0 --list-only | [<major>]" >&2; exit 2; }
PG=${1:-18}
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$ROOT"

status=0
err() { echo "FAIL: $*" >&2; status=1; }

LIST=$(git archive --format=tar HEAD | tar -tf -)
[ -n "$LIST" ] || { echo "FAIL: empty archive" >&2; exit 1; }

# Left out: agent and planning files (CLAUDE.md, BACKLOG*.md, the backlog
# script, agent settings), scratch and worktree directories, and research/.
for p in CLAUDE.md BACKLOG.md BACKLOG-COMPLETE.md scripts/backlog-complete.py; do
	if grep -qx "$p" <<<"$LIST"; then err "$p is in the archive"; fi
done
for d in research worktrees tmp .claude .copilot; do
	if grep -q "^$d/" <<<"$LIST"; then err "$d/ is in the archive"; fi
done

# Kept: every tracked file that is not export-ignored, so in particular the
# build, the tests and their scripts, the docs and the benchmark data.
for p in Makefile pg_stat_statement_context.control LICENSE NOTICE README.md \
	CHANGELOG.md DESIGN.md .gitattributes sql/pg_stat_statement_context--1.0.sql \
	sql/frozen.sha256 src/pg_stat_statement_context.c test/t/043_release_tree.pl \
	test/pg_stat_statement_context.conf docker/run-tests.sh docker/Dockerfile \
	scripts/docker-test.sh scripts/check-release-exports.sh \
	bench/benchmarks.md.in bench/analyze.py docs/benchmarks.md \
	docs/maintaining.md fuzz/Makefile; do
	grep -qx "$p" <<<"$LIST" || err "$p is missing from the archive"
done
grep -q '^bench/results/.*/campaign\.json$' <<<"$LIST" || err "bench/results/ is missing from the archive"
while IFS= read -r f; do
	case $f in
	CLAUDE.md | BACKLOG.md | BACKLOG-COMPLETE.md | scripts/backlog-complete.py | research/*) continue ;;
	esac
	grep -qxF "$f" <<<"$LIST" || err "tracked file $f is missing from the archive"
done < <(git ls-tree -r --name-only HEAD)
[ $status = 0 ] || exit 1
echo "archive contents: ok ($(wc -l <<<"$LIST" | tr -d ' ') entries)"
[ $LIST_ONLY = 0 ] || exit 0

DIR="$ROOT/tmp/release-tarball/pg_stat_statement_context"
rm -rf "$DIR"
mkdir -p "$DIR"
git archive --format=tar HEAD | tar -xf - -C "$DIR"
[ ! -e "$DIR/.git" ] || { echo "FAIL: extracted tree has .git" >&2; exit 1; }

if command -v python3 >/dev/null; then
	mkdir -p "$DIR/tmp"
	python3 "$DIR/bench/analyze.py" doc --results "$DIR/bench/results" \
		--template "$DIR/bench/benchmarks.md.in" --out "$DIR/tmp/benchmarks.md"
	cmp -s "$DIR/tmp/benchmarks.md" "$DIR/docs/benchmarks.md" \
		|| { echo "FAIL: docs/benchmarks.md does not regenerate from the tarball" >&2; exit 1; }
	echo "docs/benchmarks.md regenerates from the tarball: ok"
fi

prune() { "$DIR/scripts/docker-test.sh" --prune >/dev/null || true; }
if ! "$DIR/scripts/docker-test.sh" "$PG"; then
	prune
	echo "FAIL: scripts/docker-test.sh $PG from the extracted tarball; logs in $DIR/tmp/docker-$PG/" >&2
	exit 1
fi
prune
rm -rf "$ROOT/tmp/release-tarball"
echo "release tarball: ok (PostgreSQL $PG)"
