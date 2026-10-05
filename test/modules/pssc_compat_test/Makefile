# TEST-ONLY module: exercises every shim in src/compat.h on the running
# PostgreSQL version (see test/t/002_compat.pl). It is never built or
# installed by the top-level "make install"; use "make install-test-modules"
# (scripts/docker-test.sh does this).

MODULES = pssc_compat_test
EXTENSION = pssc_compat_test
DATA = pssc_compat_test--1.0.sql
PGFILEDESC = "pssc_compat_test - test-only exerciser for src/compat.h"

PG_CPPFLAGS = -I$(srcdir)/../../../src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
