#!/usr/bin/env bash
# Run every driver repro in Docker and assert the recorded expectations.
#
#   ./run.sh                 # all scenarios
#   ./run.sh pgx_ jdbc_t1    # only scenarios whose name starts with one of the args
#   FLIP=1 ./run.sh pgx_default   # invert expectations: every scenario must FAIL
#
# Each scenario gets a fresh postgres:17 container logging every Parse / Bind /
# Execute (log_min_duration_statement = 0, jsonlog). The driver runs in its own
# container on a private network; lib/check_log.py then compares, for each
# execution, the ctx in the comment of the statement text the server actually
# ran against the ctx the client intended for that call, and asserts the
# verdict (fresh|stale|uncommented|partial) plus exact prepared-state invariants
# (Parses, named statements, cache hits, re-Parses, cross-client sharing)
# recorded in expectations.tsv, then mutation-self-tests the assertion.
#
# Only Docker is required on the host. Download caches live under <repo>/tmp/.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CACHE="$REPO/tmp/driver-prepared-statements"
mkdir -p "$CACHE"/{go,apt,pip,gem,m2,logs}

PG_IMAGE=${PG_IMAGE:-postgres:17}        # Debian trixie, PG 17 + PGDG apt repo
GO_IMAGE=${GO_IMAGE:-golang:1.27}
RUBY_IMAGE=${RUBY_IMAGE:-ruby:3.4}
CHECK_IMAGE=${CHECK_IMAGE:-ruby:3.4}     # any image with python3
PGX_VERSION=v5.11.0
PGJDBC_VERSION=42.7.13
PSYCOPG_VERSION=3.3.6
PGBOUNCER_APT_VERSION=1.26.0-4.pgdg13+1

NET=pssc-drv-$$
PG=$NET-pg
BOUNCER=$NET-bouncer

