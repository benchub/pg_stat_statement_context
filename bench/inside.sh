#!/bin/bash
# Runs inside the docker/Dockerfile image (pg_stat_statement_context-test:pgN),
# started by bench/run.sh. Builds and installs the extension from /src, creates
# a pgbench database, and runs every configuration round-robin (one restart per
# configuration and run, then a warmup, a stats reset and the measured run).
# The server and pgbench share the container and talk over the Unix socket;
# each is pinned to its own CPUs with taskset. Each measured run is checked
# (preload list, tags actually recorded, evictions, bucket rollovers) and
# summarized by bench/analyze.py; any failed check exits non-zero.
#
# Environment (defaults are the full run; bench/run.sh --quick lowers them):
#   BENCH_RUNS=5 BENCH_DURATION=20 BENCH_WARMUP=3   runs per config, seconds
#   BENCH_CLIENTS=8 BENCH_THREADS=3 BENCH_SCALE=10  pgbench -c/-j/-s
#   BENCH_INLIST_N=10000                            IN-list length
#   BENCH_WINDOW_MS=5                               boundary window (+-ms)
#   BENCH_SERVER_CPUS=0-4 BENCH_CLIENT_CPUS=5-7     taskset CPU lists (with 10
#                                                   CPUs; 2 are left idle so the
#                                                   busy vCPUs can stay on
#                                                   performance cores)
#   BENCH_ONLY=<regex>                              configs to run (debugging)
#   BENCH_KEEP_LOGS=1                               keep gzipped pgbench logs
#   BENCH_MAX_FOREIGN=0.75 BENCH_ATTEMPTS=3          retry a run when other
#                                                   containers used more than
#                                                   this many CPU cores during it
#
# Interference: Docker containers see the VM-wide /proc/stat, so the CPU time
# used outside this container during a run (VM busy time minus this cgroup's
# usage_usec) is measured and recorded per run as foreign_cores.
set -euo pipefail

SRC=${PSSC_SRC:-/src}
OUT=${PSSC_OUT:-/out}
WORK=/var/lib/postgresql/bench
DATA=$WORK/data
LOGS=$WORK/logs
SCRIPTS=$WORK/scripts
SOCK=/var/run/postgresql
RUNS=${BENCH_RUNS:-5}
DURATION=${BENCH_DURATION:-20}
WARMUP=${BENCH_WARMUP:-3}
CLIENTS=${BENCH_CLIENTS:-8}
THREADS=${BENCH_THREADS:-3}
SCALE=${BENCH_SCALE:-10}
INLIST_N=${BENCH_INLIST_N:-10000}
WINDOW_MS=${BENCH_WINDOW_MS:-5}
ONLY=${BENCH_ONLY:-}
KEEP_LOGS=${BENCH_KEEP_LOGS:-0}
MAX_FOREIGN=${BENCH_MAX_FOREIGN:-0.75}
ATTEMPTS=${BENCH_ATTEMPTS:-3}
CLK_TCK=$(getconf CLK_TCK)
NCPU=$(nproc)
if [ "$NCPU" -ge 10 ]; then
	SERVER_CPUS=${BENCH_SERVER_CPUS:-0-4}
	CLIENT_CPUS=${BENCH_CLIENT_CPUS:-5-7}
elif [ "$NCPU" -ge 6 ]; then
	SERVER_CPUS=${BENCH_SERVER_CPUS:-0-$((NCPU / 2 - 1))}
	CLIENT_CPUS=${BENCH_CLIENT_CPUS:-$((NCPU / 2))-$((NCPU - 1))}
else
	SERVER_CPUS=${BENCH_SERVER_CPUS:-0-$((NCPU - 1))}
	CLIENT_CPUS=${BENCH_CLIENT_CPUS:-0-$((NCPU - 1))}
fi
PGBIN=$(pg_config --bindir)
export PYTHONDONTWRITEBYTECODE=1
EXT=pg_stat_statement_context
preload_list() {
	case $1 in
	none) echo "" ;;
	pgss) echo "pg_stat_statements" ;;
	ext) echo "pg_stat_statements, $EXT" ;;
	esac
}

