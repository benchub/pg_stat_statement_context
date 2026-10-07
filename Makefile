# pg_stat_statement_context — PGXS build
#   make && make install && make installcheck
# installcheck needs a running server with
#   shared_preload_libraries = 'pg_stat_statement_context'
# (scripts/docker-test.sh <pg-major> sets one up). The TAP tests also need the
# TEST-ONLY modules from "make install-test-modules", which is never installed
# by "make install".

MODULE_big = pg_stat_statement_context
OBJS = \
	src/pg_stat_statement_context.o \
	src/guc.o \
	src/scan.o \
	src/pairs.o \
	src/tagset.o \
	src/extract.o \
	src/extract_fn.o \
	src/tagout.o \
	src/stats_fn.o \
	src/info_fn.o \
	src/regex_runtime.o \
	src/context.o \
	src/executor.o \
	src/utility.o \
	src/counters.o \
	src/store.o \
	src/reclaim.o \
	src/activity.o \
	src/activity_fn.o \
	src/cardcap.o
PGFILEDESC = "pg_stat_statement_context - per-tag statement statistics from SQL comments"

# The extension version, recorded in the stats file (src/store.c, DESIGN.md
# §5.5): a file written by another version is discarded.
PSSC_EXT_VERSION := $(shell sed -n "s/^default_version *= *'\([^']*\)'.*/\1/p" $(dir $(lastword $(MAKEFILE_LIST)))pg_stat_statement_context.control)
PG_CPPFLAGS += -DPSSC_EXT_VERSION='"$(PSSC_EXT_VERSION)"'

EXTENSION = pg_stat_statement_context
DATA = sql/pg_stat_statement_context--1.0.sql

REGRESS = smoke guc extract normalize appname tags_override
# extract needs a UTF8 database (multibyte cases); --no-locale makes the
# database creatable with any server locale.
REGRESS_OPTS = --inputdir=test --encoding=UTF8 --no-locale \
	--temp-config=$(srcdir)/test/pg_stat_statement_context.conf

TAP_TESTS = 1
PROVE_TESTS = test/t/*.pl

# Standalone unit tests (test/unit) and fuzz targets (fuzz/), built by
# "make unittest".
EXTRA_CLEAN = test/unit/test_scan test/unit/test_scan_checked \
	test/unit/test_stmt test/unit/test_stmt_checked \
	test/unit/test_pairs test/unit/test_pairs_checked \
	test/unit/test_tagset test/unit/test_tagset_checked \
	test/unit/test_counters \
	test/unit/pairs_alloc_check.o \
	test/unit/corpus test/unit/*.dSYM \
	fuzz/fuzz_scan fuzz/fuzz_sqlcommenter fuzz/fuzz_marginalia fuzz/fuzz_tagset \
	fuzz/fuzz_scan_standalone fuzz/fuzz_sqlcommenter_standalone \
	fuzz/fuzz_marginalia_standalone fuzz/fuzz_tagset_standalone \
	fuzz/corpus fuzz/*.dSYM

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Shared TAP helpers (test/perl/PsscTest.pm).
PG_PROVE_FLAGS += -I $(srcdir)/test/perl

# TEST-ONLY modules: pssc_compat_test exercises the src/compat.h shims
# (test/t/002_compat.pl); pssc_guc_test inspects the parsed GUC state of
# src/guc.h (test/t/003_guc.pl) and runs the load-order matcher of
# src/utility.h (test/t/014_load_order.pl); pssc_extract_test runs the
# tag-set pipeline of src/extract.h on given text and injects faults into the regex runtime
# (test/t/005_extract.pl, test/t/006_regex.pl); pssc_store_test drives the
# shared store of src/store.h (test/t/007_store.pl, 008_buckets.pl,
# 009_eviction.pl, and reads recorded entries in 011_executor.pl and
# 012_utility.pl and 014_load_order.pl, and the shared counters in
# 013_extract_fn.pl, which also
# injects a regex compile failure through pssc_extract_test), and seeds the
# entries read by the stats views in 015_stats.pl and by _info() in
# 016_info.pl (which also injects a regex compile failure through
# pssc_extract_test). Its debug clock, stalls and forced collisions drive
# real recording in 018_store_reconfig.pl and 019_sql_surface.pl.
# pssc_context_test exposes the execution frames of src/context.h (frame
# registry, active frame, frames seen at ExecutorEnd) to SQL
# (test/t/010_context.pl, 012_utility.pl, which also reads recorded entries
# through pssc_store_test).
TEST_MODULES = test/modules/pssc_compat_test test/modules/pssc_guc_test \
	test/modules/pssc_extract_test test/modules/pssc_store_test \
	test/modules/pssc_context_test

.PHONY: test-modules install-test-modules clean-test-modules check-version-guards check-frozen-sql unittest

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

# Released extension scripts are frozen (sql/frozen.sha256).
check-frozen-sql:
	scripts/check-frozen-sql.sh --self-test
	scripts/check-frozen-sql.sh

# Standalone unit tests for src/scan.c (lexer, statement ranges, positional
# scans), src/pairs.c (SQLCommenter/marginalia parsers), src/tagset.c
# (tag-set pipeline and extractor chain) and src/counters.c (per-bucket
# counter slot) under ASan/UBSan,
# then the parser fuzz targets through their standalone driver; no server
# needed. Also runnable without pg_config: make -C test/unit; make -C fuzz
# (libFuzzer builds: make -C fuzz fuzz, needs clang with -fsanitize=fuzzer).
unittest:
	$(MAKE) -C test/unit
	$(MAKE) -C fuzz check