cleanup() {
  docker rm -f "$PG" "$BOUNCER" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker network create "$NET" >/dev/null

# --- infrastructure ---------------------------------------------------------

start_pg() {
  docker rm -f "$PG" >/dev/null 2>&1 || true
  docker run -d --rm --name "$PG" --network "$NET" \
    -e POSTGRES_HOST_AUTH_METHOD=trust \
    "$PG_IMAGE" \
    -c logging_collector=on -c log_destination=jsonlog \
    -c log_directory=log -c log_filename=pg.log \
    -c log_min_duration_statement=0 -c log_parameter_max_length=-1 >/dev/null
  for _ in $(seq 1 60); do
    docker exec "$PG" pg_isready -q -h 127.0.0.1 -U postgres 2>/dev/null && return 0
    sleep 1
  done
  echo "postgres did not start" >&2; docker logs "$PG" >&2; return 1
}

pg_log() { sleep 1; docker exec "$PG" sh -c 'cat "$PGDATA"/log/pg.json'; }

# $1 = max_prepared_statements, $2 = default_pool_size
start_bouncer() {
  docker rm -f "$BOUNCER" >/dev/null 2>&1 || true
  docker run -d --rm --name "$BOUNCER" --network "$NET" \
    -v "$CACHE/apt:/var/cache/apt/archives" \
    -e MPS="$1" -e POOL="$2" -e PGHOST="$PG" -e VER="$PGBOUNCER_APT_VERSION" \
    "$PG_IMAGE" bash -c '
      set -e
      rm -f /etc/apt/apt.conf.d/docker-clean
      apt-get update -qq >/dev/null && apt-get install -y -qq "pgbouncer=$VER" >/dev/null
      cat > /tmp/pgbouncer.ini <<EOF
[databases]
postgres = host=$PGHOST port=5432 dbname=postgres user=postgres
[pgbouncer]
listen_addr = 0.0.0.0
listen_port = 6432
auth_type = trust
auth_file = /tmp/userlist.txt
pool_mode = transaction
default_pool_size = $POOL
max_prepared_statements = $MPS
ignore_startup_parameters = extra_float_digits
EOF
      echo "\"postgres\" \"\"" > /tmp/userlist.txt
      chown postgres /tmp/pgbouncer.ini /tmp/userlist.txt
      exec su postgres -s /bin/sh -c "pgbouncer -V; pgbouncer /tmp/pgbouncer.ini"' >/dev/null
  for _ in $(seq 1 180); do
    docker exec "$BOUNCER" pg_isready -q -h 127.0.0.1 -p 6432 -U postgres 2>/dev/null && return 0
    sleep 1
  done
  echo "pgbouncer did not start" >&2; docker logs "$BOUNCER" >&2; return 1
}

# --- driver runners ---------------------------------------------------------

DSN_PG="postgres://postgres@$PG:5432/postgres?sslmode=disable"
DSN_BOUNCER="postgres://postgres@$BOUNCER:6432/postgres?sslmode=disable"

run_pgx() { # $1 = DSN, rest = args
  local dsn=$1; shift
  docker run --rm --network "$NET" -e DSN="$dsn" \
    -v "$HERE/pgx:/src:ro" -v "$CACHE/go:/go/pkg/mod" \
    "$GO_IMAGE" sh -c 'cp -r /src /w && cd /w && go run . "$@"' -- "$@"
}

run_jdbc() { # $1 = JDBC URL
  docker run --rm --network "$NET" -e URL="$1" -e VER="$PGJDBC_VERSION" \
    -v "$HERE/jdbc:/src:ro" -v "$CACHE/apt:/var/cache/apt/archives" -v "$CACHE/m2:/m2" \
    "$PG_IMAGE" bash -c '
      set -e
      rm -f /etc/apt/apt.conf.d/docker-clean
      apt-get update -qq >/dev/null && apt-get install -y -qq openjdk-21-jdk-headless curl >/dev/null 2>&1
      jar=/m2/postgresql-$VER.jar
      [ -s "$jar" ] || curl -fsSL -o "$jar" "https://repo1.maven.org/maven2/org/postgresql/postgresql/$VER/postgresql-$VER.jar"
      java -version 2>&1 | head -1
      java -cp "$jar" /src/Repro.java "$URL"'
}

run_psycopg() { # $1 = conninfo, rest = args
  local dsn=$1; shift
  docker run --rm --network "$NET" -e DSN="$dsn" -e VER="$PSYCOPG_VERSION" \
    -v "$HERE/psycopg:/src:ro" -v "$CACHE/apt:/var/cache/apt/archives" -v "$CACHE/pip:/root/.cache/pip" \
    "$PG_IMAGE" bash -c '
      set -e
      rm -f /etc/apt/apt.conf.d/docker-clean
      apt-get update -qq >/dev/null && apt-get install -y -qq python3-venv >/dev/null 2>&1
      python3 -m venv /v && /v/bin/pip install -q "psycopg[binary]==$VER"
      /v/bin/python /src/repro.py "$@"' -- "$@"
}

run_rails() { # $1 = script under rails/, $2 = ActiveRecord version, rest = VAR=value env
  local script=$1 ar=$2; shift 2
  local envs=(); for kv in "$@"; do envs+=(-e "$kv"); done
  docker run --rm --network "$NET" -e DATABASE_URL="$DSN_PG" -e AR_VERSION="$ar" "${envs[@]}" \
    -v "$HERE/rails:/src:ro" -v "$CACHE/gem:/usr/local/bundle" \
    "$RUBY_IMAGE" ruby "/src/$script"
}

# --- scenarios (names must match expectations.tsv) --------------------------

scenario() {
  case "$1" in
    pgx_default)        start_pg; run_pgx "$DSN_PG" -mode default ;;
    pgx_cache_describe) start_pg; run_pgx "$DSN_PG" -mode cache_describe ;;
    pgx_describe_exec)  start_pg; run_pgx "$DSN_PG" -mode describe_exec ;;
    pgx_exec)           start_pg; run_pgx "$DSN_PG" -mode exec ;;
    pgx_simple)         start_pg; run_pgx "$DSN_PG" -mode simple ;;
    pgx_app_prepared)   start_pg; run_pgx "$DSN_PG" -mode app_prepared ;;
    jdbc_default)       start_pg; run_jdbc "jdbc:postgresql://$PG:5432/postgres?user=postgres" ;;
    jdbc_t1)            start_pg; run_jdbc "jdbc:postgresql://$PG:5432/postgres?user=postgres&prepareThreshold=1" ;;
    jdbc_t1_reused_ps)  start_pg; run_jdbc "jdbc:postgresql://$PG:5432/postgres?user=postgres&prepareThreshold=1&reusePs=true" ;;
    psycopg_default)    start_pg; run_psycopg "host=$PG user=postgres dbname=postgres" default ;;
    psycopg_t0)         start_pg; run_psycopg "host=$PG user=postgres dbname=postgres" 0 ;;
    psycopg_none)       start_pg; run_psycopg "host=$PG user=postgres dbname=postgres" none ;;
    rails_query_log_tags) start_pg; run_rails query_log_tags.rb 8.1.4 QLT_MODE=native ;;
    rails_query_log_tags_ar70) start_pg; run_rails query_log_tags.rb 7.0.8.7 QLT_MODE=native ;;
    rails_query_log_tags_prepared_override) start_pg; run_rails query_log_tags.rb 8.1.4 QLT_MODE=prepared_override ;;
    rails_marginalia)   start_pg; run_rails marginalia.rb 8.1.4 ;;
    rails_marginalia_ar70) start_pg; run_rails marginalia.rb 7.0.8.7 ;;
    rails_marginalia_ar72) start_pg; run_rails marginalia.rb 7.2.3.2 ;;
    rails_marginalia_ar80) start_pg; run_rails marginalia.rb 8.0.5.1 ;;
    rails_marginalia_exec_query) start_pg; run_rails marginalia.rb 8.1.4 MARG_PATH=exec_query ;;
    pgbouncer_mps_pgx)  start_pg; start_bouncer 200 1; run_pgx "$DSN_BOUNCER" -mode default -clients 2 ;;
    pgbouncer_mps_psycopg) start_pg; start_bouncer 200 1; run_psycopg "host=$BOUNCER port=6432 user=postgres dbname=postgres" 0 2 ;;
    pgbouncer_mps_jdbc) start_pg; start_bouncer 200 1; run_jdbc "jdbc:postgresql://$BOUNCER:6432/postgres?user=postgres&prepareThreshold=1&clients=2" ;;
    pgbouncer_nomps_pgx) start_pg; start_bouncer 0 1; run_pgx "$DSN_BOUNCER" -mode default ;;
    *) echo "unknown scenario $1" >&2; return 1 ;;
  esac
}

