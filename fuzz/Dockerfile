# Linux clang toolchain with libFuzzer for the fuzz/ targets (Apple clang has
# no -fsanitize=fuzzer). Built by fuzz/run-libfuzzer.sh as
# pssc-fuzz:clang-<hash> (hash of this file) when that tag is missing.
# BASE may be any Debian/Ubuntu image (run-libfuzzer.sh passes
# $PSSC_FUZZ_BASE, e.g. an already pulled postgres:18).
ARG BASE=debian:trixie-slim
FROM ${BASE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang libclang-rt-dev make libc6-dev \
 && rm -rf /var/lib/apt/lists/* \
 && printf 'int LLVMFuzzerTestOneInput(const char *d, unsigned long n) { return 0; }\n' > /tmp/t.c \
 && clang -fsanitize=fuzzer,address,undefined -o /tmp/t /tmp/t.c \
 && rm -f /tmp/t /tmp/t.c
