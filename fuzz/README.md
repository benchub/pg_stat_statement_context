# Fuzzing

Two kinds of harness (DESIGN.md §9):

* **libFuzzer targets** for the backend-independent C code (no server, no `pg_config`), each also built with a standalone driver that runs on any compiler as part of `make unittest`.
* **An SQL-level driver** for the regex extractor, which needs the backend (its regex engine, GUC check hooks and memory contexts). It runs against a server in Docker.

| Command | What it does |
|---|---|
| `make -C fuzz check` | Standalone driver, any C compiler with ASan/UBSan (macOS host included): seed corpus + 20000 random inputs per target. Part of `make unittest`. |
| `fuzz/run-libfuzzer.sh -t 600` | libFuzzer, `clang -fsanitize=fuzzer,address,undefined`, in a Linux container: every target, in parallel, 600 s each. |
| `fuzz/sql/run.sh -- --duration 600` | SQL-level regex fuzzer, 600 s against the PG 18 `--enable-cassert` server. |
| `make -C fuzz fuzz` | Build the libFuzzer binaries directly, where clang has libFuzzer (Linux; not Apple clang). |

## libFuzzer targets

| Target | Code | Main invariants (beyond "no crash, no sanitizer report") |
|---|---|---|
| `fuzz_scan` | `src/scan.c`: lexer, statement ranges, owned starts, footers, positional scans in every mode (`any`, `append`, `prepend`), exact, windowed and heuristic-tail paths, both `standard_conforming_strings` settings | spans in range, ordered, disjoint, at most 16, each a whole comment that re-lexes to itself; budgets give a prefix and set `truncated`; translation invariance; `append`/`prepend` runs are maximal and match the exact scan; linear read count |
| `fuzz_sqlcommenter` | `pssc_parse_sqlcommenter`, `pssc_comment_body` | pairs inside the body; "always enough" storage drops nothing; small storage keeps a subsequence and counts the rest; deterministic; linear read count |
| `fuzz_marginalia` | `pssc_parse_marginalia` | same as above |
| `fuzz_tagset` | `src/tagset.c`: scanner + parsers + extractor chain + limits + serialization, across a fixed set of configurations, UTF-8 and SQL_ASCII | at most `max_tags` tags and `max_tagset_bytes` bytes; keys sorted, unique, 1..63 bytes; values ≤ `max_tag_value_len`, no NUL, valid in the encoding; allow/deny lists honored; deterministic; OOM at any allocation gives an empty set with `oom` set |

Sanitizer findings are fatal: the libFuzzer build uses `-fno-sanitize-recover=all` (as the standalone build does), and `run-libfuzzer.sh` sets `UBSAN_OPTIONS=halt_on_error=1`. So undefined behavior fails the run with a non-zero exit and leaves a reproducer in `artifacts/`, instead of printing a diagnostic and carrying on.

`scan.c` and `pairs.c` are compiled with `-DPSSC_SCAN_CHECKED` and `-DPSSC_PAIRS_CHECKED`. In those builds, any read outside the range the caller passed aborts, not only reads outside the allocation (which ASan already catches). Reads are also counted, so a quadratic regression fails too.

The seed corpus (`fuzz/corpus/`, generated, not committed) is built by `make -C fuzz corpus`. It is made of the case inputs of the `test/unit` suites (`--emit-corpus`) and the statements of `test/sql/*.sql` (`split-sql.awk`). `fuzz.dict` is the libFuzzer dictionary.

`run-libfuzzer.sh [-t SECONDS] [-r ARTIFACT] [TARGET...]`:

* The image is `pssc-fuzz:clang-<hash of fuzz/Dockerfile>`, Debian clang 19 with `libclang-rt-dev`. It is built on first use and kept, so later runs start immediately; remove it with `docker rmi`.
* `PSSC_FUZZ_BASE=postgres:18` builds that image from an image you have already pulled.
* The script copies the sources into the container, so host build output is never reused.
* Output goes to `tmp/fuzz/`:
  * `<target>.log`: the logs;
  * `corpus-<target>/`: the grown corpus, reused by the next run;
  * `artifacts/`: crash, leak and timeout inputs.
* Replay a crash with `fuzz/run-libfuzzer.sh -r tmp/fuzz/artifacts/<file> <target>`.
* On the host, the standalone driver also replays a file: `make -C fuzz fuzz_scan_standalone && fuzz/fuzz_scan_standalone FILE`.

## SQL-level regex fuzzer (`fuzz/sql/`)

`run.sh [--assert|--valgrind|--pgdg] [--pg MAJOR] [-- OPTIONS]` runs the driver in a container. It builds and installs the extension there with `-Werror`, then starts a private server with it in `shared_preload_libraries`. Images come from `scripts/docker-test.sh`:

* `--assert` (the default) and `--valgrind` use the source-build images, so run `scripts/docker-test.sh --assert 18` (or `--valgrind 18`) once first.
* `--pgdg` uses the package image and builds it when it's missing.

Under `--valgrind` the run fails if any backend reports a Valgrind error.

`regex_fuzz.pl` options:

| Option | Meaning |
|---|---|
| `--calls N` | Extract calls per round (default 40). |
| `--duration S` | Run time in seconds (default 60). |
| `--rounds N` | Run exactly N rounds instead of a duration, to replay a seed. |
| `--rounds-per-epoch N` | Rounds between server restarts (default 25). |
| `--seed N` | Seed for the run. Always printed at the start; random if omitted. |

How a run is organised:

