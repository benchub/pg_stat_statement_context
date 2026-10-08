#!/bin/bash
# Build the extension and run its tests against PostgreSQL in Docker.
#   scripts/docker-test.sh 17              latest PGDG 17.x package
#   scripts/docker-test.sh 15.0            exact release built from source
#                                          (docker/Dockerfile.source)
#   scripts/docker-test.sh --assert 17     source build with --enable-cassert
#                                          (latest 17.x, or give 17.2 etc.)
#   scripts/docker-test.sh --valgrind 18   source build with cassert and
#                                          -DUSE_VALGRIND; the regression suite
#                                          runs with the server under Valgrind
#                                          (src/tools/valgrind.supp), TAP skipped
#   scripts/docker-test.sh --valgrind-tap 18
#                                          same image; a subset of the TAP
#                                          tests (PSSC_VALGRIND_TAP_TESTS in
#                                          docker/run-tests.sh) with their
#                                          servers under Valgrind
#   scripts/docker-test.sh --print-image [--assert|--valgrind|--valgrind-tap] <version>
#                                          print the image tag and exit (CI
#                                          uses it as the build-cache key)
#   scripts/docker-test.sh --build-image <version>
#                                          build the image if needed, print
#                                          its tag and exit (bench/run.sh and
#                                          scripts/test-integrations.sh)
#   scripts/docker-test.sh --prune         remove this checkout's PGDG images
#   scripts/docker-test.sh --prune-stale   remove the PGDG images of checkouts
#                                          that no longer exist (e.g. removed
#                                          worktrees)
# PGDG images (docker/Dockerfile) are per checkout, so that parallel worktrees
# never run each other's image: pg_stat_statement_context-test:pg<major>-<hash>,
# where <hash> is a hash of the checkout's absolute path, which the image also
# records in the label pssc.checkout. --prune and --prune-stale select images
# by that label only; images of other existing checkouts are never removed.
# A bare major with --assert/--valgrind means the release of the local
# postgres:<major> image (pulled if missing), i.e. the same minor as PGDG.
# Source builds are tagged pg_stat_statement_context-pgsrc:<version>-<flavor>-<hash>,
# where <hash> covers docker/Dockerfile.source and docker/build-postgres.sh,
# and are only built when that tag is missing (CI restores it from its cache).
# docker/run-tests.sh is mounted from the repository for source builds. It
# tests the testing build (make PSSC_TESTING=1) with every test, then the
# release build (exported symbols, pg_regress, TAP tests without TEST-ONLY
# modules); valgrind and valgrind-tap run only the testing build.
# PSSC_SOAK_STATEMENTS (test/t/039_memory_soak.pl) and PSSC_VALGRIND_TAP_TESTS
# are passed to the container when set.
# Logs and regression diffs from a failed run land in tmp/docker-<version>[-<mode>]/.
set -euo pipefail

usage() {
	echo "usage: $0 [--assert|--valgrind|--valgrind-tap] [--print-image|--build-image] <major|major.minor>" >&2
	echo "       $0 --prune|--prune-stale" >&2
	exit 2
}

# FLAVOR selects the image (docker/build-postgres.sh flavor), MODE what
# docker/run-tests.sh runs in it.
FLAVOR=
MODE=
PRINT=0
BUILD_ONLY=0
PRUNE=
while [ $# -gt 0 ]; do
	case $1 in
	--assert) FLAVOR=assert MODE=assert ;;
	--valgrind) FLAVOR=valgrind MODE=valgrind ;;
	--valgrind-tap) FLAVOR=valgrind MODE=valgrind-tap ;;
	--print-image) PRINT=1 ;;
	--build-image) BUILD_ONLY=1 ;;
	--prune) PRUNE=this ;;
	--prune-stale) PRUNE=stale ;;
	-*) usage ;;
	*) break ;;
	esac
	shift
