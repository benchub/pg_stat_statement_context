#!/bin/bash
# Overhead and latency benchmarks for pg_stat_statement_context (pgbench in Docker).
#
#   bench/run.sh [--major N] [--quick] [--runs R] [--duration S] [--only REGEX] [--keep-logs]
#
#   --major N     PostgreSQL major (PGDG image; default 18)
#   --quick       smoke run: 1 run x 10 s per configuration, 2 s warmup (~5 min)
#   --runs R      measured runs (rounds) per configuration (default 5)
#   --duration S  seconds per measured run (default 20)
#   --only REGEX  only the configurations whose name matches (e.g. '^evict/')
#   --keep-logs   keep gzipped per-transaction pgbench logs under the output dir
#
# The full run (20 configurations x 5 rounds x (20 s + 3 s warmup + restart and
# analysis)) takes about 50 minutes. It uses the same image as
# scripts/docker-test.sh N (this checkout's pg_stat_statement_context-test:pgN-<hash>,
# built from docker/Dockerfile), so no extra image is created. Server and pgbench run in
# one --rm container over the Unix socket, each pinned to its own CPUs.
# Results land in tmp/bench-pgN[-quick]/: results.md (tables), results.json,
# runs/*.json (per run), env-*.txt. Exits non-zero if any configuration fails
# its checks (bench/inside.sh) or the analysis self-test fails.
# docs/benchmarks.md describes the configurations and the published results.
set -euo pipefail

usage() { sed -n '4,11p' "$0" >&2; exit 2; }
MAJOR=18
QUICK=0
RUNS=
DURATION=
ONLY=
KEEP=0
while [ $# -gt 0 ]; do
	case $1 in
	--major) MAJOR=${2:?}; shift ;;
	--quick) QUICK=1 ;;
	--runs) RUNS=${2:?}; shift ;;
	--duration) DURATION=${2:?}; shift ;;
	--only) ONLY=${2:?}; shift ;;
	--keep-logs) KEEP=1 ;;
	*) usage ;;
	esac
	shift
done
[[ $MAJOR =~ ^[0-9]+$ ]] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ "$QUICK" = 1 ]; then
	RUNS=${RUNS:-1} DURATION=${DURATION:-10} WARMUP=2
	OUT="$ROOT/tmp/bench-pg${MAJOR}-quick"
else
	RUNS=${RUNS:-5} DURATION=${DURATION:-20} WARMUP=3
	OUT="$ROOT/tmp/bench-pg${MAJOR}"
fi
IMAGE=$("$ROOT/scripts/docker-test.sh" --print-image "$MAJOR")
NAME="pssc-bench-pg${MAJOR}-$$"

# Fail fast on the analysis code before spending time on Docker.
if command -v python3 >/dev/null; then
	PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/bench/analyze.py" --self-test >/dev/null 2>&1 \
		|| { echo "bench/analyze.py self-test failed" >&2; exit 1; }
fi

rm -rf "$OUT"
mkdir -p "$OUT"
{
	echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "git: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')$(git -C "$ROOT" diff --quiet HEAD 2>/dev/null || echo ' (dirty)')"
	if [ "$(uname)" = Darwin ]; then
		echo "host: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?') performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?') efficiency), $(($(sysctl -n hw.memsize) / 1073741824)) GB, macOS $(sw_vers -productVersion)"
	else
		echo "host: $(uname -srm), $(nproc) CPUs"
	fi
	echo "docker: $(docker info --format '{{.OperatingSystem}} {{.ServerVersion}}, VM: {{.NCPU}} CPUs, {{.MemTotal}} bytes')"
	echo "other running containers at start: $(docker ps -q | wc -l | tr -d ' ')"
	docker stats --no-stream --format '  {{.Name}}: {{.CPUPerc}} CPU' 2>/dev/null || true
	echo "image: $IMAGE"
} | tee "$OUT/env-host.txt"

"$ROOT/scripts/docker-test.sh" --build-image "$MAJOR" >/dev/null
trap 'docker kill "$NAME" >/dev/null 2>&1 || true' INT TERM
docker run --rm --name "$NAME" --shm-size=1g \
	-v "$ROOT:/src:ro" -v "$OUT:/out" \
	-e BENCH_RUNS="$RUNS" -e BENCH_DURATION="$DURATION" -e BENCH_WARMUP="$WARMUP" \
	-e BENCH_ONLY="$ONLY" -e BENCH_KEEP_LOGS="$KEEP" \
	-e BENCH_CLIENTS -e BENCH_THREADS -e BENCH_SCALE -e BENCH_INLIST_N -e BENCH_WINDOW_MS \
	-e BENCH_SERVER_CPUS -e BENCH_CLIENT_CPUS -e BENCH_MAX_FOREIGN -e BENCH_ATTEMPTS \
	--entrypoint /src/bench/inside.sh "$IMAGE"
echo "results: $OUT/results.md"
