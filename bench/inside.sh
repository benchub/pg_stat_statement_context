#!/bin/bash
# Runs inside the docker/Dockerfile image (pg_stat_statement_context-test:pgN-<hash>),
# started by bench/run.sh. Builds and installs the extension from /src (release
# build: plain `make`), creates a pgbench database, and runs the plan written
# by bench/scenarios.py: every configuration of every scenario once per block,
# in a seeded random order. Each run restarts the server with the run's
# settings, prewarms the tables, runs a warmup, resets the statistics, then the
# measured run. Server CPU (utime + stime of the postmaster and all its
# children, live or reaped) is sampled around the measured run. Each run is
# checked (preload list, tags actually recorded, evictions, rollovers, reader
# reads) and summarized by bench/analyze.py; any failed check exits non-zero.
#
# Environment (bench/run.sh sets them from its options):
#   BENCH_BLOCKS=8 BENCH_DURATION=12 BENCH_WARMUP=3  blocks, seconds per run
#   BENCH_SEED=1                                     plan order seed
#   BENCH_ONLY=<regex>                               scenarios to run
#   BENCH_HIGH_CLIENTS=0                             1: add the 256-client scenarios
#   BENCH_SCALE=10 BENCH_INLIST_N=10000              pgbench -s, IN-list length
#   BENCH_WINDOW_MS=5                                boundary window (+-ms)
#   BENCH_SERVER_CPUS BENCH_CLIENT_CPUS              taskset CPU lists; default:
#                                                    of the N CPUs the container
#                                                    may use, the first N/2 for
#                                                    the server and the next
#                                                    3N/10 for pgbench (5 and 3
#                                                    of 10), the rest idle
#   BENCH_KEEP_LOGS=1                                keep gzipped pgbench logs
#   BENCH_MAX_FOREIGN=0.75 BENCH_ATTEMPTS=3          retry a run when other
#                                                   containers used more than
#                                                   this many CPU cores during it
#
# "CPU count" in the scenario names (c1, cN, c4N) is the number of server CPUs.
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
BLOCKS=${BENCH_BLOCKS:-8}
DURATION=${BENCH_DURATION:-12}
WARMUP=${BENCH_WARMUP:-3}
SEED=${BENCH_SEED:-1}
ONLY=${BENCH_ONLY:-}
HIGH=${BENCH_HIGH_CLIENTS:-0}
SCALE=${BENCH_SCALE:-10}
INLIST_N=${BENCH_INLIST_N:-10000}
WINDOW_MS=${BENCH_WINDOW_MS:-5}
KEEP_LOGS=${BENCH_KEEP_LOGS:-0}
MAX_FOREIGN=${BENCH_MAX_FOREIGN:-0.75}
ATTEMPTS=${BENCH_ATTEMPTS:-3}
CLK_TCK=$(getconf CLK_TCK)
PGBIN=$(pg_config --bindir)
export PYTHONDONTWRITEBYTECODE=1
EXT=pg_stat_statement_context

# The CPUs this container may use (honors docker run --cpuset-cpus).
expand_cpus() {
	local IFS=, part out=()
	for part in $1; do
		if [[ $part == *-* ]]; then out+=($(seq "${part%-*}" "${part#*-}")); else out+=("$part"); fi
	done
	echo "${out[@]}"
}
count_cpus() { set -- $(expand_cpus "$1"); echo $#; }
read -ra ALLOWED <<< "$(expand_cpus "$(taskset -pc $$ | sed 's/.*: //')")"
NALLOWED=${#ALLOWED[@]}
if [ "$NALLOWED" -ge 6 ]; then
	ns=$((NALLOWED / 2)) nc=$((NALLOWED * 3 / 10))
	SERVER_CPUS=${BENCH_SERVER_CPUS:-$(IFS=,; echo "${ALLOWED[*]:0:ns}")}
	CLIENT_CPUS=${BENCH_CLIENT_CPUS:-$(IFS=,; echo "${ALLOWED[*]:ns:nc}")}
else
	SERVER_CPUS=${BENCH_SERVER_CPUS:-$(IFS=,; echo "${ALLOWED[*]}")}
	CLIENT_CPUS=${BENCH_CLIENT_CPUS:-$SERVER_CPUS}
fi
NSERVER=$(count_cpus "$SERVER_CPUS")
NCLIENT=$(count_cpus "$CLIENT_CPUS")

preload_list() {
	case $1 in
	pgss) echo "pg_stat_statements" ;;
	ext) echo "pg_stat_statements, $EXT" ;;
	*) echo "unknown preload $1" >&2; exit 1 ;;
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
# Server CPU in clock ticks: utime + stime of the postmaster and its live
# children (zombies included), plus cutime + cstime of the postmaster, which
# holds the CPU of the children it has reaped (backends that exited).
server_ticks() {
	local pm
	pm=$(head -n 1 "$DATA/postmaster.pid")
	# comm is "(postgres)": no spaces, so the fields are fixed (proc(5))
	cat /proc/[0-9]*/stat 2>/dev/null | awk -v pm="$pm" '
		$1 == pm { s += $14 + $15 + $16 + $17 }
		$4 == pm { s += $14 + $15 }
		END { print s + 0 }'
}
# Waits until no client backend is left (pgbench's backends exit asynchronously).
wait_backends() {
	local i
	for i in $(seq 1 100); do
		grep -l -s -a '\[local\]' /proc/[0-9]*/cmdline >/dev/null 2>&1 || return 0
		sleep 0.05
	done
	echo "  warning: client backends still alive after 5 s" >&2
}
pgbench() { taskset -c "$CLIENT_CPUS" gosu postgres "$PGBIN/pgbench" -h "$SOCK" "$@"; }

