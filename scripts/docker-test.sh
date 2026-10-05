#!/bin/bash
# Build the extension and run its tests against PostgreSQL in Docker.
#   scripts/docker-test.sh 17      latest PGDG 17.x package
#   scripts/docker-test.sh 15.0    exact release built from source
#                                  (docker/Dockerfile.source)
# Logs and regression diffs from a failed run land in tmp/docker-<version>/.
set -euo pipefail

PG_VER=${1:-17}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/tmp/docker-${PG_VER}"
IMAGE="pg_stat_statement_context-test:pg${PG_VER}"

mkdir -p "$OUT"
if [[ $PG_VER == *.* ]]; then
	docker build -q -f "$ROOT/docker/Dockerfile.source" \
		--build-arg "PG_MAJOR=${PG_VER%%.*}" --build-arg "PG_VERSION=${PG_VER}" \
		-t "$IMAGE" "$ROOT/docker" >/dev/null
else
	docker build -q --build-arg "PG_MAJOR=${PG_VER}" -t "$IMAGE" "$ROOT/docker" >/dev/null
fi
docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" "$IMAGE"
