# TEST-ONLY module: runs pg_stat_statement_context's tag-set pipeline
# (src/extract.h) on given text from SQL (see test/t/005_extract.pl).
# It is never built or installed by the top-level "make install"; use
# "make install-test-modules" (scripts/docker-test.sh does this). It needs
# pg_stat_statement_context in shared_preload_libraries.

# scan_copy.c compiles the backend-independent src/scan.c into the module
# (for pssc_stmt_range/pssc_stmt_owned_start, which the main library does
# not export).
MODULE_big = pssc_extract_test
OBJS = pssc_extract_test.o scan_copy.o
EXTENSION = pssc_extract_test
DATA = pssc_extract_test--1.0.sql
PGFILEDESC = "pssc_extract_test - test-only driver for the pg_stat_statement_context tag-set pipeline"

# Built against the testing variant of the library (make PSSC_TESTING=1).
PG_CPPFLAGS = -I$(srcdir)/../../../src -DPSSC_TESTING

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