mkdir -p "$OUT/runs" "$WORK"
chown postgres:postgres "$WORK"

step "analysis self-test"
python3 "$SRC/bench/analyze.py" --self-test 2>&1 | tail -n 1
python3 "$SRC/bench/analyze.py" --self-test >/dev/null 2>&1 || fail "bench/analyze.py self-test"

step "build and install $EXT ($(pg_config --version), release build)"
rm -rf /build && mkdir -p /build
tar -C "$SRC" --exclude=./.git --exclude=./tmp --exclude=./worktrees --exclude='*.o' --exclude='*.so' \
	--exclude='*.dylib' --exclude='*.bc' --exclude=./fuzz --exclude=./test --exclude=./bench/results \
	-cf - . | tar -C /build -xf -
make -C /build -s clean >/dev/null
make -C /build -s >/dev/null || fail "make"
make -C /build -s install >/dev/null || fail "make install"

step "plan"
python3 "$SRC/bench/scenarios.py" plan --ncpu "$NSERVER" --duration "$DURATION" --blocks "$BLOCKS" \
	--seed "$SEED" ${ONLY:+--only "$ONLY"} $([ "$HIGH" = 1 ] && echo --high-clients) \
	--out-tsv "$OUT/plan.tsv" --out-plan-json "$OUT/plan.json" --out-json "$OUT/scenarios.json" || fail "plan"
NRUNS=$(wc -l < "$OUT/plan.tsv")
MAXCLIENTS=$(cut -f 8 "$OUT/plan.tsv" | sort -n | tail -n 1)

step "environment"
{
	echo "pg_config --version: $(pg_config --version)"
	echo "pgbench: $("$PGBIN/pgbench" --version)"
	echo "container CPUs: ${ALLOWED[*]}; server CPUs: $SERVER_CPUS ($NSERVER); pgbench CPUs: $CLIENT_CPUS ($NCLIENT)"
	echo "kernel: $(uname -srm)"
	grep -m1 MemTotal /proc/meminfo
	lscpu 2>/dev/null | grep -E '^(Architecture|Model name|CPU\(s\)|Vendor ID|BogoMIPS)' || true
	echo "pgbench: -T $DURATION (warmup ${WARMUP}s), -j min(clients, $NCLIENT), scale $SCALE, Unix socket"
	echo "plan: $NRUNS runs, $BLOCKS blocks, seed $SEED${ONLY:+, only '$ONLY'}; IN list: $INLIST_N elements"
	echo "boundary windows: +-${WINDOW_MS} ms on a 1 s grid"
} | tee "$OUT/env-container.txt"
printf '{"pg_version": "%s", "server_cpus": "%s", "client_cpus": "%s", "ncpu": %d, "nclient": %d, "kernel": "%s"}\n' \
	"$(pg_config --version)" "$SERVER_CPUS" "$CLIENT_CPUS" "$NSERVER" "$NCLIENT" "$(uname -srm)" > "$OUT/container.json"

