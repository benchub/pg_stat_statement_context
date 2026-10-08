#!/bin/bash
# End-to-end test of the integration recipes in docs/integrations/, in Docker.
#
#   scripts/test-integrations.sh [--remove-images] [pg-major]   (default 17)
#
# On a private Docker network it starts:
#   pg     PostgreSQL <pg-major> (official image + PGXS, docker/Dockerfile: this
#          checkout's image of scripts/docker-test.sh <pg-major>) with the
#          extension built from this checkout,
#          10-second buckets, track = all, and the recipe's monitoring role
#   load   a sample tagged workload (test/integrations/workload.sql) in a loop;
#          psql runs it with ON_ERROR_STOP, and the checks fail if it exits
#   pe     postgres_exporter with docs/integrations/postgres_exporter/queries.yaml
#   se     sql_exporter with docs/integrations/sql_exporter/
#   otel   otelcol-contrib with docs/integrations/otel-collector/config.yaml
#   prom   Prometheus scraping the three exporters
#   graf   Grafana with a Prometheus datasource and the provisioned dashboard
#          docs/integrations/grafana/pg_stat_statement_context.json
# and runs test/integrations/check.py, which checks the exported series and
# values, Prometheus, and every dashboard panel query through Grafana's
# /api/ds/query. It then steps the store's clock ahead of now() (as after a
# backward clock step, through the TEST-ONLY pssc_store_test module) and
# checks that the recipes keep following the store's last closed bucket
# rather than one chosen by now(), and recover when the clock catches up.
# Exits non-zero on failure; container logs then land in
# tmp/integrations-<pg-major>/. Everything it creates is removed on exit;
# --remove-images also removes the third-party images it pulled.
# Needs docker, python3 and curl on the host.
set -euo pipefail

# Pinned third-party images (keep in sync with docs/integrations/README.md).
PE_IMAGE=quay.io/prometheuscommunity/postgres-exporter:v0.20.1
SE_IMAGE=burningalchemist/sql_exporter:0.24.9
OTEL_IMAGE=otel/opentelemetry-collector-contrib:0.162.0
PROM_IMAGE=prom/prometheus:v3.15.0
GRAFANA_IMAGE=grafana/grafana:13.2.3