as_pg() { gosu postgres "$@"; }
step() { printf '\n=== %s\n' "$*"; }
fail() {
	echo "FAIL: $*" >&2
	cp -f "$WORK"/server.log "$OUT"/ 2>/dev/null || true
	exit 1
}
q() { as_pg "$PGBIN/psql" -X -At -v ON_ERROR_STOP=1 -h "$SOCK" -d bench -c "$1"; }
pg_start() {
	taskset -c "$SERVER_CPUS" gosu postgres "$PGBIN/pg_ctl" -D "$DATA" -l "$WORK/server.log" -w start >/dev/null \
		|| fail "server start ($(tail -n 5 "$WORK/server.log"))"
}
pg_stop() { as_pg "$PGBIN/pg_ctl" -D "$DATA" -m fast -w stop >/dev/null; }
# "<VM busy jiffies> <this container's CPU usec> <wall ns>"
cpu_sample() {
	local ours=0
	[ -r /sys/fs/cgroup/cpu.stat ] && ours=$(awk '/^usage_usec/ {print $2}' /sys/fs/cgroup/cpu.stat)
	echo "$(awk '/^cpu / {print $2 + $3 + $4 + $7 + $8 + $9}' /proc/stat) $ours $(date +%s%N)"
}
# CPU cores used outside this container between two cpu_sample()s
foreign_cores() {
	[ -r /sys/fs/cgroup/cpu.stat ] || { echo null; return; }
	echo "$1 $2" | awk -v hz="$CLK_TCK" '{
		f = (($4 - $1) * 1e6 / hz - ($5 - $2)) / (($6 - $3) / 1e3);
		printf "%.2f\n", f < 0 ? 0 : f }'
}
pgbench() { taskset -c "$CLIENT_CPUS" gosu postgres "$PGBIN/pgbench" -h "$SOCK" "$@"; }

mkdir -p "$OUT/runs" "$WORK"
chown postgres:postgres "$WORK"

step "analysis self-test"
python3 "$SRC/bench/analyze.py" --self-test 2>&1 | tail -n 1
python3 "$SRC/bench/analyze.py" --self-test >/dev/null 2>&1 || fail "bench/analyze.py self-test"

step "build and install $EXT ($(pg_config --version))"
rm -rf /build && mkdir -p /build
tar -C "$SRC" --exclude=./.git --exclude=./tmp --exclude='*.o' --exclude='*.so' \
	--exclude='*.dylib' --exclude='*.bc' --exclude=./fuzz --exclude=./test -cf - . | tar -C /build -xf -
make -C /build -s clean >/dev/null
make -C /build -s >/dev/null || fail "make"
make -C /build -s install >/dev/null || fail "make install"

step "environment"
{
	echo "pg_config --version: $(pg_config --version)"
	echo "pgbench: $("$PGBIN/pgbench" --version)"
	echo "container nproc: $NCPU; server CPUs: $SERVER_CPUS; pgbench CPUs: $CLIENT_CPUS"
	echo "kernel: $(uname -srm)"
	grep -m1 MemTotal /proc/meminfo
	lscpu 2>/dev/null | grep -E '^(Architecture|Model name|CPU\(s\)|Vendor ID|BogoMIPS)' || true
	echo "pgbench: -M simple -c $CLIENTS -j $THREADS -T $DURATION (warmup ${WARMUP}s), scale $SCALE, Unix socket"
	echo "runs per config: $RUNS (round-robin, odd rounds forward, even rounds reversed; restart before each run); IN list: $INLIST_N elements"
	echo "boundary windows: +-${WINDOW_MS} ms on a 1 s grid"
} | tee "$OUT/env-container.txt"

step "initdb + pgbench -i -s $SCALE"
rm -rf "$DATA" "$LOGS" "$SCRIPTS"
mkdir -p "$LOGS" "$SCRIPTS"
chown -R postgres:postgres "$WORK"
as_pg "$PGBIN/initdb" -D "$DATA" -A trust --no-sync -E UTF8 --locale=C >/dev/null || fail initdb
cat >> "$DATA/postgresql.conf" <<-EOF
	listen_addresses = ''
	max_connections = 50
	shared_buffers = 1GB
	autovacuum = off
	jit = off
	logging_collector = off
	include 'bench.conf'
