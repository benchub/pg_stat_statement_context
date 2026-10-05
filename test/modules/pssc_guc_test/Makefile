# TEST-ONLY module: inspects the parsed configuration of
# pg_stat_statement_context (src/guc.h) from SQL (see test/t/003_guc.pl).
# It is never built or installed by the top-level "make install"; use
# "make install-test-modules" (scripts/docker-test.sh does this). It needs
# pg_stat_statement_context in shared_preload_libraries.

MODULES = pssc_guc_test
EXTENSION = pssc_guc_test
DATA = pssc_guc_test--1.0.sql
PGFILEDESC = "pssc_guc_test - test-only inspector for pg_stat_statement_context GUCs"

PG_CPPFLAGS = -I$(srcdir)/../../../src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
