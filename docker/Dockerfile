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
# (test/t/036_hook_coexistence.pl), where PGDG packages them for this major
# (pg_stat_monitor is not packaged for Debian by PGDG). The modules installed
# are listed in /usr/local/share/pssc-test-modules, which docker/run-tests.sh
# turns into PSSC_REQUIRE_MODULES so that the test fails instead of skipping
# them. A package that exists but fails to install fails the build: only a
# package PGDG does not publish is skipped.
RUN set -e; apt-get update; \
    mods="auto_explain"; \
    for p in pgaudit:pgaudit pg-hint-plan:pg_hint_plan pg-stat-monitor:pg_stat_monitor; do \
      pkg="postgresql-${PG_MAJOR}-${p%%:*}"; \
      if [ -n "$(apt-cache policy "$pkg" | sed -n 's/^ *Candidate: //p' | grep -v '(none)')" ]; then \
        apt-get install -y --no-install-recommends "$pkg" \
          || { echo "installing $pkg failed" >&2; exit 1; }; \
        [ -e "/usr/lib/postgresql/${PG_MAJOR}/lib/${p#*:}.so" ] \
          || { echo "$pkg installed no ${p#*:}.so" >&2; exit 1; }; \
        mods="$mods ${p#*:}"; \
      else \
        echo "no PGDG package $pkg: ${p#*:} is not tested"; \
      fi; \
    done; \
    echo "$mods" > /usr/local/share/pssc-test-modules; \
    rm -rf /var/lib/apt/lists/*
COPY run-tests.sh /usr/local/bin/run-tests.sh
ENTRYPOINT ["/usr/local/bin/run-tests.sh"]