* **Epochs.** Each epoch restarts the server with random `max_tags`, `max_tag_value_len` and `max_tagset_bytes`.
* **Rounds.** Each round:
  * picks a UTF-8 or SQL_ASCII database;
  * generates a regex pattern: structured patterns with known capture groups, plus broken and over-long ones;
  * sets `extractors` (with keys, position and merge), `tags`, `exclude_tags` and `scan_window` with `ALTER SYSTEM` and a reload, and pins `normalize` to '' (no rules), so every result must report `normalized_tags` = 0 and `normalize_failures` = 0. It sets no `cardinality_cap*` parameters (no cap), so `capped_tags` must be 0;
  * calls `pg_stat_statement_context_extract()` on generated statements: comments that the pattern matches, nested and unterminated comments, strings, multibyte text and invalid arguments.
* **Checks.**
  * The check_hook verdict is predicted (too long, invalid, back-reference, more groups than `max_tags`, key count ≠ group count) and compared with the server's DETAIL. Core `regexp_matches()` is the authority on syntax. Only the `ALTER SYSTEM`'s own error is classified (its SQLSTATE, message and DETAIL), never an earlier error from the same round. A timeout from the check hook is accepted only when the pre-check timed out too. `--self-test` covers this verdict as well.
  * Every result is checked against the limits, the key list, the encoding and the scanner's positional contract.
  * Repeated calls must give identical results.
  * For single-comment statements, the result must equal an oracle exactly:
    * `regexp_matches(..., 'g')` gives the first match per group, truncated to `max_tag_value_len` and filtered by `tags`/`exclude_tags`;
    * the greedy fill of DESIGN.md §6.11 step 9 is modelled (priority by allowlist position or sorted key; a tag that doesn't fit the bytes left is skipped; the fill stops at `max_tags`);
    * the kept tags, `dropped_tags` and `tagset_bytes` must all match the model, so a regression that discards tags and counts them as dropped is caught.

    `perl fuzz/sql/regex_fuzz.pl --self-test` checks the oracle itself on synthetic results, with no server. It also checks that the driver's list of result keys matches the keys `src/extract_fn.c` emits.
  * Backend crashes, assertion failures (`TRAP:`), hangs and unexpected errors fail the run.

A failure prints the seed and the round, and saves the round's SQL, psql output, server log and parameters under `tmp/fuzz-sql/fail-round-N/`. The container hands these files to the owner of `tmp/fuzz-sql/` (the host user), so they are readable on the host and in CI. `params.txt` there has the replay command. Some patterns hit core regex resource limits in the pre-check: "invalid memory alloc request size" for huge NFAs, or a compile that outlasts `statement_timeout`. These are counted as resource-limited rather than predicted:
* the check hook may accept them, and matching errors are then swallowed by design (`src/regex_runtime.c`);
* or it may reject them, including by timing out, since compiling is interruptible.

A pattern that takes longer than 100 ms to compile in the check hook is rejected by the compile time limit (`took longer than 100 ms`). That verdict is accepted for any pattern within the length limit, because the time limit is checked before the pattern's other compile verdicts.

## Checking that a harness catches bugs (mutation check)

Never edit `src/` for this. Copy it to `tmp/`, inject the bug into the copy, and point the harness at the copy:

```sh
mkdir -p tmp/mutant && cp -R src tmp/mutant/src
$EDITOR tmp/mutant/src/scan.c          # e.g. drop a bounds check
PSSC_FUZZ_SRC=tmp/mutant/src PSSC_FUZZ_OUT=tmp/mutant/out fuzz/run-libfuzzer.sh -t 120 fuzz_scan
PSSC_FUZZ_SRC=tmp/mutant/src fuzz/sql/run.sh -- --duration 120   # for src/regex_runtime.c, src/guc.c
```

Mutants used when the harnesses were written, all caught:

| Bug injected into the copy | Caught by |
|---|---|
| block-comment skip reads one byte past the range (`scan.c`) | `fuzz_scan`: ASan heap-buffer-overflow after 10 runs |
| escape lookahead past the body in the SQLCommenter value scan (`pairs.c`) | `fuzz_sqlcommenter`: ASan after 20 runs |
| signed overflow on a 0x7f lead byte (`pairs.c`) | `fuzz_sqlcommenter`: UBSan, exit 1 with a reproducer (the earlier recoverable build exited 0) |
| tag-count limit off by one (`tagset.c`) | `fuzz_tagset`: invariant `ntags <= max_tags` |
| line comments end at `\r` (`scan.c`) | `fuzz_scan`: invariant "a line comment contains no newline" |
| regex group offsets used as byte offsets (`regex_runtime.c`) | SQL driver, round 2: oracle mismatch |
| unmatched group (`rm_so = -1`) not skipped (`regex_runtime.c`) | SQL driver, round 52: oracle mismatch |
| every capture discarded and counted as dropped (`regex_runtime.c`) | SQL driver, round 2: missing tag, `dropped_tags`/`tagset_bytes` mismatch |

## CI

The `fuzz-smoke` job in `.github/workflows/ci.yml` runs `fuzz/run-libfuzzer.sh -t 15`, `perl fuzz/sql/regex_fuzz.pl --self-test` and uploads `tmp/fuzz/` artifacts and logs on failure. The `linux-source` PG 18 assert cell runs `fuzz/sql/run.sh --assert --pg 18 -- --duration 30` after its tests, against the `--enable-cassert` server, and uploads `tmp/fuzz-sql/` on failure.