EOF
write_conf() {
	# $1 = preload list, $2 = ';'-separated extra settings
	{
		echo "shared_preload_libraries = '$1'"
		local IFS=';'
		for s in $2; do [ -n "$s" ] && echo "$s"; done
	} > "$DATA/bench.conf"
	chown postgres:postgres "$DATA/bench.conf"
}
write_conf "$(preload_list ext)" ""
pg_start
as_pg "$PGBIN/createdb" -h "$SOCK" bench
as_pg "$PGBIN/pgbench" -h "$SOCK" -i -q -s "$SCALE" bench 2>&1 | tail -n 1
for sql in "CREATE EXTENSION pg_stat_statements" "CREATE EXTENSION $EXT" "VACUUM ANALYZE" "CHECKPOINT"; do
	q "$sql" >/dev/null
done
q "SELECT name || ' = ' || current_setting(name) FROM pg_settings
   WHERE name IN ('shared_buffers', 'max_connections', 'jit', 'autovacuum', 'compute_query_id',
                  'pg_stat_statements.max', 'pg_stat_statements.track', 'pg_stat_statements.track_planning',
                  'pg_stat_statements.track_utility')
      OR name LIKE '$EXT.%' ORDER BY name" | sed 's/^/setting: /' | tee -a "$OUT/env-container.txt"
pg_stop

