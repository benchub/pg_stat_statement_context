#!/bin/bash
# Overhead, CPU and latency benchmarks for pg_stat_statement_context (pgbench in Docker).
#
#   bench/run.sh [--major N] [--blocks B] [--duration S] [--warmup S] [--seed X]
#                [--only REGEX] [--high-clients] [--cpuset CPUS] [--keep-logs]
#                [--dry-run] [--save [--label L]] [--allow-dirty]
#
#   --major N       PostgreSQL major (PGDG image; default 18)
#   --blocks B      blocks (pairs): each runs every configuration once (default 8)
#   --duration S    seconds per measured run (default 12); --warmup S (default 3)
#   --seed X        seed of the run order (default: random, recorded)
#   --only REGEX    only the scenarios whose name matches (e.g. '^inlist-')
#   --high-clients  add the opt-in 256-client scenarios (ro and rw, simple)
#   --cpuset CPUS   docker run --cpuset-cpus (dedicated hosts; see bench/README.md)
#   --keep-logs     keep gzipped per-transaction pgbench logs in the output dir
#   --dry-run       self-check: every scenario, 2 blocks of 2 s runs (~25 min)
#   --save          copy the raw results to bench/results/<date>-<label>/
#                   (refused for --dry-run and for uncommitted extension code)
#   --label L       host label for --save (default: derived from the CPU)
#
# The default campaign is 50 configurations x 8 blocks x (12 s + 3 s warmup +
# ~8 s restart, checks and analysis), about 2.5 hours. It uses the image of
# scripts/docker-test.sh N (this checkout's pg_stat_statement_context-test:pgN-<hash>),
# and one --rm container: the server and pgbench talk over the Unix socket and
# are pinned to their own CPUs. Output: tmp/bench-pgN[-dry]/ (report.md,
# runs.jsonl, runs/*, plan.tsv, env-*.txt, campaign.json). Exits non-zero if
# any run fails its checks (bench/inside.sh) or the analysis self-test fails.
# bench/README.md has the method and the instructions for dedicated hardware;
# docs/benchmarks.md is generated from bench/results/.
set -euo pipefail

usage() { sed -n '4,20p' "$0" >&2; exit 2; }
MAJOR=18 BLOCKS=8 DURATION=12 WARMUP=3 SEED= ONLY= HIGH=0 KEEP=0 DRY=0 SAVE=0 LABEL= DIRTY_OK=0 CPUSET=
while [ $# -gt 0 ]; do
	case $1 in
	--major) MAJOR=${2:?}; shift ;;
	--blocks) BLOCKS=${2:?}; shift ;;
	--duration) DURATION=${2:?}; shift ;;
	--warmup) WARMUP=${2:?}; shift ;;
	--seed) SEED=${2:?}; shift ;;
	--only) ONLY=${2:?}; shift ;;
	--high-clients) HIGH=1 ;;
	--cpuset) CPUSET=${2:?}; shift ;;
	--keep-logs) KEEP=1 ;;
	--dry-run) DRY=1 ;;
	--save) SAVE=1 ;;
	--label) LABEL=${2:?}; shift ;;
	--allow-dirty) DIRTY_OK=1 ;;
	*) usage ;;
	esac
	shift
done
[[ $MAJOR =~ ^[0-9]+$ && $BLOCKS =~ ^[0-9]+$ && $DURATION =~ ^[0-9]+$ && $WARMUP =~ ^[0-9]+$ ]] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ "$DRY" = 1 ]; then
	BLOCKS=2 DURATION=2 WARMUP=1
	[ "$SAVE" = 1 ] && { echo "--save is not allowed with --dry-run" >&2; exit 2; }
	OUT="$ROOT/tmp/bench-pg${MAJOR}-dry"
else
	OUT="$ROOT/tmp/bench-pg${MAJOR}"
fi
SEED=${SEED:-$((RANDOM * 32768 + RANDOM))}
IMAGE=$("$ROOT/scripts/docker-test.sh" --print-image "$MAJOR")
NAME="pssc-bench-pg${MAJOR}-$$"
COMMIT=$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo '?')
DIRTY=false
if [ -n "$(git -C "$ROOT" status --porcelain -- src sql Makefile pg_stat_statement_context.control 2>/dev/null)" ]; then
	DIRTY=true
	if [ "$SAVE" = 1 ] && [ "$DIRTY_OK" = 0 ]; then
		echo "the extension's code has uncommitted changes: commit them before --save (or --allow-dirty)" >&2
		exit 2
	fi
fi

# Fail fast on the analysis code before spending time on Docker.
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/bench/analyze.py" --self-test >/dev/null 2>&1 \
	|| { echo "bench/analyze.py self-test failed" >&2; exit 1; }

others() { docker ps --format '{{.Names}}' | grep -v -x "$NAME" | paste -sd, - || true; }
rm -rf "$OUT"
mkdir -p "$OUT"
OTHERS_START=$(others)
if [ "$(uname)" = Darwin ]; then
	HOST="$(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?') performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?') efficiency), $(($(sysctl -n hw.memsize) / 1073741824)) GB, macOS $(sw_vers -productVersion)"
	DEFLABEL=$(sysctl -n machdep.cpu.brand_string | tr 'A-Z ' 'a-z-')-docker
