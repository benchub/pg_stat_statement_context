#!/bin/bash
# Build the libFuzzer targets (fuzz/Makefile "fuzz") with clang
# -fsanitize=fuzzer,address,undefined inside a Linux container and run them,
# in parallel, one process per target. Apple clang has no libFuzzer, so this
# is the way to run them on macOS; on Linux it works too (or run
# "make -C fuzz fuzz" with a libFuzzer-capable clang directly).
#
#   fuzz/run-libfuzzer.sh [-t SECONDS] [TARGET...]
#
# TARGET: fuzz_scan fuzz_sqlcommenter fuzz_marginalia fuzz_tagset (default:
# all). -t: seconds per target (default 600). Each target starts from the
# seed corpus (make -C fuzz corpus) plus tmp/fuzz/corpus-<target>/, where
# new inputs accumulate across runs. Crash/leak/timeout inputs land in
# tmp/fuzz/artifacts/ and the logs in tmp/fuzz/<target>.log. Reproduce a
# crash with: fuzz/run-libfuzzer.sh -r tmp/fuzz/artifacts/<file> <target>.
# Exits non-zero if any target found a problem.
#
# The image is pssc-fuzz:clang-<hash of fuzz/Dockerfile>, built when that
# tag is missing; PSSC_FUZZ_BASE overrides its base image (any Debian or
# Ubuntu image, e.g. an already pulled postgres:18).
# PSSC_FUZZ_SRC=tmp/<dir> builds against that copy of src/ instead (for
# mutation checks: inject a bug into a throwaway copy, never into src/),
# and PSSC_FUZZ_OUT=tmp/<dir> replaces tmp/fuzz as the output directory.
set -euo pipefail

usage() { echo "usage: $0 [-t SECONDS] [-r ARTIFACT] [TARGET...]" >&2; exit 2; }
SECS=600
REPRO=
while getopts t:r: o; do
	case $o in
	t) SECS=$OPTARG ;;
	r) REPRO=$OPTARG ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
ALL="fuzz_scan fuzz_sqlcommenter fuzz_marginalia fuzz_tagset"
TARGETS=${*:-$ALL}
for t in $TARGETS; do
	case " $ALL " in *" $t "*) ;; *) echo "unknown target $t" >&2; usage ;; esac
done
[[ $SECS =~ ^[0-9]+$ ]] || usage

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/${PSSC_FUZZ_OUT:-tmp/fuzz}
case $OUT in "$ROOT"/tmp/*) ;; *) echo "PSSC_FUZZ_OUT must be under tmp/" >&2; exit 2 ;; esac
mkdir -p "$OUT/artifacts"
if command -v sha256sum >/dev/null; then sha() { sha256sum; }; else sha() { shasum -a 256; }; fi
IMAGE="pssc-fuzz:clang-$(sha < "$ROOT/fuzz/Dockerfile" | cut -c1-12)"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "building $IMAGE" >&2
	docker build -q ${PSSC_FUZZ_BASE:+--build-arg "BASE=$PSSC_FUZZ_BASE"} \
		-t "$IMAGE" "$ROOT/fuzz" >/dev/null
fi
if [ -n "$REPRO" ]; then
	case $REPRO in "$ROOT"/tmp/*) ;; tmp/*) REPRO=$ROOT/$REPRO ;; *)
		echo "-r: the artifact must be under tmp/" >&2; exit 2 ;; esac
	REPRO=/repo/${REPRO#"$ROOT"/}
fi

SRCMOUNT=()
if [ -n "${PSSC_FUZZ_SRC:-}" ]; then
	SRC_DIR=$(cd "$PSSC_FUZZ_SRC" && pwd)
	case $SRC_DIR in "$ROOT"/tmp/*) ;; *) echo "PSSC_FUZZ_SRC must be under tmp/" >&2; exit 2 ;; esac
	SRCMOUNT=(-v "$SRC_DIR:/repo/src:ro")
fi

# Copy only what the build needs, so host build output is never reused. UBSan
# findings are fatal (-fno-sanitize-recover=all in fuzz/Makefile, and
# halt_on_error here), so they fail the run and leave a reproducer.
docker run --rm -v "$ROOT:/repo:ro" ${SRCMOUNT[@]+"${SRCMOUNT[@]}"} -v "$OUT:/out" \
	-e "SECS=$SECS" -e "TARGETS=$TARGETS" -e "REPRO=$REPRO" \
	-e UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$IMAGE" bash -c '
set -euo pipefail
mkdir -p /build && cd /repo
tar -cf - src test/unit/*.c test/unit/*.h test/unit/Makefile test/sql fuzz/*.c \
	fuzz/*.h fuzz/Makefile fuzz/split-sql.awk fuzz/fuzz.dict | tar -C /build -xf -
cd /build/fuzz
make -s CC=clang corpus >/dev/null
make -s CLANG=clang $TARGETS
if [ -n "$REPRO" ]; then exec ./$TARGETS "$REPRO"; fi
echo "fuzzing $TARGETS for ${SECS}s each ($(clang --version | head -n1))"
pids=
for t in $TARGETS; do
	mkdir -p /out/corpus-$t
	./$t -max_total_time="$SECS" -timeout=25 -max_len=4096 -dict=fuzz.dict \
		-print_final_stats=1 -artifact_prefix=/out/artifacts/$t- \
		/out/corpus-$t corpus > /out/$t.log 2>&1 &
	pids="$pids $!"
done
rc=0
set -- $TARGETS
for p in $pids; do
	if wait $p; then st=ok; else st=FAILED; rc=1; fi
	echo "$1: $st, $(grep -m1 "^stat::number_of_executed_units" /out/$1.log | sed "s/.*: *//") runs, $(grep -o "cov: [0-9]*" /out/$1.log | tail -n1), corpus $(ls /out/corpus-$1 | wc -l)"
	[ $st = ok ] || grep -E "ERROR|invariant failed|SUMMARY|^artifact_prefix|Test unit written" /out/$1.log | head -n 20
	shift
done
exit $rc
'