# Workloads. pgbench sends a statement's trailing ';' (stmt_len > 0), and a
# last statement without one is sent as is: stmt_len = 0, the strlen() case.
COMMENT="/*action='show',controller='users',db_driver='psycopg2',framework='django%3A4.2.1',route='%2Fusers%2F%3Cint%3Aid%3E',traceparent='00-5bd66ef5095369c7b0d1f8f4bd33716a-c532cb4098ac3dd2-01'*/"
ECOMMENT="/*action='show',controller='users:r',db_driver='psycopg2',framework='django%3A4.2.1',traceparent='00-5bd66ef5095369c7b0d1f8f4bd33716a-c532cb4098ac3dd2-01'*/"
SET_AID='\set aid random(1, 100000 * :scale)'
Q="SELECT abalance FROM pgbench_accounts WHERE aid = :aid"
INLIST=$(seq -s, -"$INLIST_N" -1)
printf '%s\n%s;\n' "$SET_AID" "$Q" > "$SCRIPTS/plain.sql"
printf '%s\n%s %s;\n' "$SET_AID" "$Q" "$COMMENT" > "$SCRIPTS/append.sql"
printf '%s\n%s %s;\n' "$SET_AID" "$COMMENT" "$Q" > "$SCRIPTS/prepend.sql"
printf '%s\n%s AND abalance NOT IN (%s) %s;\n' "$SET_AID" "$Q" "$INLIST" "$COMMENT" > "$SCRIPTS/inlist.sql"
printf '%s\n%s AND abalance NOT IN (%s) %s\n' "$SET_AID" "$Q" "$INLIST" "$COMMENT" > "$SCRIPTS/inlist0.sql"
printf '%s\n\\set r random(1, 1000000000)\n%s %s;\n' "$SET_AID" "$Q" "$ECOMMENT" > "$SCRIPTS/evict.sql"
grep -q ';$' "$SCRIPTS/inlist0.sql" && fail "inlist0.sql must not end in ';' (stmt_len = 0 case)"
chown -R postgres:postgres "$SCRIPTS"
wc -c "$SCRIPTS"/*.sql | sed 's/^/script bytes: /' | tee -a "$OUT/env-container.txt"

# 1 s buckets, enough of them that the whole run stays live (exact call checks)
BOUNDARY_BUCKETS=$((DURATION + 30))
ANY="$EXT.extractors = 'sqlcommenter(position=any), marginalia(position=any)'"
PREPEND="$EXT.extractors = 'sqlcommenter(position=prepend), marginalia(position=prepend)'"
# name | workload | preload | extra settings (';'-separated) | checks (','-separated) | baseline
#   [| reader]   activity: a session reads ${EXT}_activity every 1 ms during
#                the measured run (a monitoring stress test, check "reader")
CONFIGS=(
	"plain/none|plain|none||none|plain/pgss"
	"plain/pgss|plain|pgss||pgss|plain/pgss"
	"plain/ext|plain|ext||untagged|plain/pgss"
	"append/none|append|none||none|append/pgss"
	"append/pgss|append|pgss||pgss|append/pgss"
	"append/ext|append|ext||tagged|append/pgss"
	"append/ext-any|append|ext|$ANY|tagged|append/pgss"
	"append/ext-activity-reader|append|ext||tagged,reader|append/pgss|activity"
	"append/ext-1s-buckets|append|ext|$EXT.bucket_interval = '1s';$EXT.bucket_count = $BOUNDARY_BUCKETS|tagged,buckets|append/pgss"
	"prepend/pgss|prepend|pgss||pgss|prepend/pgss"
	"prepend/ext|prepend|ext|$PREPEND|tagged|prepend/pgss"
	"inlist/pgss|inlist|pgss||pgss|inlist/pgss"
	"inlist/ext|inlist|ext||tagged,heuristic|inlist/pgss"
	"inlist/ext-any|inlist|ext|$ANY|tagged,exact|inlist/pgss"
	"inlist/ext-window-1MB|inlist|ext|$EXT.scan_window = '1MB'|tagged,exact|inlist/pgss"
	"inlist0/pgss|inlist0|pgss||pgss|inlist0/pgss"
	"inlist0/ext|inlist0|ext||tagged,heuristic|inlist0/pgss"
	"inlist0/ext-any|inlist0|ext|$ANY|tagged,exact|inlist0/pgss"
	"evict/pgss|evict|pgss||pgss|evict/pgss"
	"evict/ext-max1000|evict|ext|$EXT.max_entries = 1000|evict|evict/pgss"
	"evict/ext-max10000|evict|ext|$EXT.max_entries = 10000|evict|evict/pgss"
)

# Runs the checks for one measured run; prints a JSON object, fails on error.
check_run() {
	local preload=$1 checks=$2 txns=$3 spl calls pcalls rows info c json=""
	add() { json="$json${json:+, }\"$1\": $2"; }
	spl=$(q "SHOW shared_preload_libraries")
	[ "$spl" = "$(preload_list "$preload")" ] || fail "shared_preload_libraries is '$spl'"
	add txns "$txns"
	[ "$txns" -gt 0 ] || fail "no transactions"
	IFS=',' read -ra cs <<< "$checks"
	for c in "${cs[@]}"; do
		case $c in
		none) ;;
		pgss)
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements WHERE query LIKE '%pgbench_accounts%'")
			add pgss_calls "$pcalls"
			[ "$pcalls" -ge "$txns" ] || fail "pgss recorded $pcalls calls < $txns transactions"
			;;
		untagged)
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements WHERE query LIKE '%pgbench_accounts%'")
			rows=$(q "SELECT count(*) FROM ${EXT}_totals")
			add pgss_calls "$pcalls"; add ext_rows "$rows"
			[ "$pcalls" -ge "$txns" ] || fail "pgss recorded $pcalls calls < $txns transactions"
			[ "$rows" = 0 ] || fail "untagged statements recorded ($rows rows) with untagged = skip"
			;;
		tagged)
			calls=$(q "SELECT coalesce(sum(calls), 0) FROM ${EXT}_totals
			           WHERE toplevel AND tags = '{\"action\": \"show\", \"controller\": \"users\"}'")
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements
			            WHERE queryid IN (SELECT queryid FROM ${EXT}_totals)")
			rows=$(q "SELECT count(*) FROM ${EXT}_totals")
			add ext_calls "$calls"; add pgss_calls "$pcalls"; add ext_rows "$rows"
			[ "$calls" -ge "$txns" ] && [ "$calls" -le $((txns + CLIENTS)) ] \
				|| fail "tagged calls $calls not in [$txns, $txns + $CLIENTS]: comment produced no tags?"
			[ "$calls" = "$pcalls" ] || fail "tagged calls $calls != pgss calls $pcalls"
			[ "$rows" = 1 ] || fail "expected one tagged entry, got $rows"
			;;
		heuristic | exact)
			info=$(q "SELECT heuristic_scans FROM ${EXT}_info()")
			add heuristic_scans "$info"
			if [ "$c" = heuristic ]; then
				[ "$info" -ge "$txns" ] || fail "heuristic_scans $info < $txns: tail path not used"
			else
				[ "$info" = 0 ] || fail "heuristic_scans $info != 0: exact path expected"
			fi
			;;
		buckets)
			rows=$(q "SELECT count(DISTINCT bucket_start) FROM $EXT")
			add live_buckets "$rows"
			# 1 s buckets covering the whole run: one live bucket per second
			c=$((DURATION - 2))
			[ "$rows" -ge "$c" ] && [ "$rows" -ge 2 ] || fail "only $rows live buckets (expected >= $c): no bucket rollovers?"
			;;
		reader)
			# "reads active_tagged" from the reader's NOTICE
			read -r c rows <<< "$(sed -n 's/.*NOTICE:  reads=\([0-9]*\) active_tagged=\([0-9]*\).*/\1 \2/p' "$READER_OUT")"
			add reader_reads "${c:-0}"; add reader_active_tagged "${rows:-0}"
			[ "${c:-0}" -gt 0 ] || fail "the activity reader made no reads"
			[ "${rows:-0}" -gt 0 ] || fail "the activity reader saw no active tagged statement"
			;;
		evict)
			info=$(q "SELECT dealloc || ' ' || evicted_entries || ' ' || entries || ' ' || max_entries FROM ${EXT}_info()")
			read -r d e n m <<< "$info"
			rows=$(q "SELECT count(DISTINCT tags->>'controller') FROM ${EXT}_totals")
			add dealloc "$d"; add evicted_entries "$e"; add entries "$n"; add distinct_controllers "$rows"
			[ "$d" -gt 0 ] && [ "$e" -gt 0 ] || fail "no evictions (dealloc $d, evicted_entries $e)"
			[ "$n" -le "$m" ] || fail "entries $n > max_entries $m"
			[ "$rows" -gt 100 ] || fail "only $rows distinct controller values: random tag not substituted?"
			;;
		*) fail "unknown check $c" ;;
		esac
	done
	echo "{$json}"
}

