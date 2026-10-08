#!/bin/bash
# SQL-level fuzz driver for the regex extractor, against a server in Docker.
#   fuzz/sql/run.sh [--assert|--valgrind|--pgdg] [--pg MAJOR] [-- regex_fuzz.pl options]
# Default: --assert --pg 18, i.e. the cassert source build of
# scripts/docker-test.sh --assert 18 (built by that script; run it once first).
# --pgdg uses this checkout's PGDG package image (scripts/docker-test.sh
# --build-image, so --prune sees it; as CI does).
# regex_fuzz.pl options: --seed N (printed at the start; default random),
# --duration SECONDS (default 60), --rounds N (instead of --duration, to
# replay a seed), --calls N (extract calls per round, default 40).
# Failures are saved under tmp/fuzz-sql/ (round SQL, psql output, server log,
# seed). Example: fuzz/sql/run.sh -- --duration 600
# PSSC_FUZZ_SRC=tmp/<dir> mounts that directory over src/ (for mutation
# checks: inject a bug into a throwaway copy, never into src/).
set -euo pipefail

usage() { echo "usage: $0 [--assert|--valgrind|--pgdg] [--pg MAJOR] [-- regex_fuzz.pl options]" >&2; exit 2; }
FLAVOR=assert
PG=18
while [ $# -gt 0 ]; do
	case $1 in
	--assert) FLAVOR=assert ;;
	--valgrind) FLAVOR=valgrind ;;
	--pgdg) FLAVOR=pgdg ;;
	--pg) PG=${2:?}; shift ;;
	--) shift; break ;;
	*) usage ;;
	esac
	shift
done
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/tmp/fuzz-sql
mkdir -p "$OUT"

if [ "$FLAVOR" = pgdg ]; then
	IMAGE=$("$ROOT/scripts/docker-test.sh" --build-image "$PG")
else
	IMAGE=$("$ROOT/scripts/docker-test.sh" --print-image "--$FLAVOR" "$PG")
	docker image inspect "$IMAGE" >/dev/null 2>&1 || {
		echo "missing $IMAGE: build it with scripts/docker-test.sh --$FLAVOR $PG" >&2
		exit 1
	}
fi
SRCMOUNT=()
if [ -n "${PSSC_FUZZ_SRC:-}" ]; then
	MUT=$(cd "$PSSC_FUZZ_SRC" && pwd)
	case $MUT in "$ROOT"/tmp/*) ;; *) echo "PSSC_FUZZ_SRC must be under tmp/" >&2; exit 2 ;; esac
	SRCMOUNT=(-v "$MUT:/src/src:ro")
	echo "src/ replaced by $PSSC_FUZZ_SRC"
fi
echo "image: $IMAGE"
exec docker run --rm --init -v "$ROOT:/src:ro" ${SRCMOUNT[@]+"${SRCMOUNT[@]}"} -v "$OUT:/out" \
	--entrypoint /src/fuzz/sql/container.sh "$IMAGE" "$FLAVOR" "$@"