step "initdb + pgbench -i -s $SCALE"
rm -rf "$DATA" "$LOGS" "$SCRIPTS"
mkdir -p "$LOGS" "$SCRIPTS"
chown -R postgres:postgres "$WORK"
as_pg "$PGBIN/initdb" -D "$DATA" -A trust --no-sync -E UTF8 --locale=C >/dev/null || fail initdb
cat >> "$DATA/postgresql.conf" <<-EOF
	listen_addresses = ''
	max_connections = $((MAXCLIENTS + 40))
	shared_buffers = 1GB
	autovacuum = off
	jit = off
	synchronous_commit = off
	max_wal_size = 8GB
	checkpoint_timeout = 30min
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
for sql in "CREATE EXTENSION pg_stat_statements" "CREATE EXTENSION $EXT" "CREATE EXTENSION pg_prewarm" \
	"$(python3 "$SRC/bench/scenarios.py" setup-sql)" "VACUUM ANALYZE" "CHECKPOINT"; do
	q "$sql" >/dev/null
done
q "SELECT name || ' = ' || current_setting(name) FROM pg_settings
   WHERE name IN ('shared_buffers', 'max_connections', 'jit', 'autovacuum', 'compute_query_id',
                  'synchronous_commit', 'max_wal_size', 'checkpoint_timeout',
                  'pg_stat_statements.max', 'pg_stat_statements.track', 'pg_stat_statements.track_planning',
                  'pg_stat_statements.track_utility')
      OR name LIKE '$EXT.%' ORDER BY name" | sed 's/^/setting: /' | tee -a "$OUT/env-container.txt"
pg_stop

python3 "$SRC/bench/scenarios.py" scripts "$SCRIPTS" --inlist-n "$INLIST_N"
python3 "$SRC/bench/scenarios.py" exporter-sql "$SRC/docs/integrations/postgres_exporter/queries.yaml" \
	"$SCRIPTS/exporter.sql"
