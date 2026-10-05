# Standalone unit tests for src/scan.c: no server and no pg_config needed.
#   make -C test/unit          build and run (top-level: make unittest)
#   make -C test/unit corpus   write the case inputs to corpus/ (fuzz seeds)
# Two builds run under AddressSanitizer + UBSan: test_scan uses the
# production scan.c; test_scan_checked compiles scan.c with
# -DPSSC_SCAN_CHECKED, which asserts every byte read is inside [start, end)
# and counts reads so the driver can check the scan is linear.
# Set SANITIZE= to build without sanitizers.

CC ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS_UNIT = -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
	-Wmissing-prototypes -Wdeclaration-after-statement \
	-Wno-missing-field-initializers -I../../src $(SANITIZE)
# LeakSanitizer needs ptrace, which containers often forbid; scan.c never
# allocates anyway.
export ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0
export UBSAN_OPTIONS ?= print_stacktrace=1

SRCS = test_scan.c ../../src/scan.c ../../src/scan.h

.PHONY: all check corpus clean
all: check

test_scan: $(SRCS)
	$(CC) $(CFLAGS_UNIT) -o $@ test_scan.c ../../src/scan.c

test_scan_checked: $(SRCS)
	$(CC) $(CFLAGS_UNIT) -DPSSC_SCAN_CHECKED -o $@ test_scan.c ../../src/scan.c

check: test_scan test_scan_checked
	./test_scan
	./test_scan_checked

corpus: test_scan
	mkdir -p corpus
	./test_scan --emit-corpus corpus

clean:
	rm -rf test_scan test_scan_checked *.dSYM corpus
