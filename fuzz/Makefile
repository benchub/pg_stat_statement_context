# Fuzz targets for the backend-independent C code (DESIGN.md §9). No server
# or pg_config. Each file defines LLVMFuzzerTestOneInput:
#   fuzz_scan.c           the comment scanner, src/scan.c: every entry point
#                         and position (any/append/prepend, exact, windowed
#                         and heuristic tail paths, footers, owned starts)
#   fuzz_sqlcommenter.c   pssc_parse_sqlcommenter (+ pssc_comment_body)
#   fuzz_marginalia.c     pssc_parse_marginalia
#   fuzz_tagset.c         the tag-set pipeline, src/tagset.c (scanner +
#                         parsers + chain + limits + serialization)
# scan.c and pairs.c are built with -DPSSC_SCAN_CHECKED/-DPSSC_PAIRS_CHECKED
# so any read outside the given range aborts and reads are counted (linear
# bounds). The SQL-level regex driver is fuzz/sql/ (see fuzz/README.md).
#
#   make -C fuzz check   build with any C compiler + ASan/UBSan and the
#                        standalone driver (standalone_main.c); run the seed
#                        corpus plus random inputs. Part of "make unittest".
#   make -C fuzz fuzz    build libFuzzer binaries (needs clang with
#                        -fsanitize=fuzzer, i.e. not Apple clang; on macOS
#                        use fuzz/run-libfuzzer.sh, which runs in Docker)
#   make -C fuzz corpus  (re)generate the seed corpus in corpus/: the
#                        test/unit case inputs and the statements of
#                        test/sql/*.sql

CC ?= cc
CLANG ?= clang
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS_FUZZ = -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
	-Wmissing-prototypes -Wdeclaration-after-statement -Wvla \
	-Wno-missing-field-initializers -Wno-unused-function -I../src \
	-DPSSC_SCAN_CHECKED -DPSSC_PAIRS_CHECKED
export ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0
export UBSAN_OPTIONS ?= print_stacktrace=1

TARGETS = fuzz_scan fuzz_sqlcommenter fuzz_marginalia fuzz_tagset
RANDOM_RUNS ?= 20000

PAIRS = ../src/pairs.c ../src/pairs.h
SCAN = ../src/scan.c ../src/scan.h
COMMON = fuzz_check.h Makefile
fuzz_scan_SRCS = ../src/scan.c
fuzz_scan_DEPS = $(SCAN)
fuzz_sqlcommenter_SRCS = ../src/pairs.c
fuzz_sqlcommenter_DEPS = $(PAIRS) fuzz_common.h
fuzz_marginalia_SRCS = ../src/pairs.c
fuzz_marginalia_DEPS = $(PAIRS) fuzz_common.h
fuzz_tagset_SRCS = ../src/tagset.c ../src/scan.c ../src/pairs.c
fuzz_tagset_DEPS = $(SCAN) $(PAIRS) ../src/tagset.c ../src/tagset.h \
	../src/guc.h ../test/unit/pssc_standalone.h
fuzz_tagset_CFLAGS = -DPSSC_STANDALONE -I../test/unit

.PHONY: all check fuzz corpus clean
all: check

.SECONDEXPANSION:
%_standalone: %.c standalone_main.c $$($$*_DEPS) $(COMMON)
	$(CC) $(CFLAGS_FUZZ) $($*_CFLAGS) $(SANITIZE) -o $@ $< standalone_main.c $($*_SRCS)

$(TARGETS): %: %.c $$($$*_DEPS) $(COMMON)
	$(CLANG) $(CFLAGS_FUZZ) $($*_CFLAGS) -fsanitize=fuzzer,address,undefined \
		-fno-sanitize-recover=all -fno-omit-frame-pointer -o $@ $< $($*_SRCS)

corpus:
	$(MAKE) -C ../test/unit test_scan test_stmt test_pairs test_tagset
	rm -rf corpus && mkdir -p corpus
	../test/unit/test_scan --emit-corpus corpus >/dev/null
	../test/unit/test_stmt --emit-corpus corpus >/dev/null
	../test/unit/test_pairs --emit-corpus corpus >/dev/null
	../test/unit/test_tagset --emit-corpus corpus
	awk -v out=corpus -f split-sql.awk ../test/sql/*.sql
	@echo "corpus: $$(ls corpus | wc -l | tr -d ' ') seeds"

check: $(TARGETS:%=%_standalone) corpus
	for t in $(TARGETS); do ./$${t}_standalone --random $(RANDOM_RUNS) corpus || exit 1; done

fuzz: $(TARGETS)

clean:
	rm -rf $(TARGETS) $(TARGETS:%=%_standalone) *.dSYM corpus
