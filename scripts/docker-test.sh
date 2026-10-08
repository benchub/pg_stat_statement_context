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
#   scripts/docker-test.sh --print-image [--assert|--valgrind] <version>
#                                          print the image tag and exit (CI
#                                          uses it as the build-cache key)
# A bare major with --assert/--valgrind means the release of the local
# postgres:<major> image (pulled if missing), i.e. the same minor as PGDG.
# Source builds are tagged pg_stat_statement_context-pgsrc:<version>-<flavor>-<hash>,
# where <hash> covers docker/Dockerfile.source and docker/build-postgres.sh,
# and are only built when that tag is missing (CI restores it from its cache).
# docker/run-tests.sh is mounted from the repository for source builds. It
# tests the testing build (make PSSC_TESTING=1) with every test, then the
# release build (exported symbols, pg_regress, TAP tests without TEST-ONLY
# modules); valgrind runs only the testing build.
# Logs and regression diffs from a failed run land in tmp/docker-<version>[-<flavor>]/.
set -euo pipefail

usage() { echo "usage: $0 [--assert|--valgrind] [--print-image] <major|major.minor>" >&2; exit 2; }

FLAVOR=
PRINT=0
while [ $# -gt 0 ]; do
	case $1 in
	--assert) FLAVOR=assert ;;
	--valgrind) FLAVOR=valgrind ;;
	--print-image) PRINT=1 ;;
	-*) usage ;;
	*) break ;;
	esac
	shift
done
[ $# -le 1 ] || usage
PG_VER=${1:-17}
[[ $PG_VER =~ ^[0-9]+(\.[0-9]+)?$ ]] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Plain major without a flavor: PGDG packages (docker/Dockerfile).
if [ -z "$FLAVOR" ] && [[ $PG_VER != *.* ]]; then
	IMAGE="pg_stat_statement_context-test:pg${PG_VER}"
	if [ "$PRINT" = 1 ]; then echo "$IMAGE"; exit 0; fi
	OUT="$ROOT/tmp/docker-${PG_VER}"
	mkdir -p "$OUT"
	docker build -q --build-arg "PG_MAJOR=${PG_VER}" -t "$IMAGE" "$ROOT/docker" >/dev/null
	docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" "$IMAGE"
	exit
fi

# Source build (docker/Dockerfile.source).
MODE=${FLAVOR:-release}
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
if command -v sha256sum >/dev/null; then sha() { sha256sum; }; else sha() { shasum -a 256; }; fi
HASH=$(cat "$ROOT/docker/Dockerfile.source" "$ROOT/docker/build-postgres.sh" | sha | cut -c1-12)
IMAGE="pg_stat_statement_context-pgsrc:${PG_RELEASE}-${MODE}-${HASH}"
if [ "$PRINT" = 1 ]; then echo "$IMAGE"; exit 0; fi

OUT="$ROOT/tmp/docker-${PG_VER}${FLAVOR:+-$FLAVOR}"
mkdir -p "$OUT"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "building $IMAGE (PostgreSQL $PG_RELEASE from source, $MODE)" >&2
	docker build -q -f "$ROOT/docker/Dockerfile.source" \
		--build-arg "PG_MAJOR=${PG_MAJOR}" --build-arg "PG_SOURCE_VERSION=${PG_RELEASE}" \
		--build-arg "PG_FLAVOR=${MODE}" -t "$IMAGE" "$ROOT/docker" >/dev/null
fi
docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" -e "PSSC_TEST_MODE=${MODE}" \
	--entrypoint /src/docker/run-tests.sh "$IMAGE"