grep -q ';$' "$SCRIPTS/inlist0.sql" && fail "inlist0.sql must not end in ';' (stmt_len = 0 case)"
chown -R postgres:postgres "$SCRIPTS"
wc -c "$SCRIPTS"/*.sql | sed 's/^/script bytes: /' | tee -a "$OUT/env-container.txt"

TAGS_SHOW='{"action": "show", "controller": "users"}'
TAGS_REGEXNORM='{"action": "show", "controller": "users", "route": "/users/:id", "driver": "psycopg2"}'
# Runs the checks for one measured run; prints a JSON object, fails on error.
check_run() {
	local preload=$1 checks=$2 txns=$3 clients=$4 spl calls pcalls rows info c k n json=""
	add() { json="$json${json:+, }\"$1\": $2"; }
	spl=$(q "SHOW shared_preload_libraries")
	[ "$spl" = "$(preload_list "$preload")" ] || fail "shared_preload_libraries is '$spl'"
	add txns "$txns"
	[ "$txns" -gt 0 ] || fail "no transactions"
	IFS=',' read -ra cs <<< "$checks"
	for c in "${cs[@]}"; do
		case $c in
		pgss)
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements")
			add pgss_calls "$pcalls"
			[ "$pcalls" -ge "$txns" ] || fail "pgss recorded $pcalls calls < $txns transactions"
			;;
		untagged)
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements")
			rows=$(q "SELECT count(*) FROM ${EXT}_totals")
			add pgss_calls "$pcalls"; add ext_rows "$rows"
			[ "$pcalls" -ge "$txns" ] || fail "pgss recorded $pcalls calls < $txns transactions"
			[ "$rows" = 0 ] || fail "untagged statements recorded ($rows rows) with untagged = skip"
			;;
		tagged:* | regexnorm:*)
			k=${c#*:}
			if [[ $c == tagged:* ]]; then tags=$TAGS_SHOW; else tags=$TAGS_REGEXNORM; fi
			calls=$(q "SELECT coalesce(sum(calls), 0) FROM ${EXT}_totals WHERE toplevel AND tags @> '$tags'")
			pcalls=$(q "SELECT coalesce(sum(calls), 0) FROM pg_stat_statements WHERE toplevel
			            AND queryid IN (SELECT queryid FROM ${EXT}_totals WHERE toplevel AND tags @> '$tags')")
			add "ext_calls_${c%%:*}" "$calls"; add pgss_calls "$pcalls"
			[ "$calls" -ge $((k * txns)) ] && [ "$calls" -le $((k * (txns + clients))) ] \
				|| fail "$c: tagged calls $calls not in [$k * $txns, $k * ($txns + $clients)]: no tags?"
			[ "$calls" = "$pcalls" ] || fail "$c: tagged calls $calls != pgss calls $pcalls"
			;;
		nested:*:*)
			read -r _ mode k <<< "${c//:/ }"
			if [ "$mode" = inherit ]; then tags=$TAGS_SHOW; else tags='{"controller": "nested"}'; fi
			calls=$(q "SELECT coalesce(sum(calls), 0) FROM ${EXT}_totals WHERE NOT toplevel AND tags @> '$tags'")
			add nested_calls "$calls"
			[ "$calls" -ge $((k * txns)) ] || fail "$c: nested tagged calls $calls < $k * $txns"
			;;
		store:*)
			info=$(q "SELECT entries || ' ' || dealloc FROM ${EXT}_info()")
			read -r n d <<< "$info"
			add entries "$n"; add dealloc "$d"
			[ "$n" -gt "${c#store:}" ] || fail "store: $n entries, expected more than ${c#store:}"
			[ "$d" = 0 ] || fail "store: $d eviction passes"
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
			n=$((DURATION - 2))
			[ "$rows" -ge "$n" ] && [ "$rows" -ge 2 ] || fail "only $rows live buckets (expected >= $n): no rollovers?"
			;;
		reader)
			read -r n rows < "$READER_OUT" || true
			add reader_reads "${n:-0}"; add reader_rows "${rows:-0}"
			[ "${n:-0}" -gt 0 ] || fail "the exporter reader made no reads"
			[ "${rows:-0}" -gt 0 ] || fail "the exporter queries returned no rows"
			;;
		evict)
			info=$(q "SELECT dealloc || ' ' || evicted_entries || ' ' || reclaimed_entries || ' ' || entries || ' ' || max_entries FROM ${EXT}_info()")
			read -r d e r n m <<< "$info"
			rows=$(q "SELECT count(DISTINCT tags->>'controller') FROM ${EXT}_totals")
			add dealloc "$d"; add evicted_entries "$e"; add entries "$n"; add distinct_controllers "$rows"
			[ "$d" -gt 0 ] && [ $((e + r)) -gt 0 ] || fail "no evictions (dealloc $d, evicted_entries $e, reclaimed_entries $r)"
			[ "$n" -le "$m" ] || fail "entries $n > max_entries $m"
			[ "$rows" -gt 100 ] || fail "only $rows distinct controller values: random tag not substituted?"
			;;
		*) fail "unknown check $c" ;;
		esac
	done
	echo "{$json}"
}

# Runs the exporter recipe's queries (docs/integrations/postgres_exporter)
# every $1 s during the measured run, the first after half an interval (so a
# 15 s reader reads inside a shorter run too), each from a new connection
# like a scrape; then writes "<reads> <rows returned>".
READER_OUT=$WORK/reader.out
start_reader() {
	local every=$1
	(
		start=$(date +%s%N) n=0 rows=0
		end=$((start + DURATION * 1000000000))
		next=$((start + every * 500000000))
		while :; do
			now=$(date +%s%N)
			[ "$next" -lt "$end" ] || break
			[ "$now" -lt "$next" ] && sleep "$(awk -v d=$((next - now)) 'BEGIN { printf "%.3f", d / 1e9 }')"
			c=$(taskset -c "$CLIENT_CPUS" gosu postgres "$PGBIN/psql" -X -At -v ON_ERROR_STOP=1 -h "$SOCK" \
				-d bench -f "$SCRIPTS/exporter.sql" | wc -l) || exit 1
			n=$((n + 1)) rows=$((rows + c))
			next=$((next + every * 1000000000))
		done
		echo "$n $rows" > "$READER_OUT"
	) &
	READER_PID=$!
}

# Fills the store with $1 distinct tag sets (one entry each), as top-level
# statements.
prepopulate() {
	printf "SELECT format('SELECT 1 /*action=''fill'',controller=''c%%s''*/', g) FROM generate_series(1, %d) g \\\\gexec\n" "$1" \
		| as_pg "$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCK" -d bench >/dev/null || fail "prepopulate"
}

