# Fuzz targets for the pure-C parsers (DESIGN.md §9). No server or pg_config.
#   fuzz_sqlcommenter.c   pssc_parse_sqlcommenter (+ pssc_comment_body)
#   fuzz_marginalia.c     pssc_parse_marginalia
# Both define LLVMFuzzerTestOneInput.
#
#   make -C fuzz check   build with any C compiler + ASan/UBSan and the
#                        standalone driver (standalone_main.c); run the
#                        seed corpus from test/unit plus random inputs.
#                        Part of "make unittest".
#   make -C fuzz fuzz    build libFuzzer binaries (needs clang with
#                        -fsanitize=fuzzer), e.g.
#                          fuzz/fuzz_sqlcommenter corpus/ -max_total_time=60
#
# A long-running fuzzing setup (CI job, scanner target) is backlog item
# 20261005-091225-25.

CC ?= cc
CLANG ?= clang
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS_FUZZ = -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
	-Wmissing-prototypes -Wdeclaration-after-statement -Wvla \
	-Wno-missing-field-initializers -Wno-unused-function -I../src
export ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0
export UBSAN_OPTIONS ?= print_stacktrace=1

TARGETS = fuzz_sqlcommenter fuzz_marginalia
DEPS = ../src/pairs.c ../src/pairs.h fuzz_common.h
RANDOM_RUNS ?= 20000

.PHONY: all check fuzz corpus clean
all: check

%_standalone: %.c standalone_main.c $(DEPS)
	$(CC) $(CFLAGS_FUZZ) $(SANITIZE) -o $@ $< standalone_main.c ../src/pairs.c

corpus:
	$(MAKE) -C ../test/unit test_pairs
	rm -rf corpus && mkdir -p corpus
	../test/unit/test_pairs --emit-corpus corpus

check: $(TARGETS:%=%_standalone) corpus
	./fuzz_sqlcommenter_standalone --random $(RANDOM_RUNS) corpus
	./fuzz_marginalia_standalone --random $(RANDOM_RUNS) corpus

fuzz: $(TARGETS)

$(TARGETS): %: %.c $(DEPS)
	$(CLANG) $(CFLAGS_FUZZ) -fsanitize=fuzzer,address,undefined -o $@ $< ../src/pairs.c

clean:
	rm -rf $(TARGETS) $(TARGETS:%=%_standalone) *.dSYM corpus