done
[ $# -le 1 ] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
if command -v sha256sum >/dev/null; then sha() { sha256sum; }; else sha() { shasum -a 256; }; fi
LABEL=pssc.checkout

# --prune / --prune-stale: remove PGDG images by their checkout label, tagged
# ones by name and dangling ones (left by rebuilds) by ID.
if [ -n "$PRUNE" ]; then
	[ $# -eq 0 ] && [ -z "$FLAVOR" ] && [ "$PRINT$BUILD_ONLY" = 00 ] || usage
	docker image ls --filter "label=$LABEL" --format '{{.ID}} {{.Repository}}:{{.Tag}}' |
	while read -r id ref; do
		path=$(docker image inspect -f "{{index .Config.Labels \"$LABEL\"}}" "$id" 2>/dev/null) || continue
		[ -n "$path" ] || continue
		if [ "$PRUNE" = this ]; then
			[ "$path" = "$ROOT" ] || continue
		else
			[ ! -e "$path" ] || continue
		fi
		[ "$ref" = "<none>:<none>" ] && ref=$id
		echo "removing $ref ($path)"
		docker rmi "$ref" >/dev/null || echo "could not remove $ref" >&2
	done
	exit 0
fi

PG_VER=${1:-17}
[[ $PG_VER =~ ^[0-9]+(\.[0-9]+)?$ ]] || usage

# Plain major without a flavor: PGDG packages (docker/Dockerfile).
if [ -z "$FLAVOR" ] && [[ $PG_VER != *.* ]]; then
	IMAGE="pg_stat_statement_context-test:pg${PG_VER}-$(printf '%s' "$ROOT" | sha | cut -c1-12)"
	if [ "$PRINT" = 1 ]; then echo "$IMAGE"; exit 0; fi
	docker build -q --build-arg "PG_MAJOR=${PG_VER}" --label "$LABEL=$ROOT" \
		-t "$IMAGE" "$ROOT/docker" >/dev/null
	if [ "$BUILD_ONLY" = 1 ]; then echo "$IMAGE"; exit 0; fi
	echo "image: $IMAGE ($(docker image inspect -f '{{.Id}}' "$IMAGE" | cut -c1-19))" >&2
	OUT="$ROOT/tmp/docker-${PG_VER}"
	mkdir -p "$OUT"
	docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" -e PSSC_SOAK_STATEMENTS "$IMAGE"
	exit
fi

# Source build (docker/Dockerfile.source).
FLAVOR=${FLAVOR:-release}
MODE=${MODE:-release}
PG_MAJOR=${PG_VER%%.*}
if [[ $PG_VER == *.* ]]; then
	PG_RELEASE=$PG_VER
else
	docker image inspect "postgres:${PG_MAJOR}" >/dev/null 2>&1 \
		|| docker pull -q "postgres:${PG_MAJOR}" >/dev/null
	PG_RELEASE=$(docker image inspect "postgres:${PG_MAJOR}" \
		--format '{{range .Config.Env}}{{println .}}{{end}}' | sed -n 's/^PG_VERSION=\([0-9.]*\).*/\1/p')
	[[ $PG_RELEASE == "$PG_MAJOR".* ]] \
		|| { echo "cannot tell the release of postgres:${PG_MAJOR} (got '$PG_RELEASE')" >&2; exit 1; }
fi
HASH=$(cat "$ROOT/docker/Dockerfile.source" "$ROOT/docker/build-postgres.sh" | sha | cut -c1-12)
IMAGE="pg_stat_statement_context-pgsrc:${PG_RELEASE}-${FLAVOR}-${HASH}"
if [ "$PRINT" = 1 ]; then echo "$IMAGE"; exit 0; fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "building $IMAGE (PostgreSQL $PG_RELEASE from source, $FLAVOR)" >&2
	docker build -q -f "$ROOT/docker/Dockerfile.source" \
		--build-arg "PG_MAJOR=${PG_MAJOR}" --build-arg "PG_SOURCE_VERSION=${PG_RELEASE}" \
		--build-arg "PG_FLAVOR=${FLAVOR}" -t "$IMAGE" "$ROOT/docker" >/dev/null
fi
if [ "$BUILD_ONLY" = 1 ]; then echo "$IMAGE"; exit 0; fi
OUT="$ROOT/tmp/docker-${PG_VER}"
[ "$MODE" = release ] || OUT="$OUT-$MODE"
mkdir -p "$OUT"
docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" -e "PSSC_TEST_MODE=${MODE}" \
	-e PSSC_SOAK_STATEMENTS -e PSSC_VALGRIND_TAP_TESTS \
	--entrypoint /src/docker/run-tests.sh "$IMAGE"
