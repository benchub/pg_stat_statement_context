# pg_stat_statement_context — PGXS build
#   make && make install && make installcheck
# installcheck needs a running server with
#   shared_preload_libraries = 'pg_stat_statement_context'
# (scripts/docker-test.sh <pg-major> sets one up). The TAP tests also need the
# TEST-ONLY module from "make install-test-modules", which is never installed
# by "make install".

MODULE_big = pg_stat_statement_context
OBJS = \
	src/pg_stat_statement_context.o \
	src/guc.o \
	src/scan.o \
	src/extract.o \
	src/context.o \
	src/store.o
PGFILEDESC = "pg_stat_statement_context - per-tag statement statistics from SQL comments"

EXTENSION = pg_stat_statement_context
DATA = sql/pg_stat_statement_context--1.0.sql

REGRESS = smoke
REGRESS_OPTS = --inputdir=test \
	--temp-config=$(srcdir)/test/pg_stat_statement_context.conf

TAP_TESTS = 1
PROVE_TESTS = test/t/*.pl

# Standalone scanner unit tests (test/unit, built by "make unittest").
EXTRA_CLEAN = test/unit/test_scan test/unit/test_scan_checked \
	test/unit/test_stmt test/unit/test_stmt_checked \
	test/unit/corpus test/unit/*.dSYM

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# TEST-ONLY module that exercises the src/compat.h shims (test/t/002_compat.pl).
TEST_MODULES = test/modules/pssc_compat_test

.PHONY: test-modules install-test-modules clean-test-modules check-version-guards unittest

test-modules:
	for d in $(TEST_MODULES); do $(MAKE) -C $$d PG_CONFIG=$(PG_CONFIG) || exit 1; done

install-test-modules: test-modules
	for d in $(TEST_MODULES); do $(MAKE) -C $$d PG_CONFIG=$(PG_CONFIG) install || exit 1; done

clean-test-modules:
	for d in $(TEST_MODULES); do $(MAKE) -C $$d PG_CONFIG=$(PG_CONFIG) clean || exit 1; done

# PG_VERSION_NUM may only appear outside src/compat.h with a justifying comment.
check-version-guards:
	scripts/check-version-guards.sh --self-test
	scripts/check-version-guards.sh

# Standalone unit tests for src/scan.c (lexer, statement ranges, positional
# scans) under ASan/UBSan; no server needed.
# Also runnable without pg_config: make -C test/unit
unittest:
	$(MAKE) -C test/unit
