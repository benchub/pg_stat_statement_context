# Benchmarks

Overhead and latency of pg_stat_statement_context measured with pgbench against `pg_stat_statements` alone. 

The numbers below come from a **Docker Desktop VM on a laptop (macOS, Apple M1 Max)**, not production hardware. Read them as relative costs with a resolution of roughly ±5–10%, not as absolute throughput. The scripts are in [`bench/`](../bench/) and anyone can re-run them.

## Running

```sh
bench/run.sh                              # full run, PostgreSQL 18, about 45 minutes
bench/run.sh --major 14 --quick           # smoke run, 1 x 10s per configuration, about 5 minutes
bench/run.sh --only '^evict/' --runs 9    # a subset, more rounds
python3 bench/analyze.py --self-test      # analysis self-test (stdlib only)
```

Options: 
* `--major N` (PGDG image, default 18)
* `--quick`
* `--runs R` (default 5)
* `--duration S` (default 20)
* `--only REGEX` and `--keep-logs` (keeps the gzipped per-transaction logs)

`bench/inside.sh` documents the `BENCH_*` environment variables (clients, CPU pinning, IN-list length, boundary window, interference threshold).

The run reuses the image of `scripts/docker-test.sh N` (this checkout's `pg_stat_statement_context-test:pgN-<hash>`, built from `docker/Dockerfile`; see `scripts/docker-test.sh` for the tag scheme and `--prune`), so it creates no other image. It runs one `docker run --rm` container and leaves nothing behind. Output goes to `tmp/bench-pgN[-quick]/`:

- `results.md` and `results.json`: the tables below;
- `runs/*.json`: one summary per run, plus pgbench's own output;
- `env-host.txt` and `env-container.txt`: the environment and settings.

`bench/run.sh` will fail with a non-zero exit code in any of these cases:

- the analysis self-test fails
- a configuration fails one of its checks (see [Validation](#validation))
- the server logs a crash

## Method

- **One container, Unix socket.** The server and pgbench share the container, so there is no network noise. The server is pinned with `taskset` to CPUs 0–4 and pgbench to CPUs 5–7. Two vCPUs are left idle so that the busy ones can stay on performance cores.
- **pgbench.** `pgbench -M simple -c 8 -j 3 -T 20`, scale 10, with a 3s warmup. The server is restarted before every run and the stats of both extensions are reset after the warmup. Only the simple query protocol is measured.
- **Server settings.**
  - `shared_buffers = 1GB`
  - `autovacuum = off`
  - `jit = off`
  - `max_connections = 50`
  - `pg_stat_statements` defaults (`max = 5000`, `track = top`)
  - This extension uses its defaults unless a configuration says otherwise:
    - `extractors = 'sqlcommenter, marginalia'`
    - `tags = action, controller, job`
    - `scan_window = 2kB`
    - `max_entries = 10000`
    - 12 × 5 min buckets
  - `shared_preload_libraries = 'pg_stat_statements, pg_stat_statement_context'` or the corresponding subset.
- **Rounds.** 5 rounds; each round runs every configuration once. Odd rounds run in list order and even rounds in reverse.
- **Deltas** (ΔTPS, Δavg, Δp99, Δmax) are computed in three steps:
  1. In each round, a run is divided by its workload's `…/`pg_stat_statements`` run from the same round.
  2. Rounds 2k−1 (forward) and 2k (reverse) form a balanced pair, and the pair's ratio is the geometric mean of its two per-round ratios. Drift that is smooth over the course of a round (the same slowdown per position in the order) cancels exactly within a pair. A plain median over 3 forward and 2 reverse rounds does not cancel it: with identical performance and a 1% slowdown per position, it reports −2.97% for `append/ext-1s-buckets` against `append/`pg_stat_statements``, three positions apart. The self-test covers this case.
  3. The tables show the median over pairs, and ΔTPS also shows [min, max] over pairs.
- **Odd round counts.** A round without a partner is left out of the deltas: round 5 of the full run, so its deltas rest on **two pairs**. Their "median" is the mean of the two, and [min, max] are the two pair values.
- **Single-round runs.** A run with a single round (`--quick`) has no pair. Its deltas are single-round ratios, marked † and not drift-corrected.
- **Absolute columns** are medians over all runs (round 5 included). "TPS spread" is (max − min) / median.
- **Latencies** come from pgbench's per-transaction log (`--log`). Percentiles are nearest-rank over all transactions of a run.
- **Bucket boundaries.**
  - Buckets start at multiples of the interval since 2000-01-01, so with 1s buckets every whole second is a boundary.
  - A transaction counts as *near* a boundary when its execution `[end − latency, end]` overlaps ±5 ms of a whole second; all others are *far*.
  - Every configuration is analyzed on the same 1s grid. The configurations  that do not use 1s buckets act as controls.
- **Interference.** Docker containers see the VM-wide `/proc/stat`. For each run, the CPU time used outside the benchmark container (VM busy time minus the container cgroup's `usage_usec`) is recorded as "foreign CPU". A run is repeated, up to 3 attempts, when other containers used more than 0.75 cores during it. Stalls of the macOS host or the VM itself are not visible from inside and remain as noise.

## Workloads

Each script picks `aid = random(1, 1000000)` and runs `SELECT abalance FROM pgbench_accounts WHERE aid = :aid` (the `pgbench -S` statement):

| Workload | Statement |
|---|---|
| `plain` | the bare statement with a trailing `;` (no comment) |
| `append` | statement, then a 190-byte sqlcommenter comment with six tags (`action`, `controller`, `db_driver`, `framework`, `route`, `traceparent`, URL-encoded values); two of them pass the default tag allowlist |
| `prepend` | the same comment before the statement |
| `inlist` | `… AND abalance NOT IN (-10000, …, -1)`: a 10,000-element IN list (59 KB), then the comment and `;` (stmt_len > 0) |
| `inlist0` | the same without the trailing `;`. pgbench then sends the statement as is, so the server sees stmt_len = 0, the `strlen()` case of §6.2 |
| `evict` | `append`, but `controller='users:r'` with `\set r random(1, 1000000000)`: a new tag set nearly every transaction |

The configurations per workload:

| Configuration | Settings |
|---|---|
| `none` | no library preloaded |
| ``pg_stat_statements`` | pg_stat_statements only: the baseline |
| `ext` | `pg_stat_statements` + this extension, defaults (append mode; for `plain`, no comment at all) |
| `ext-any` | `extractors = 'sqlcommenter(position=any), marginalia(position=any)'`: an exact scan of the whole statement |
| `ext-activity-reader` | `ext`, plus a session that reads `pg_stat_statement_context_activity` every 1 ms during the run (see [Activity view](#activity-view)) |
| `ext-1s-buckets` | `bucket_interval = '1s'`, `bucket_count = 50`: about 20 bucket rollovers per run |
| `prepend/ext` | `extractors = '…(position=prepend), …'` |
| `ext-window-1MB` | `scan_window = '1MB'`: the window covers the whole 59 KB statement, so the scan is exact rather than heuristic |
| `ext-max1000`, `ext-max10000` | `max_entries` = 1000 or 10000, with sustained eviction |
| `ext-cap` | `cardinality_cap = 100` (see [Cardinality caps](#cardinality-caps)) |
| `evict/ext-cap100`, `evict/ext-cap-full` | `cardinality_cap = 100`; or `cardinality_cap = 1000000` with `cardinality_cap_slots = 256` (a full table) |

## Results: PostgreSQL 18.6, full run

The full run took 43 minutes on 2026-10-06 (git 22f9e0f).

Environment:

- Host: Apple M1 Max (8 performance + 2 efficiency cores), 32 GB, macOS 26.7.1.
- Docker Desktop 29.8.1; the VM has 10 vCPUs and 7.7 GB, kernel 7.0.14-linuxkit aarch64.
- Image `postgres:18` (PGDG 18.6-1.pgdg13+2).
- Seven unrelated containers of other projects were running, mostly idle (about 0.3–0.7 cores in total; see the foreign CPU column).

| Configuration | Runs | TPS (median) | TPS spread | ΔTPS vs `pg_stat_statements` | avg ms | Δavg | p99 ms | Δp99 | max ms (median / worst) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| plain/none | 5 | 58547 | 35.2% | -2.3% [-9.9%, +5.4%] | 0.129 | +2.8% | 0.463 | +4.7% | 15.59 / 46.12 |
| plain/`pg_stat_statements` | 5 | 62451 | 35.2% | +0.0% [+0.0%, +0.0%] | 0.121 | +0.0% | 0.379 | +0.0% | 22.24 / 133.79 |
| plain/ext | 5 | 57927 | 35.7% | -13.1% [-17.7%, -8.5%] | 0.131 | +15.8% | 0.433 | +35.0% | 43.81 / 83.90 |
| append/none | 5 | 61187 | 40.2% | -1.8% [-4.4%, +0.9%] | 0.123 | +1.7% | 0.383 | +12.8% | 16.34 / 87.29 |
| append/`pg_stat_statements` | 5 | 63541 | 37.9% | +0.0% [+0.0%, +0.0%] | 0.119 | +0.0% | 0.364 | +0.0% | 18.66 / 54.61 |
| append/ext | 5 | 55579 | 50.8% | -8.8% [-13.2%, -4.4%] | 0.137 | +10.3% | 0.444 | +14.3% | 22.80 / 28.94 |
| append/ext-any | 5 | 53526 | 64.5% | -27.0% [-47.9%, -6.2%] | 0.142 | +51.0% | 0.492 | +150.1% | 19.61 / 84.13 |
| append/ext-1s-buckets | 5 | 52263 | 26.9% | -15.5% [-23.1%, -7.8%] | 0.145 | +20.1% | 0.494 | +61.3% | 70.05 / 140.72 |
| prepend/`pg_stat_statements` | 5 | 58205 | 26.5% | +0.0% [+0.0%, +0.0%] | 0.130 | +0.0% | 0.419 | +0.0% | 18.15 / 146.98 |
| prepend/ext | 5 | 54646 | 47.3% | -11.7% [-21.6%, -1.7%] | 0.139 | +15.2% | 0.449 | +47.2% | 15.36 / 334.70 |
| inlist/`pg_stat_statements` | 5 | 709 | 4.9% | +0.0% [+0.0%, +0.0%] | 11.270 | +0.0% | 26.528 | +0.0% | 112.01 / 168.71 |
| inlist/ext | 5 | 709 | 23.2% | -1.6% [-6.2%, +3.0%] | 11.260 | +1.9% | 26.823 | +30.3% | 134.66 / 368.12 |
| inlist/ext-any | 5 | 660 | 14.1% | -4.8% [-5.2%, -4.3%] | 12.104 | +5.0% | 29.766 | +7.7% | 186.09 / 763.41 |
| inlist/ext-window-1MB | 5 | 703 | 42.4% | -11.0% [-23.5%, +1.5%] | 11.366 | +14.6% | 26.053 | +54.1% | 83.17 / 1096.94 |
| inlist0/`pg_stat_statements` | 5 | 702 | 17.9% | +0.0% [+0.0%, +0.0%] | 11.386 | +0.0% | 25.707 | +0.0% | 117.62 / 272.26 |
| inlist0/ext | 5 | 682 | 34.9% | -7.0% [-16.9%, +3.0%] | 11.704 | +8.7% | 28.346 | +15.1% | 117.84 / 159.38 |
| inlist0/ext-any | 5 | 626 | 37.8% | -11.3% [-23.4%, +0.8%] | 12.764 | +14.8% | 33.044 | +45.2% | 136.71 / 565.60 |
| evict/`pg_stat_statements` | 5 | 56125 | 31.7% | +0.0% [+0.0%, +0.0%] | 0.135 | +0.0% | 0.477 | +0.0% | 24.61 / 34.83 |
| evict/ext-max1000 | 5 | 47868 | 19.7% | -14.7% [-21.0%, -8.4%] | 0.159 | +18.9% | 0.565 | +34.9% | 24.67 / 40.74 |
| evict/ext-max10000 | 5 | 49554 | 26.7% | -14.0% [-21.3%, -6.7%] | 0.154 | +18.3% | 1.200 | +174.6% | 35.73 / 42.08 |

Bucket boundaries: p99 and max of transactions within ±5 ms of a whole second, compared with the rest (medians over runs; the worst run is shown for max):

| Configuration | grid | window | boundaries/run | near n | near p99 ms | near max ms (median / worst) | far p99 ms | far max ms (median / worst) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| plain/none | 1s | ±5 ms | 20 | 12501 | 0.345 | 2.41 / 10.99 | 0.463 | 15.59 / 46.12 |
| plain/`pg_stat_statements` | 1s | ±5 ms | 20 | 12393 | 0.297 | 4.31 / 9.90 | 0.380 | 22.24 / 133.79 |
| plain/ext | 1s | ±5 ms | 20 | 10789 | 0.472 | 14.82 / 23.02 | 0.433 | 43.81 / 83.90 |
| append/none | 1s | ±5 ms | 20 | 11559 | 0.495 | 11.54 / 16.51 | 0.382 | 16.34 / 87.29 |
| append/`pg_stat_statements` | 1s | ±5 ms | 20 | 12779 | 0.316 | 3.59 / 10.00 | 0.365 | 18.66 / 54.61 |
| append/ext | 1s | ±5 ms | 20 | 11508 | 0.349 | 2.17 / 3.09 | 0.445 | 22.80 / 28.94 |
| append/ext-any | 1s | ±5 ms | 20 | 10537 | 0.486 | 6.89 / 47.74 | 0.491 | 19.61 / 84.13 |
| append/ext-1s-buckets | 1s | ±5 ms | 20 | 10638 | 0.505 | 8.02 / 49.68 | 0.493 | 70.05 / 140.72 |
| prepend/`pg_stat_statements` | 1s | ±5 ms | 20 | 11503 | 0.468 | 6.03 / 79.92 | 0.418 | 18.15 / 146.98 |
| prepend/ext | 1s | ±5 ms | 20 | 11137 | 0.422 | 8.05 / 35.22 | 0.449 | 15.36 / 334.70 |
| inlist/`pg_stat_statements` | 1s | ±5 ms | 20 | 303 | 33.148 | 48.71 / 84.58 | 25.374 | 112.01 / 168.71 |
| inlist/ext | 1s | ±5 ms | 20 | 296 | 60.966 | 79.43 / 368.12 | 26.827 | 134.66 / 324.35 |
| inlist/ext-any | 1s | ±5 ms | 20 | 289 | 41.177 | 62.68 / 763.41 | 29.306 | 186.09 / 691.34 |
| inlist/ext-window-1MB | 1s | ±5 ms | 20 | 302 | 36.024 | 75.28 / 1096.94 | 25.840 | 83.17 / 717.50 |
| inlist0/`pg_stat_statements` | 1s | ±5 ms | 20 | 299 | 62.116 | 64.79 / 89.20 | 25.668 | 117.62 / 272.26 |
| inlist0/ext | 1s | ±5 ms | 20 | 304 | 28.110 | 33.88 / 66.38 | 28.610 | 117.84 / 159.38 |
| inlist0/ext-any | 1s | ±5 ms | 20 | 287 | 43.361 | 55.07 / 565.60 | 32.684 | 136.71 / 429.38 |
| evict/`pg_stat_statements` | 1s | ±5 ms | 20 | 10869 | 0.402 | 11.19 / 18.60 | 0.479 | 24.61 / 34.83 |
| evict/ext-max1000 | 1s | ±5 ms | 20 | 10128 | 0.488 | 5.37 / 9.63 | 0.566 | 24.67 / 40.74 |
| evict/ext-max10000 | 1s | ±5 ms | 20 | 9820 | 1.254 | 3.77 / 9.38 | 1.199 | 35.73 / 42.08 |

<a id="validation"></a>Validation: what the run checked. Every run was checked; the table shows the values from the greatest round of each configuration, the last one run.

| Configuration | checks | foreign CPU (max) | retries |
|---|---|---:|---:|
| plain/none | txns=1059451 | 0.55 | 0 |
| plain/`pg_stat_statements` | txns=982533, `pg_stat_statements`_calls=982533 | 0.54 | 0 |
| plain/ext | txns=853088, `pg_stat_statements`_calls=853088, ext_rows=0 | 0.66 | 0 |
| append/none | txns=1094788 | 0.52 | 0 |
| append/`pg_stat_statements` | txns=1366380, `pg_stat_statements`_calls=1366380 | 0.55 | 1 |
| append/ext | txns=1191642, ext_calls=1191642, `pg_stat_statements`_calls=1191642, ext_rows=1 | 0.72 | 1 |
| append/ext-any | txns=1096050, ext_calls=1096050, `pg_stat_statements`_calls=1096050, ext_rows=1 | 0.55 | 0 |
| append/ext-1s-buckets | txns=1241915, ext_calls=1241915, `pg_stat_statements`_calls=1241915, ext_rows=1, live_buckets=21 | 0.44 | 0 |
| prepend/`pg_stat_statements` | txns=1290749, `pg_stat_statements`_calls=1290749 | 0.64 | 0 |
| prepend/ext | txns=1160674, ext_calls=1160674, `pg_stat_statements`_calls=1160674, ext_rows=1 | 0.43 | 0 |
| inlist/`pg_stat_statements` | txns=14380, `pg_stat_statements`_calls=14380 | 0.64 | 0 |
| inlist/ext | txns=11294, ext_calls=11294, `pg_stat_statements`_calls=11294, ext_rows=1, heuristic_scans=11294 | 0.70 | 0 |
| inlist/ext-any | txns=13067, ext_calls=13067, `pg_stat_statements`_calls=13067, ext_rows=1, heuristic_scans=0 | 0.63 | 0 |
| inlist/ext-window-1MB | txns=14062, ext_calls=14062, `pg_stat_statements`_calls=14062, ext_rows=1, heuristic_scans=0 | 0.75 | 0 |
| inlist0/`pg_stat_statements` | txns=14863, `pg_stat_statements`_calls=14863 | 0.72 | 2 |
| inlist0/ext | txns=14789, ext_calls=14789, `pg_stat_statements`_calls=14789, ext_rows=1, heuristic_scans=14789 | 0.75 | 1 |
| inlist0/ext-any | txns=12856, ext_calls=12856, `pg_stat_statements`_calls=12856, ext_rows=1, heuristic_scans=0 | 0.51 | 0 |
| evict/`pg_stat_statements` | txns=1339027, `pg_stat_statements`_calls=1339027 | 0.60 | 0 |
| evict/ext-max1000 | txns=1100860, dealloc=21998, evicted_entries=1099900, entries=960, distinct_controllers=960 | 0.72 | 1 |
| evict/ext-max10000 | txns=1164409, dealloc=2309, evicted_entries=1154500, entries=9897, distinct_controllers=9897 | 0.74 | 1 |

## Results: PostgreSQL 14.24, quick run

This is a smoke run: one round of 10s per configuration with a 2s warmup. All deltas are single-round ratios (unpaired, †) and are not drift-corrected. Single runs are noisy (±15%), so only large effects are meaningful. All checks passed. Selected rows:

| Configuration | TPS | ΔTPS vs `pg_stat_statements` | p99 ms | Δp99 |
|---|---:|---:|---:|---:|
| plain/`pg_stat_statements` | 75306 | – | 0.255 | – |
| plain/ext | 74073 | −1.6% | 0.274 | +7.5% |
| append/ext | 61277 | −11.0% | 0.469 | +49.8% |
| append/ext-1s-buckets | 68686 | −0.2% | 0.286 | −8.6% |
| prepend/ext | 68291 | +1.1% | 0.301 | −3.5% |
| inlist/ext | 689 | +6.6% | 21.751 | −30.6% |
| inlist0/ext-any | 736 | +10.5% | 21.565 | −14.1% |
| evict/`pg_stat_statements` | 78030 | – | 0.218 | – |
| evict/ext-max1000 | 64113 | −17.8% | 0.295 | +35.3% |
| evict/ext-max10000 | 63198 | −19.0% | **1.089** | **+399.5%** |

The two `append/ext` variants (−11.0% and −0.2%) differ by more than any real effect, which shows the noise of a single, unpaired run. The eviction p99 is the only effect that reproduces clearly, matching PG 18.

## Activity view

The [activity view](sql-interface.md#pg_stat_statement_context_activity) adds a write to the backend's shared slot at the start and end of each top-level statement. Readers never block writers: the slot uses a change counter, like `pg_stat_activity`. Measured on PG 18.6 (Docker on an M1, `max_connections = 100`, so 136 slots).

**Microbenchmark** (`pssc_context_test_activity_bench(loops, tags_bytes)` in `test/modules/pssc_context_test`; the cost of one publish plus one set-idle, the pair a statement causes, and of reading all slots):

| Tag set size | Writer, per pair | Reader, all 136 slots |
|---:|---:|---:|
| 0 B | 5.6 ns | ~0.5–0.6 µs |
| 30 B (typical) | 5.9 ns | |
| 190 B | 44 ns | |
| 512 B (`max_tagset_bytes`) | 110 ns | |

A `SELECT` publishes at `ExecutorRun` and again at `ExecutorFinish`, so it makes two pairs. That is about 12 ns per statement with typical tags, or 0.01% of a 0.12 ms point select.

**Reading the view from SQL** (pgbench, one client, 9 backends): `SELECT count(*) FROM pg_stat_statement_context_activity` takes 0.077 ms, `pg_stat_activity` takes 0.289 ms, and the join of both takes 0.355 ms.

**pgbench A/B** (`append` workload, 6 rounds of each configuration):

| Tree | Configuration | ΔTPS vs `pg_stat_statements` [min, max] | Δp99 |
|---|---|---:|---:|
| without the view | `append/ext` | −2.1% [−2.2%, +5.4%] | +3.2% |
| with the view | `append/ext` | −4.6% [−5.3%, −3.7%] | +9.2% |
| with the view | `append/ext-activity-reader` | −5.2% [−7.5%, −2.9%] | +13.2% |

The two trees were run one after the other, in different sessions. The baselines' TPS spread was 14.6% and 22.5%, so the difference between the trees, 2.5 points, is within the noise ([Caveats](#caveats)). A difference of 12 ns per statement cannot be resolved this way; the microbenchmark is the measurement. A reader polling the view every 1 ms (9416 reads, 3149 of which saw a tagged active row) did not measurably slow the writers: −5.2% against −4.6%. Reproduce with:

```sh
bench/run.sh --runs 6 --only '^append/(`pg_stat_statements`|ext|ext-activity-reader)$'
```

## Cardinality caps

With a [cardinality cap](configuration.md#cardinality_cap) set, each kept tag costs a lookup in a shared, lock-free hash table: a hash of key and value, then a few atomic reads (one compare-and-swap more for a new value). No lock is taken. Measured on PG 18.6 (Docker on an M1).

**Microbenchmark**: 200,000 calls of [`pg_stat_statement_context_extract()`](sql-interface.md#pg_stat_statement_context_extract) on `SELECT 1 /*action='show',controller='users'*/` (2 kept tags) in a PL/pgSQL loop; median of 5 alternating rounds per case. `_extract()` only peeks at the caps, but for an admitted value that is the hooks' path too:

| Case | ns per call | vs caps off |
|---|---:|---:|
| caps off (`cardinality_cap = 0`: no check at all) | 3244 | |
| both values admitted (steady state) | 3305 | +61 ns (≈ 30 ns per tag) |
| `controller` over its cap (collapses to `null`) | 3282 | +38 ns |
| table full (`cardinality_cap_slots = 256`, 64 probes per tag) | 3463 | +144 ns (vs 3319 off in the same run) |

That is about 0.03% of a 0.12 ms point select per capped tag, and well under 0.2% even in the worst case of a full table.

**pgbench A/B** (6 rounds; `ext-cap`: `cardinality_cap = 100`; `evict/ext-cap100`: the `evict` workload's random `controller` collapses after 100 values; `evict/ext-cap-full`: `cardinality_cap = 1000000`, `cardinality_cap_slots = 256`, so nearly every value fails closed):

| Configuration | ΔTPS vs `pg_stat_statements` [min, max] | Δp99 | Checks |
|---|---:|---:|---|
| `append/ext` | −7.0% [−9.6%, +9.2%] | +16.2% | |
| `append/ext-cap` | −7.1% [−16.9%, +3.0%] | +15.6% | 1 entry, `capped_tags` 0 |
| `evict/ext-max10000` | −12.3% [−19.1%, +11.2%] | +60.0% | 1630 eviction passes |
| `evict/ext-cap100` | −4.4% [−24.4%, +24.6%] | +0.2% | 100 distinct values + `null`, no eviction |
| `evict/ext-cap-full` | +1.9% [−6.5%, +20.7%] | −18.4% | 255 values, the rest `cap_table_full` |

Ten other containers were running, and the baselines' TPS spread was up to 59%, so these deltas are noise apart from their sign. The microbenchmark is the measurement. As expected, a cap on a flooded key removes the eviction churn: the entries stay at about 100 instead of turning over the whole table. Reproduce with:

```sh
bench/run.sh --runs 6 --only '^(append/(`pg_stat_statements`|ext|ext-cap)|evict/(`pg_stat_statements`|ext-max10000|ext-cap100|ext-cap-full))$'
```

## Findings

1. **Steady-state overhead is not resolved by this setup. It is at most about 10–15% on a 0.12 ms point select and possibly much less.**
   - In the full run, every `ext` configuration of the short-statement workloads is slower than `pg_stat_statements` alone: `append/ext` −8.8%, `prepend/ext` −11.7%, `plain/ext` (no comment) −13.1%, `append/ext-1s-buckets` −15.5%. That is about 10–20 µs per transaction.
   - `append/ext-any` shows −27.0%, but its two pair values are −47.9% and −6.2%. The first pair contains a round at −71% that coincided with interference.
   - With two pairs per configuration these figures are fragile, and they contradict a dedicated run. That run had 9 rounds (4 pairs) of only `plain/`pg_stat_statements`` and `plain/ext`, and gave **+1.9% [−7.2%, +23.2%]** for the configuration the full run put at −13.1%.
   - The code agrees with the dedicated run: an untagged statement takes no lock and only scans the comment window.
   - A real per-statement cost of a few percent is plausible: frame setup, comment scanning, and for tagged statements a shared LWLock and two `GetCurrentTimestamp()` calls. Resolving it needs quieter hardware or many more rounds.
2. **No latency spike at bucket boundaries.**
   - With 1s buckets (about 20 rollovers per run, every one verified as a separate live bucket), the near-boundary p99 is 0.505 ms against 0.493 ms elsewhere.
   - The near-boundary max is 8 ms (median) against 70 ms elsewhere. The controls look the same (`append/`pg_stat_statements``: 0.316 ms against 0.365 ms).
   - This matches the design (§5.2): the boundary advance is a lock-free compare-and-swap, and slots roll over lazily under the per-entry spinlock.
3. **The large IN list costs little beyond `pg_stat_statements`, including the stmt_len = 0 `strlen()` case.** Parsing and planning a 10,000-element list (a 59 KB statement, about 700 TPS and 11 ms per query) dominate.
   - Heuristic append mode (the default, 2 kB window): `inlist/ext` −1.6% [−6.2%, +3.0%] and `inlist0/ext` −7.0% [−16.9%, +3.0%]. Both ranges include zero.
   - Exact scans of the whole statement:
     - `inlist/ext-any` −4.8% [−5.2%, −4.3%]: consistent, about 0.5 ms per 59 KB statement.
     - `inlist/ext-window-1MB` −11.0% [−23.5%, +1.5%] and `inlist0/ext-any` −11.3% [−23.4%, +0.8%]: noisy.
   - So `position=any` or a large `scan_window` costs on the order of 0.5–1 ms per 59 KB statement. The default heuristic mode avoids that.
   - `heuristic_scans` confirms which path each configuration took.
4. **Sustained eviction raises p99 latency about 2.7× (PG 18, paired) to 5× (PG 14, single round) at `max_entries = 10000`.** This is the one clear performance finding. It was measured before the two updates at the end of this item, which bring the PG 18 ratio down to about 1.3–1.5×.
   - Evidence, PG 18, 5 rounds:
     - `evict/ext-max10000`: p99 1.200 ms against 0.477 ms for `evict/`pg_stat_statements``. The paired delta is +174.6%, with pair values +154.9% and +194.2%.
     - In every round the extension's p99 was 1.13–1.27 ms, against 0.32–0.53 ms for `pg_stat_statements` alone. The earlier full run, before the interference monitor, gave +202%.
     - TPS −14.0%.
     - A median of 1,962 eviction passes per 20s run (about 100 per second) over the five rounds, each removing 500 entries (5%).
   - With `max_entries = 1000` there is a median of 19,122 passes of 50 entries per run, yet p99 rises only 35% (pairs +26% and +44%).
   - Cause (from src/store.c): every pass holds the store's exclusive lock while it scans the whole hash table, then copies and `qsort`s every live entry (the `pg_stat_statements` `entry_dealloc` approach). Every backend that records meanwhile waits on that lock.
   - So the cost of a pass grows with `max_entries`. At 10,000 entries a pass takes on the order of a millisecond, and the p99 equals that stall.
   - The workload is pathological on purpose: a new tag value on almost every transaction. The configuration docs warn against high-cardinality tags, and `dealloc` in `_info()` reveals this condition.
   - Fixes worth considering:
     - select the victims with a partial selection (`nth_element`-style) or a  heap instead of a full sort;
     - evict a larger batch per pass when passes come close together;
     - otherwise shorten the time the exclusive lock is held.
   - **Update (item 20261006-043919-1):** a pass now scans once and picks the victims with a bounded heap instead of copying and sorting every entry. Single-pass timings on PG 18 with 10,000 live entries and 500 victims (medians): 953–1,181 µs before, 545–733 µs after. What remains is the walk over ~8.7 MB of entries. Re-measured with `bench/run.sh --major 18 --only evict --runs 5` (host load average ~20, so treat as indicative):

     | Config | p99 before → after (ms) | Δp99 vs `pg_stat_statements` | ΔTPS vs `pg_stat_statements` |
     |---|---|---|---|
     | `pg_stat_statements` alone | 0.436 → 0.380 | — | — |
     | ext, `max_entries=10000` | 1.291 → 0.818 | +196% → +115% | −21.1% → −16.0% |
     | ext, `max_entries=1000` | 0.633 → 0.444 | +39% → +11% | −18.5% → −7.9% |

     A shared-lock scan can't help this workload, since every statement inserts a new key. A throwaway build that evicted 20% per pass gave Δp99 +34%, but it changes §5.3 semantics; 20261006-075124-1 tracks that decision.
   - **Update (item 20261006-075124-1):** a pass no longer walks the hash table. It scans a compact array of 24-byte `(last_bucket, usage, entry)` slots, one per entry, in shared memory (DESIGN.md §5.3). It touches a full entry only when it removes it. The array costs `24 × max_entries` bytes of shared memory (240 kB at the default). The semantics are unchanged: the target is still 5%, and dead entries go first.
     - Single pass, PG 18.6, 10,000 live entries, 500 victims, 80 passes per build in two alternating rounds (timed around the record call that triggered the pass):
       - median 733 and 719 µs before, 273 and 270 µs after;
       - p90 1,019 and 898 µs before, 344 and 317 µs after.
     - Two paired runs of `bench/run.sh --major 18 --only '^evict/(`pg_stat_statements`|ext-max10000)$' --runs 5`      per build. The baseline is the parent commit, run just before. No foreign containers were running.

       | Run | Build | `pg_stat_statements` p99 (ms) | ext p99 (ms) | Δp99 vs `pg_stat_statements` | ΔTPS vs `pg_stat_statements` |
       |---|---|---:|---:|---:|---:|
       | 1 | before | 0.340 | 0.800 | +122.1% | −12.3% |
       | 1 | after | 0.326 | 0.481 | +67.9% | −19.4% [−23.8%, −15.1%] |
       | 2 | before | 0.288 | 0.744 | +127.0% | −11.4% [−15.5%, −7.4%] |
       | 2 | after | 0.325 | 0.425 | +25.8% | −7.6% [−10.4%, −4.8%] |

     - The ratio of median p99s is now 1.48× and 1.31× `pg_stat_statements` alone, against 2.35× and 2.58× before. That meets the ~1.5× target of the item.
     - The paired Δp99 of run 1 (+67.9%) is above the target. In run 2 it is +25.8%. Run 1 had other agents' containers running between rounds.
     - The ΔTPS of run 1 (−19.4%) did not reproduce in run 2 (−7.6%, better than the −11.4% before), so it is noise rather than a cost of keeping the array up to date.
     - A pass still holds the exclusive lock for about 0.27 ms at 10,000 entries. That remains the floor of the p99 under this workload.
5. **Noise.**
   - Medians of the same configuration range 25–65% between runs (the TPS spread column).
   - Single transactions stall for tens or hundreds of milliseconds in every configuration, including `none`. A 0.3s stall happened in one `prepend/ext` run without any foreign CPU, so the VM or the host paused it.
   - Max latency is therefore not comparable between configurations here. Use dedicated Linux hardware to resolve effects below about 10%.

## Caveats

- **The hardware is not production-like.** Docker Desktop runs a Linux VM on macOS arm64. Its vCPUs float across performance and efficiency cores, and the host's scheduler, power management and other processes affect it. Absolute TPS and latency figures say nothing about a production server.
- **Short, read-only, fully cached statements are the worst case for relative overhead.** The fixed per-statement cost is largest compared with a 0.1 ms query. Real workloads with more expensive statements see proportionally less.
- **Not measured:**
  - the extended or prepared protocols (pgbench `-M simple` only);
  - write workloads;
  - more clients than CPUs;
  - `track = all` and nested statements;
  - regex and normalize extractors;
  - `untagged = record`.
- **Most configurations use the default 5 min buckets,** so their runs never cross a bucket boundary. Only `ext-1s-buckets` exercises rollovers.
