# Standalone unit tests for src/scan.c and src/pairs.c: no server and no
# pg_config needed.
#   test_scan.c   the comment lexer (pssc_scan_comments, pssc_scan_only_trivia)
#   test_stmt.c   statement ranges, positional/windowed scans, footers
#   test_pairs.c  SQLCommenter / marginalia pair parsers, pssc_comment_body
#   make -C test/unit          build and run (top-level: make unittest)
#   make -C test/unit corpus   write the case inputs to corpus/ (fuzz seeds)
# Each runs as two builds under AddressSanitizer + UBSan: test_X uses the
# production source; test_X_checked compiles it with -DPSSC_SCAN_CHECKED /
# -DPSSC_PAIRS_CHECKED, which asserts every byte read is inside the input
# and counts reads so the driver can check the work is linear.
# check-alloc compiles pairs.c without sanitizers and fails if the object
# references any function outside a small allowlist (no malloc, palloc, ...):
# the parsers allocate nothing beyond the caller's buffer.
# Set SANITIZE= to build without sanitizers.

CC ?= cc
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS_UNIT = -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
	-Wmissing-prototypes -Wdeclaration-after-statement -Wvla \
	-Wno-missing-field-initializers -I../../src $(SANITIZE)
# LeakSanitizer needs ptrace, which containers often forbid; scan.c never
# allocates anyway (nor does pairs.c).
export ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0
export UBSAN_OPTIONS ?= print_stacktrace=1

DEPS = ../../src/scan.c ../../src/scan.h
PAIRS_DEPS = ../../src/pairs.c ../../src/pairs.h
PROGS = test_scan test_scan_checked test_stmt test_stmt_checked \
	test_pairs test_pairs_checked

# Functions pairs.c may reference (compilers may emit mem* calls themselves).
ALLOWED_SYMS = memcpy memmove memset memcmp memchr __stack_chk_fail __stack_chk_guard \
	__memcpy_chk __memmove_chk __memset_chk

.PHONY: all check check-alloc corpus clean
all: check

test_pairs: test_pairs.c $(PAIRS_DEPS)
	$(CC) $(CFLAGS_UNIT) -o $@ $< ../../src/pairs.c

test_pairs_checked: test_pairs.c $(PAIRS_DEPS)
	$(CC) $(CFLAGS_UNIT) -DPSSC_PAIRS_CHECKED -o $@ $< ../../src/pairs.c

test_%: test_%.c $(DEPS)
	$(CC) $(CFLAGS_UNIT) -o $@ $< ../../src/scan.c

test_%_checked: test_%.c $(DEPS)
	$(CC) $(CFLAGS_UNIT) -DPSSC_SCAN_CHECKED -o $@ $< ../../src/scan.c

check: $(PROGS) check-alloc
	./test_scan
	./test_scan_checked
	./test_stmt
	./test_stmt_checked
	./test_pairs
	./test_pairs_checked

check-alloc: $(PAIRS_DEPS)
	$(CC) -std=c99 -O2 -Wall -Werror -c -o pairs_alloc_check.o ../../src/pairs.c
	@syms=$$(nm -u pairs_alloc_check.o | awk '{print $$NF}' | sed 's/^_\(_*[a-z]\)/\1/'); \
	rm -f pairs_alloc_check.o; \
	bad=; for s in $$syms; do \
		case " $(ALLOWED_SYMS) " in *" $$s "*) ;; *) bad="$$bad $$s";; esac; \
	done; \
	if [ -n "$$bad" ]; then echo "pairs.c references disallowed symbols:$$bad"; exit 1; fi; \
	echo "ok: pairs.c references only:" $$syms

corpus: test_scan test_stmt test_pairs
	mkdir -p corpus
	./test_scan --emit-corpus corpus
	./test_stmt --emit-corpus corpus
	./test_pairs --emit-corpus corpus

clean:
	rm -rf $(PROGS) *.dSYM corpus pairs_alloc_check.o
