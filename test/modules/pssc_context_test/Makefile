# TEST-ONLY module: exposes pg_stat_statement_context's execution frames
# (src/context.h; made by the main library's executor and utility hooks,
# src/executor.c, src/utility.c)
# to SQL: the frame registry, the active frame and, through an observing
# ExecutorEnd hook, the frames found at ExecutorEnd (see test/t/010_context.pl
# and test/t/012_utility.pl).
# It is never built or installed by the top-level "make install"; use
# "make install-test-modules" (scripts/docker-test.sh does this). Its hooks
# are installed only when it is listed in shared_preload_libraries after
# pg_stat_statement_context.

MODULES = pssc_context_test
EXTENSION = pssc_context_test
DATA = pssc_context_test--1.0.sql
PGFILEDESC = "pssc_context_test - test-only driver for pg_stat_statement_context execution frames"

PG_CPPFLAGS = -I$(srcdir)/../../../src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
