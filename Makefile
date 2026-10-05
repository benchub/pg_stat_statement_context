# pg_stat_statement_context — PGXS build
#   make && make install && make installcheck
# installcheck needs a running server with
#   shared_preload_libraries = 'pg_stat_statement_context'
# (scripts/docker-test.sh <pg-major> sets one up).

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

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
