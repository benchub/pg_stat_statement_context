# Build/test image for pg_stat_statement_context against a PGDG PostgreSQL.
# Usage: scripts/docker-test.sh <pg-major>
ARG PG_MAJOR=17
FROM postgres:${PG_MAJOR}
ARG PG_MAJOR
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      "postgresql-server-dev-${PG_MAJOR}" build-essential libipc-run-perl \
 && rm -rf /var/lib/apt/lists/*
COPY run-tests.sh /usr/local/bin/run-tests.sh
ENTRYPOINT ["/usr/local/bin/run-tests.sh"]