# Reads the activity view every 1 ms for $DURATION s (in the background,
# from a server backend), then reports its reads and the active tagged
# rows it saw.
READER_OUT=$WORK/reader.out
start_reader() {
	as_pg "$PGBIN/psql" -X -q -h "$SOCK" -d bench -v ON_ERROR_STOP=1 -c "
	DO \$\$
	DECLARE n bigint := 0; a bigint := 0; c bigint;
	        stop timestamptz := clock_timestamp() + interval '$DURATION s';
	BEGIN
	  WHILE clock_timestamp() < stop LOOP
	    SELECT count(*) FILTER (WHERE state = 'active' AND tags ? 'controller')
	      INTO c FROM ${EXT}_activity;
	    a := a + c; n := n + 1;
	    PERFORM pg_sleep(0.001);
	  END LOOP;
	  RAISE NOTICE 'reads=% active_tagged=%', n, a;
	END \$\$" > "$READER_OUT" 2>&1 &
	READER_PID=$!
}

TOTAL=0
for cfg in "${CONFIGS[@]}"; do
	[[ -z $ONLY || ${cfg%%|*} =~ $ONLY ]] && TOTAL=$((TOTAL + 1))
done
[ "$TOTAL" -gt 0 ] || fail "no configuration matches BENCH_ONLY='$ONLY'"
step "$TOTAL configurations x $RUNS runs, ~$(((TOTAL * RUNS * (DURATION + WARMUP + 6) + 59) / 60)) min"