selected() {
  [ ${#FILTERS[@]} -eq 0 ] && return 0
  for p in "${FILTERS[@]}"; do [[ $1 == "$p"* ]] && return 0; done
  return 1
}

FILTERS=("$@")
failures=() ; ran=0
while IFS=$'\t' read -r name expect invariants _; do
  [[ -z $name || $name == \#* ]] && continue
  selected "$name" || continue
  if [ "${FLIP:-0}" = 1 ]; then
    [ "$expect" = stale ] && expect=fresh || expect=stale
  fi
  ran=$((ran + 1))
  log="$CACHE/logs/$name.json"
  echo "### $name (expect $expect)"
  if ! scenario "$name" > "$CACHE/logs/$name.client.txt" 2>&1; then
    echo "   CLIENT FAILED:"; tail -20 "$CACHE/logs/$name.client.txt" | sed 's/^/   | /'
    failures+=("$name(client)"); continue
  fi
  grep -E 'version|^psycopg |^activerecord |pgx mode|done:|Error|abort' "$CACHE/logs/$name.client.txt" | sed 's/^/   client: /' || true
  pg_log > "$log"
  if [ -n "$(docker ps -q -f name="^$BOUNCER$")" ]; then
    docker logs "$BOUNCER" 2>&1 | grep -m1 -i '^PgBouncer' | sed 's/^/   bouncer: /' || true
    docker rm -f "$BOUNCER" >/dev/null
  fi
  if ! docker run --rm -i -v "$HERE/lib:/lib-src:ro" "$CHECK_IMAGE" \
       python3 /lib-src/check_log.py "$name" "$expect" "$invariants" < "$log"; then
    failures+=("$name")
  fi
  # Mutation self-test: the real log must pass and every applicable mutation
  # (strip Parses, drop named executions, ...) must fail. Uses the unflipped
  # expectation, so it is skipped under FLIP=1.
  if [ "${FLIP:-0}" != 1 ] && ! docker run --rm -i -v "$HERE/lib:/lib-src:ro" "$CHECK_IMAGE" \
       python3 /lib-src/check_log.py --selftest "$name" "$expect" "$invariants" < "$log"; then
    failures+=("$name(selftest)")
  fi
done < "$HERE/expectations.tsv"

echo
echo "scenarios run: $ran, failed: ${#failures[@]} ${failures[*]:-}"
[ "$ran" -gt 0 ] && [ ${#failures[@]} -eq 0 ]