step "$NRUNS runs, ~$(((NRUNS * (DURATION + WARMUP + 9) + 59) / 60)) min"
T0=$(date +%s)
while IFS=$'\t' read -r -u 3 seq block scenario config baseline script protocol clients preload settings checks extra stmts; do
	[ "$settings" = - ] && settings=
	[ "$extra" = - ] && extra=
	write_conf "$(preload_list "$preload")" "$settings"
	pg_start
	q "SELECT pg_prewarm('pgbench_accounts'), pg_prewarm('pgbench_accounts_pkey')" >/dev/null
	if [[ $script == rw* ]]; then
		q "TRUNCATE pgbench_history" >/dev/null
		q "VACUUM pgbench_accounts, pgbench_tellers, pgbench_branches" >/dev/null
	fi
	threads=$((clients < NCLIENT ? clients : NCLIENT))
	sfile=$SCRIPTS/$script.sql
	tag="$(printf '%04d' "$seq")-b$block-$scenario-$config"
	for attempt in $(seq 1 "$ATTEMPTS"); do
		pgbench -n -M "$protocol" -c "$clients" -j "$threads" -T "$WARMUP" -f "$sfile" bench >/dev/null 2>&1 \
			|| fail "$scenario/$config: warmup pgbench failed"
		q "SELECT pg_stat_statements_reset()" >/dev/null
		if [ "$preload" = ext ]; then q "SELECT ${EXT}_reset()" >/dev/null; fi
		if [[ $extra == store:* ]]; then prepopulate "${extra#store:}"; fi
		wait_backends
		rm -f "$LOGS"/* "$READER_OUT"
		before=$(cpu_sample)
		ticks0=$(server_ticks)
		if [[ $extra == reader:* ]]; then start_reader "${extra#reader:}"; fi
		if ! pgbench -n -M "$protocol" -c "$clients" -j "$threads" -T "$DURATION" -f "$sfile" \
			--log --log-prefix="$LOGS/tx" bench > "$OUT/runs/$tag.pgbench.txt" 2>&1; then
			cat "$OUT/runs/$tag.pgbench.txt" >&2
			fail "$scenario/$config: pgbench failed"
		fi
		if [[ $extra == reader:* ]]; then wait "$READER_PID" || fail "$scenario/$config: exporter reader failed"; fi
		wait_backends
		ticks1=$(server_ticks)
		foreign=$(foreign_cores "$before" "$(cpu_sample)")
		if [ "$foreign" = null ] || [ "$attempt" = "$ATTEMPTS" ] \
			|| awk -v f="$foreign" -v m="$MAX_FOREIGN" 'BEGIN { exit !(f <= m) }'; then
			break
		fi
		echo "  $scenario/$config: other containers used $foreign CPU cores during the run; retrying"
	done
	txns=$(sed -n 's/^number of transactions actually processed: \([0-9]*\).*/\1/p' "$OUT/runs/$tag.pgbench.txt")
	checks_json=$(check_run "$preload" "$checks" "$txns" "$clients")
	pg_stop
	grep -E "PANIC|terminated by signal|server process .* was terminated" "$WORK/server.log" \
		&& fail "$scenario/$config: crash in server log"
	meta=$(printf '{"foreign_cores": %s, "attempts": %d, "checks": %s, "server_cpu_ticks": %d, "clk_tck": %d, "duration_s": %d, "txns": %d}' \
		"$foreign" "$attempt" "$checks_json" $((ticks1 - ticks0)) "$CLK_TCK" "$DURATION" "$txns")
	printf '[%d/%d, %d min] ' "$seq" "$NRUNS" $((($(date +%s) - T0) / 60))
	python3 "$SRC/bench/analyze.py" run --meta "$meta" --plan "$OUT/plan.json" --seq "$seq" \
		--pgbench-out "$OUT/runs/$tag.pgbench.txt" --out "$OUT/runs/$tag.json" --window-ms "$WINDOW_MS" \
		"$LOGS"/tx.* || fail "$scenario/$config: analysis"
	if [ "$KEEP_LOGS" = 1 ]; then
		cat "$LOGS"/tx.* | gzip -1 > "$OUT/runs/$tag.log.gz"
	fi
	rm -f "$LOGS"/*
done 3< "$OUT/plan.tsv"

step "collect"
python3 "$SRC/bench/analyze.py" collect --out "$OUT/runs.jsonl" "$OUT"/runs/*.json
echo "$(wc -l < "$OUT/runs.jsonl") runs in $OUT/runs.jsonl"
step "ALL CHECKS PASSED"
