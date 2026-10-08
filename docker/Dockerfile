# Build/test image for pg_stat_statement_context against a PGDG PostgreSQL.
# Usage: scripts/docker-test.sh <pg-major>
ARG PG_MAJOR=17
FROM postgres:${PG_MAJOR}
ARG PG_MAJOR
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      "postgresql-server-dev-${PG_MAJOR}" build-essential libipc-run-perl \
 && rm -rf /var/lib/apt/lists/*
# Third-party extensions for the hook coexistence test
# (test/t/035_hook_coexistence.pl), where PGDG packages them for this major
# (pg_stat_monitor is not packaged for Debian by PGDG). The modules installed
# are listed in /usr/local/share/pssc-test-modules, which docker/run-tests.sh
# turns into PSSC_REQUIRE_MODULES so that the test fails instead of skipping
# them.
RUN apt-get update \
 && mods="auto_explain" \
 && for p in pgaudit:pgaudit pg-hint-plan:pg_hint_plan pg-stat-monitor:pg_stat_monitor; do \
      pkg="postgresql-${PG_MAJOR}-${p%%:*}"; \
      if apt-cache show "$pkg" >/dev/null 2>&1; then \
        apt-get install -y --no-install-recommends "$pkg" && mods="$mods ${p#*:}"; \
      else \
        echo "no PGDG package $pkg: ${p#*:} is not tested"; \
      fi; \
    done \
 && echo "$mods" > /usr/local/share/pssc-test-modules \
 && rm -rf /var/lib/apt/lists/*
COPY run-tests.sh /usr/local/bin/run-tests.sh
ENTRYPOINT ["/usr/local/bin/run-tests.sh"]