for run in $(seq 1 "$RUNS"); do
	# Even rounds run in reverse order: bench/analyze.py combines rounds 2k-1
	# and 2k into a balanced pair, so a drift that is smooth in time does not
	# favor any position (an odd last round is left out of the deltas).
	if [ $((run % 2)) = 1 ]; then idx=$(seq 0 $((${#CONFIGS[@]} - 1))); else idx=$(seq $((${#CONFIGS[@]} - 1)) -1 0); fi
	for i in $idx; do
		cfg=${CONFIGS[$i]}
		IFS='|' read -r name workload preload settings checks baseline reader <<< "$cfg"
		order=$((i + 1))
		[[ -z $ONLY || $name =~ $ONLY ]] || continue
		write_conf "$(preload_list "$preload")" "$settings"
		pg_start
		script=$SCRIPTS/$workload.sql
		tag="$(printf '%02d' "$order")-${name//\//_}-r$run"
		for attempt in $(seq 1 "$ATTEMPTS"); do
			pgbench -n -M simple -c "$CLIENTS" -j "$THREADS" -T "$WARMUP" -f "$script" bench >/dev/null 2>&1 \
				|| fail "$name: warmup pgbench failed"
			if [ "$preload" != none ]; then q "SELECT pg_stat_statements_reset()" >/dev/null; fi
			if [ "$preload" = ext ]; then q "SELECT ${EXT}_reset()" >/dev/null; fi
			rm -f "$LOGS"/*
			before=$(cpu_sample)
			if [ "$reader" = activity ]; then start_reader; fi
			if ! pgbench -n -M simple -c "$CLIENTS" -j "$THREADS" -T "$DURATION" -f "$script" \
				--log --log-prefix="$LOGS/tx" bench > "$OUT/runs/$tag.pgbench.txt" 2>&1; then
				cat "$OUT/runs/$tag.pgbench.txt" >&2
				fail "$name: pgbench failed"
			fi
			if [ "$reader" = activity ]; then
				wait "$READER_PID" || { cat "$READER_OUT" >&2; fail "$name: activity reader failed"; }
			fi
			foreign=$(foreign_cores "$before" "$(cpu_sample)")
			if [ "$foreign" = null ] || [ "$attempt" = "$ATTEMPTS" ] \
				|| awk -v f="$foreign" -v m="$MAX_FOREIGN" 'BEGIN { exit !(f <= m) }'; then
				break
			fi
			echo "  $name: other containers used $foreign CPU cores during the run; retrying"
		done
		txns=$(sed -n 's/^number of transactions actually processed: \([0-9]*\).*/\1/p' "$OUT/runs/$tag.pgbench.txt")
		checks_json=$(check_run "$preload" "$checks" "$txns")
		pg_stop
		grep -E "PANIC|terminated by signal|server process .* was terminated" "$WORK/server.log" \
			&& fail "$name: crash in server log"
		meta=$(printf '{"config": "%s", "workload": "%s", "baseline": "%s", "run": %d, "order": %d, "foreign_cores": %s, "attempts": %d, "checks": %s}' \
			"$name" "$workload" "$baseline" "$run" "$order" "$foreign" "$attempt" "$checks_json")
		python3 "$SRC/bench/analyze.py" run --meta "$meta" --pgbench-out "$OUT/runs/$tag.pgbench.txt" \
			--out "$OUT/runs/$tag.json" --window-ms "$WINDOW_MS" "$LOGS"/tx.* || fail "$name: analysis"
		if [ "$KEEP_LOGS" = 1 ]; then
			cat "$LOGS"/tx.* | gzip -1 > "$OUT/runs/$tag.log.gz"
		fi
		rm -f "$LOGS"/*
	done
done

step "report"
python3 "$SRC/bench/analyze.py" report --out-md "$OUT/results.md" --out-json "$OUT/results.json" "$OUT"/runs/*.json
cat "$OUT/results.md"
step "ALL CHECKS PASSED"
