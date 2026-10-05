#!/bin/bash
# Build the extension and run its tests against PostgreSQL <major> in Docker.
#   scripts/docker-test.sh 17
# Logs and regression diffs from a failed run land in tmp/docker-<major>/.
set -euo pipefail

PG_MAJOR=${1:-17}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE="pg_stat_statement_context-test:pg${PG_MAJOR}"
OUT="$ROOT/tmp/docker-${PG_MAJOR}"

mkdir -p "$OUT"
docker build -q --build-arg "PG_MAJOR=${PG_MAJOR}" -t "$IMAGE" "$ROOT/docker" >/dev/null
docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" "$IMAGE"
