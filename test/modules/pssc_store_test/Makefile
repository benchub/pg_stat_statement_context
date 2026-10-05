# TEST-ONLY module: drives pg_stat_statement_context's shared store
# (src/store.h) from SQL (see test/t/007_store.pl). It is never built or
# installed by the top-level "make install"; use "make install-test-modules"
# (scripts/docker-test.sh does this). It needs pg_stat_statement_context in
# shared_preload_libraries.

MODULES = pssc_store_test
EXTENSION = pssc_store_test
DATA = pssc_store_test--1.0.sql
PGFILEDESC = "pssc_store_test - test-only driver for the pg_stat_statement_context shared store"

PG_CPPFLAGS = -I$(srcdir)/../../../src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