REMOVE_IMAGES=0
if [ "${1:-}" = --remove-images ]; then REMOVE_IMAGES=1; shift; fi
PG_MAJOR=${1:-17}
[[ $PG_MAJOR =~ ^[0-9]+$ ]] || { echo "usage: $0 [--remove-images] [pg-major]" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REC=$ROOT/docs/integrations
TST=$ROOT/test/integrations
OUT=$ROOT/tmp/integrations-$PG_MAJOR
N=pssc-integ-$$
NET=$N
CONTAINERS=(pg load pe se otel prom graf)

for f in "$REC/monitoring-role.sql" "$REC/postgres_exporter/queries.yaml" \
	"$REC/sql_exporter/sql_exporter.yml" "$REC/sql_exporter/pg_stat_statement_context.collector.yml" \
	"$REC/otel-collector/config.yaml" "$REC/grafana/pg_stat_statement_context.json"; do
	[ -f "$f" ] || { echo "FAIL: recipe ${f#$ROOT/} is missing" >&2; exit 1; }
done

mkdir -p "$OUT"
rm -f "$OUT"/*.log

cleanup() {
	local rc=$?
	if [ $rc -ne 0 ]; then
		for c in "${CONTAINERS[@]}"; do
			docker logs "$N-$c" >"$OUT/$c.log" 2>&1 || true
		done
		echo "container logs: ${OUT#$ROOT/}/" >&2
	fi
	for c in "${CONTAINERS[@]}"; do docker rm -fv "$N-$c" >/dev/null 2>&1 || true; done
	docker network rm "$NET" >/dev/null 2>&1 || true
	if [ "$REMOVE_IMAGES" = 1 ]; then
		docker rmi "$PE_IMAGE" "$SE_IMAGE" "$OTEL_IMAGE" "$PROM_IMAGE" "$GRAFANA_IMAGE" >/dev/null 2>&1 || true
	fi
	exit $rc
}
trap cleanup EXIT
trap 'exit 130' INT TERM

step() { printf '=== %s\n' "$*"; }
port() { docker port "$N-$1" "$2/tcp" | head -1; }
running() { [ "$(docker inspect -f '{{.State.Running}}' "$N-$1" 2>/dev/null)" = true ]; }

step "build the PostgreSQL $PG_MAJOR image"
PG_IMAGE=$("$ROOT/scripts/docker-test.sh" --build-image "$PG_MAJOR")

for i in "$PE_IMAGE" "$SE_IMAGE" "$OTEL_IMAGE" "$PROM_IMAGE" "$GRAFANA_IMAGE"; do
	docker image inspect "$i" >/dev/null 2>&1 || { step "pull $i"; docker pull -q "$i" >/dev/null; }
done

docker network create "$NET" >/dev/null

step "start PostgreSQL (builds and installs the extension)"
docker run -d --name "$N-pg" --network "$NET" --network-alias pg --cpus 2 \
	-e POSTGRES_PASSWORD=postgres -v "$ROOT:/src:ro" \
	--entrypoint /src/test/integrations/pg-entrypoint.sh "$PG_IMAGE" >/dev/null
for _ in $(seq 300); do
	# The init scripts run against a socket-only server; TCP means init is done.
	if docker exec "$N-pg" psql -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -U postgres -Atqc \
		"SELECT 1 FROM pg_roles WHERE rolname = 'pssc_monitor'" 2>/dev/null | grep -q 1; then
		break
	fi
	running pg || { echo "FAIL: PostgreSQL container exited" >&2; exit 1; }
	sleep 1
done
docker exec "$N-pg" psql -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -U postgres -Atqc "SELECT version()"

step "start the workload and the collectors"
docker run -d --name "$N-load" --network "$NET" -e PGPASSWORD=app -v "$TST:/w:ro" \
	--entrypoint bash "$PG_IMAGE" -c \
	'while :; do psql -q -X -v ON_ERROR_STOP=1 -h pg -U app -d shop -f /w/workload.sql >/dev/null || exit 1; sleep 0.5; done' >/dev/null

docker run -d --name "$N-pe" --network "$NET" --network-alias pe -p 127.0.0.1::9187 \
	-e DATA_SOURCE_URI="pg:5432/postgres?sslmode=disable" \
	-e DATA_SOURCE_USER=pssc_monitor -e DATA_SOURCE_PASS=monitor \
	-v "$REC/postgres_exporter/queries.yaml:/etc/pssc/queries.yaml:ro" \
	"$PE_IMAGE" --config.file= --extend.query-path=/etc/pssc/queries.yaml >/dev/null

docker run -d --name "$N-se" --network "$NET" --network-alias se -p 127.0.0.1::9399 \
	-e SQLEXPORTER_TARGET_DSN="postgres://pssc_monitor:monitor@pg:5432/postgres?sslmode=disable" \
	-v "$REC/sql_exporter:/etc/sql_exporter:ro" \
	"$SE_IMAGE" --config.file=/etc/sql_exporter/sql_exporter.yml >/dev/null

docker run -d --name "$N-otel" --network "$NET" --network-alias otel -p 127.0.0.1::8889 \
	-e PSSC_PG_HOST=pg -e PSSC_PG_PORT=5432 -e PSSC_PG_DATABASE=postgres \
	-e PSSC_PG_USER=pssc_monitor -e PSSC_PG_PASSWORD=monitor \
	-v "$REC/otel-collector/config.yaml:/etc/otelcol-contrib/config.yaml:ro" \
	"$OTEL_IMAGE" >/dev/null

docker run -d --name "$N-prom" --network "$NET" --network-alias prom -p 127.0.0.1::9090 \
	-v "$TST/prometheus.yml:/etc/prometheus/prometheus.yml:ro" "$PROM_IMAGE" >/dev/null

docker run -d --name "$N-graf" --network "$NET" --network-alias graf -p 127.0.0.1::3000 \
	-e GF_SECURITY_ADMIN_PASSWORD=admin -e GF_ANALYTICS_REPORTING_ENABLED=false \
	-e GF_ANALYTICS_CHECK_FOR_UPDATES=false -e GF_ANALYTICS_CHECK_FOR_PLUGIN_UPDATES=false \
	-e GF_NEWS_NEWS_FEED_ENABLED=false \
	-v "$TST/grafana-provisioning:/etc/grafana/provisioning:ro" \
	-v "$REC/grafana:/var/lib/grafana/dashboards:ro" "$GRAFANA_IMAGE" >/dev/null

sleep 3
for c in load pe se otel prom graf; do
	running "$c" || { echo "FAIL: container $c exited" >&2; exit 1; }
done

check() {
	python3 "$TST/check.py" \
		--postgres-exporter "http://$(port pe 9187)" \
		--sql-exporter "http://$(port se 9399)" \
		--otelcol "http://$(port otel 8889)" \
		--prometheus "http://$(port prom 9090)" \
		--grafana "http://$(port graf 3000)" \
		--require-running "$N-load" "$@"
}
sql() { docker exec "$N-pg" psql -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -U postgres -Atqc "$1"; }

step "check exporters, Prometheus and the Grafana dashboard"
check --timeout "${PSSC_INTEGRATION_TIMEOUT:-240}"

# A backward clock step, as the recipes see it: the store's current bucket
# (a monotonic watermark) ends up ahead of now(). The TEST-ONLY module's
# debug clock is pinned in bucket X, 4 buckets ahead, and then in X + 1,
# before it returns to the real clock. The store's current bucket stays at
# X + 1, ahead of now(), until the clock catches up, and no bucket closes
# meanwhile. The recipes must keep exporting the store's last closed bucket
# X (ahead of now()), not an older bucket chosen by the clock. To tell the
# two apart, tag controller=rollback gets 7 calls in a real (wall-clock)
# bucket just before the step and 30 calls in X: only X gives 3 calls/s.
step "step the clock back (store 5 buckets ahead of now())"
sql "CREATE EXTENSION IF NOT EXISTS pssc_store_test" >/dev/null
seed() {
	sql "SELECT count(pssc_store_test_record(4242 + 0 * n, ARRAY['controller', 'rollback']))
	     FROM generate_series(1, $1) n" >/dev/null
}
seed 7
x=$(sql "SELECT clock_bucket + 4 FROM pssc_store_test_buckets()")
pin() {
	sql "SELECT pssc_store_test_pin_clock(pssc_store_test_bucket_start($1)
	            + (interval_us / 2 || ' microseconds')::interval)
	     FROM pssc_store_test_buckets()" >/dev/null
}
pin "$x"
seed 30
pin "$((x + 1))"
sql "SELECT count(*) FROM pg_stat_statement_context" >/dev/null   # a reader moves the watermark
sleep 3
sql "SELECT pssc_store_test_set_clock_offset(0)" >/dev/null
cur=$(sql "SELECT current_bucket FROM pssc_store_test_buckets()")
[ "$cur" = "$((x + 1))" ] || { echo "FAIL: the store's current bucket is $cur, not X + 1 = $((x + 1))" >&2; exit 1; }
ahead=$(sql "SELECT current_bucket - clock_bucket FROM pssc_store_test_buckets()")
[ "$ahead" -ge 3 ] || { echo "FAIL: the store is only $ahead buckets ahead of the clock" >&2; exit 1; }
check --phase rollback --timeout 15

step "wait for the clock to catch up"
for _ in $(seq 60); do
	[ "$(sql "SELECT current_bucket <= clock_bucket FROM pssc_store_test_buckets()")" = t ] && break
	sleep 1
done
check --phase recovered --timeout "${PSSC_INTEGRATION_TIMEOUT:-240}"