else
	HOST="$(lscpu 2>/dev/null | sed -n 's/^Model name: *//p' | head -n 1), $(nproc) CPUs, $(awk '/MemTotal/ {printf "%d GB", $2 / 1048576}' /proc/meminfo), $(uname -sr)"
	DEFLABEL=$(uname -m)-$(hostname -s)
fi
DOCKER=$(docker info --format '{{.OperatingSystem}} {{.ServerVersion}}, VM/host: {{.NCPU}} CPUs, {{.MemTotal}} bytes')
{
	echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "git: $COMMIT$([ "$DIRTY" = true ] && echo ' (extension code dirty)')"
	echo "host: $HOST"
	echo "docker: $DOCKER"
	echo "other running containers at start: ${OTHERS_START:-none}"
	echo "image: $IMAGE"
	echo "blocks: $BLOCKS, duration: ${DURATION}s, warmup: ${WARMUP}s, seed: $SEED${ONLY:+, only: $ONLY}${CPUSET:+, cpuset: $CPUSET}"
} | tee "$OUT/env-host.txt"

"$ROOT/scripts/docker-test.sh" --build-image "$MAJOR" >/dev/null
trap 'docker kill "$NAME" >/dev/null 2>&1 || true' INT TERM
T0=$(date +%s)
docker run --rm --name "$NAME" --shm-size=1g ${CPUSET:+--cpuset-cpus "$CPUSET"} \
	-v "$ROOT:/src:ro" -v "$OUT:/out" \
	-e BENCH_BLOCKS="$BLOCKS" -e BENCH_DURATION="$DURATION" -e BENCH_WARMUP="$WARMUP" -e BENCH_SEED="$SEED" \
	-e BENCH_ONLY="$ONLY" -e BENCH_HIGH_CLIENTS="$HIGH" -e BENCH_KEEP_LOGS="$KEEP" \
	-e BENCH_SCALE -e BENCH_INLIST_N -e BENCH_WINDOW_MS \
	-e BENCH_SERVER_CPUS -e BENCH_CLIENT_CPUS -e BENCH_MAX_FOREIGN -e BENCH_ATTEMPTS \
	--entrypoint /src/bench/inside.sh "$IMAGE"
WALL=$(($(date +%s) - T0))
OTHERS_END=$(others)

python3 - "$OUT" <<PY
import json, os, sys
out = sys.argv[1]
c = json.load(open(os.path.join(out, "container.json")))
foreign = []
for line in open(os.path.join(out, "runs.jsonl")):
    r = json.loads(line)
    if r.get("foreign_cores") is not None:
        foreign.append(r["foreign_cores"])
quiet = ("other containers at start: %s; at end: %s; CPU used outside the benchmark container during each run "
         "measured from the VM's /proc/stat: max %.2f cores, median %.2f (runs over ${BENCH_MAX_FOREIGN:-0.75} cores are retried)") % (
    "${OTHERS_START:-none}", "${OTHERS_END:-none}", max(foreign or [0]), sorted(foreign or [0])[len(foreign or [0]) // 2])
camp = {"commit": "$COMMIT", "dirty": "$DIRTY" == "true", "date": "$(date -u +%Y-%m-%d)", "host": """$HOST""",
        "docker": """$DOCKER""", "image": "$IMAGE", "pg_version": c["pg_version"], "kernel": c["kernel"],
        "build": "release (make, no PSSC_TESTING)", "server_cpus": c["server_cpus"], "client_cpus": c["client_cpus"],
        "ncpu": c["ncpu"], "blocks": $BLOCKS, "duration_s": $DURATION, "warmup_s": $WARMUP, "seed": $SEED,
        "only": "$ONLY" or None, "high_clients": bool($HIGH), "cpuset": "$CPUSET" or None, "dry_run": bool($DRY),
        "wall_time_s": $WALL, "quiet_check": quiet,
        "scenarios": json.load(open(os.path.join(out, "scenarios.json")))}
json.dump(camp, open(os.path.join(out, "campaign.json"), "w"), indent=1)
PY
python3 "$ROOT/bench/analyze.py" report "$OUT" --out "$OUT/report.md"
echo "report: $OUT/report.md ($((WALL / 60)) min)"
if [ "$SAVE" = 1 ]; then
	DEST="$ROOT/bench/results/$(date -u +%Y-%m-%d)-${LABEL:-$DEFLABEL}"
	mkdir -p "$DEST"
	cp "$OUT"/{campaign.json,runs.jsonl,plan.tsv,env-host.txt,env-container.txt} "$DEST"/
	echo "saved: $DEST (add a NOTES.md, then: python3 bench/analyze.py doc --results bench/results" \
		"--template bench/benchmarks.md.in --out docs/benchmarks.md --check-fresh)"
fi
