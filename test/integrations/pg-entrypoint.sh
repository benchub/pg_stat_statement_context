#!/bin/bash
# Entrypoint of the PostgreSQL container of scripts/test-integrations.sh.
# Runs in the docker/Dockerfile image (official postgres image + PGXS) with the
# repository mounted read-only at /src: builds and installs the extension,
# installs the init scripts, then starts the server through the image's own
# docker-entrypoint.sh with the library preloaded and short buckets.
set -euo pipefail

rm -rf /build
mkdir -p /build
tar -C /src --exclude=./.git --exclude=./tmp --exclude=./fuzz \
	--exclude='*.o' --exclude='*.so' --exclude='*.dylib' --exclude='*.bc' \
	--exclude='*.dSYM' --exclude=./results --exclude=./tmp_check \
	--exclude=./test/unit -cf - . | tar -C /build -xf -
make -C /build clean >/dev/null
make -C /build -j2 >/dev/null
make -C /build install >/dev/null
# TEST-ONLY: its debug clock simulates a backward clock step.
make -C /build/test/modules/pssc_store_test install >/dev/null

cp /src/test/integrations/init.sql /docker-entrypoint-initdb.d/10-init.sql
# The recipe's own role script, as published.
cp /src/docs/integrations/monitoring-role.sql /docker-entrypoint-initdb.d/20-monitoring-role.sql
cat >/docker-entrypoint-initdb.d/30-passwords.sql <<'EOF'
ALTER ROLE pssc_monitor PASSWORD 'monitor';
EOF

exec docker-entrypoint.sh postgres \
	-c shared_preload_libraries=pg_stat_statements,pg_stat_statement_context \
	-c pg_stat_statements.track=all \
	-c pg_stat_statement_context.track=all \
	-c pg_stat_statement_context.bucket_interval=10s \
	-c pg_stat_statement_context.bucket_count=6 \
	"$@"
