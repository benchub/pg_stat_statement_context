# Standalone unit tests for src/scan.c: no server and no pg_config needed.
#   test_scan.c   the comment lexer (pssc_scan_comments, pssc_scan_only_trivia)
#   test_stmt.c   statement ranges, positional/windowed scans, footers
#   make -C test/unit          build and run (top-level: make unittest)
#   make -C test/unit corpus   write the case inputs to corpus/ (fuzz seeds)
# Each runs as two builds under AddressSanitizer + UBSan: test_X uses the
# production scan.c; test_X_checked compiles scan.c with
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

DEPS = ../../src/scan.c ../../src/scan.h
PROGS = test_scan test_scan_checked test_stmt test_stmt_checked

.PHONY: all check corpus clean
all: check

test_%: test_%.c $(DEPS)
	$(CC) $(CFLAGS_UNIT) -o $@ $< ../../src/scan.c

test_%_checked: test_%.c $(DEPS)
	$(CC) $(CFLAGS_UNIT) -DPSSC_SCAN_CHECKED -o $@ $< ../../src/scan.c

check: $(PROGS)
	./test_scan
	./test_scan_checked
	./test_stmt
	./test_stmt_checked

corpus: test_scan test_stmt
	mkdir -p corpus
	./test_scan --emit-corpus corpus
	./test_stmt --emit-corpus corpus

clean:
	rm -rf $(PROGS) *.dSYM corpus
