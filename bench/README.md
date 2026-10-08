# Benchmarks

`bench/` measures what pg_stat_statement_context costs on top of `pg_stat_statements` alone, with pgbench. The results, the method and what they can and cannot show are in [docs/benchmarks.md](../docs/benchmarks.md), which is generated from the raw data in [`results/`](results/).

| File | Role |
|---|---|
| `run.sh` | Host launcher: builds the per-checkout Docker image, runs `inside.sh` in one container, writes `campaign.json` and `report.md`, and with `--save` copies the raw results to `results/` |
| `inside.sh` | Runs inside the container: release build, server setup, the plan, the per-run checks |
| `scenarios.py` | The scenario matrix, the randomized block plan, the pgbench scripts, the exporter reader's queries |
| `analyze.py` | Per-run summaries, paired statistics (95% t-intervals), the report and `docs/benchmarks.md` |
| `test_analyze.py` | Tests of the two Python files (`python3 bench/analyze.py --self-test`) |
| `benchmarks.md.in` | The prose of `docs/benchmarks.md` |
| `results/<date>-<host>/` | One committed campaign: `campaign.json` (commit, host, settings, quiet check), `runs.jsonl` (one summary per run), `plan.tsv`, `env-*.txt`, and an optional hand-written `NOTES.md` |

## Running

Docker is needed; Python 3 (stdlib only) on the host.

```sh
bench/run.sh --dry-run                    # every scenario, 2 blocks of 2 s runs: a self-check (~5 min)
bench/run.sh                              # the default campaign: PG 18, 10 blocks of 15 s runs (~2.6 h)
bench/run.sh --major 14 --blocks 4        # another major, fewer blocks
bench/run.sh --only '^inlist-' --blocks 16  # a subset, more blocks
bench/run.sh --high-clients               # add the 256-client scenarios
bench/run.sh --save --label m1-docker     # save the raw results to bench/results/<date>-m1-docker/
python3 bench/analyze.py --self-test      # the analysis tests
```

Output goes to `tmp/bench-pgN[-dry]/` (git-ignored): `report.md`, `runs.jsonl`, `runs/*` (per-run summaries and pgbench output), `plan.tsv`, `env-host.txt`, `env-container.txt` and `campaign.json`. The run exits non-zero if the analysis self-test fails, a run fails its checks, or the server log shows a crash. `inside.sh` documents the `BENCH_*` environment variables (scale, IN-list length, CPU sets, interference threshold).

The campaign length is about `50 configurations × blocks × (duration + warmup + ~1 s)`. The width of a 95% interval shrinks roughly with the square root of the number of pairs: 16 blocks give intervals about 25% narrower than 10, and halving them takes about 4× the blocks.

### Publishing a campaign

1. Commit any change to the extension first: `--save` refuses to save results for uncommitted code in `src/`, `sql/` or the `Makefile` (`--allow-dirty` overrides, and the results record it).
2. Make sure nothing else runs on the machine: no other containers (`docker ps`), no builds, no browser. The run records the containers it saw at the start and at the end and the CPU used outside its container during each run.
3. `bench/run.sh --save --label <host>` (e.g. `c7i-metal-24xl`, `c8g-metal-24xl`).
4. Optionally add `bench/results/<date>-<host>/NOTES.md` with observations; it is included in the generated page.
5. Regenerate the page and commit both:

   ```sh
   python3 bench/analyze.py doc --results bench/results --template bench/benchmarks.md.in \
       --out docs/benchmarks.md --check-fresh
   git add bench/results/<date>-<host> docs/benchmarks.md
   git commit -m "bench: <host> campaign at <commit>"
   ```

   `--check-fresh` fails if a campaign's commit is not an ancestor of `HEAD`, or if the extension's code changed since it. Re-run (or drop the older campaign) after a change to `src/`. Older campaigns stay in `results/` as history; remove a directory to drop it from the page.

## Dedicated hardware

The local campaign ran in Docker Desktop on a laptop, which cannot resolve overheads of a few percent. These steps give numbers worth quoting. Run them on Linux x86-64 and on Graviton (arm64), and commit each as its own campaign.

**Instances.** Use whole-machine (`.metal`) instances where possible, so no other tenant shares the caches or memory bandwidth; otherwise the largest size of the family (which gets a whole socket) on dedicated tenancy.

- x86-64: `c7i.metal-24xl` (Intel Sapphire Rapids, 96 vCPUs) or `m7i.metal-24xl`; `c7a.metal-48xl` for AMD.
- Graviton: `c7g.metal` (Graviton3, 64 cores) or `c8g.metal-24xl` (Graviton4).
- A local SSD is not needed: the data set is 160 MB and `synchronous_commit = off`, so the runs are CPU-bound.

**Machine setup** (Amazon Linux 2023 or Ubuntu 24.04):

```sh
sudo dnf install -y docker git python3 || sudo apt-get install -y docker.io git python3
sudo systemctl start docker && sudo usermod -aG docker "$USER"   # log in again
# Fixed clock speed: the performance governor, and no turbo where the platform allows it.
sudo cpupower frequency-set -g performance 2>/dev/null || \
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null   # Intel
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost 2>/dev/null           # AMD (acpi-cpufreq)
# Graviton runs at a fixed frequency; nothing to do.
# Keep the benchmark CPUs free of other work (optional, needs a reboot): add
#   isolcpus=8-31 nohz_full=8-31
# to the kernel command line, or at least stop irqbalance and unneeded services.
sudo systemctl stop irqbalance 2>/dev/null
```

On metal instances you can disable SMT for steadier numbers (`echo off | sudo tee /sys/devices/system/cpu/smt/control`); record it in `NOTES.md` if you do.

**Pinning.** Give the container a fixed set of physical cores on one NUMA node with `--cpuset`. `inside.sh` pins the server to the first half of them and pgbench to the next 30%, so 16 CPUs give the server 8 and pgbench 4 (`BENCH_SERVER_CPUS` and `BENCH_CLIENT_CPUS` override the split):

```sh
lscpu -e                                   # pick CPUs of one socket / NUMA node, one per core if SMT is on
git clone https://github.com/benchub/pg_stat_statement_context && cd pg_stat_statement_context
git checkout <commit to measure>
bench/run.sh --dry-run --cpuset 8-23       # check that every scenario passes (~5 min)
bench/run.sh --cpuset 8-23 --blocks 16 --save --label c7i-metal-24xl
bench/run.sh --cpuset 8-23 --blocks 8 --high-clients --only 'c256$' --save --label c7i-metal-24xl-c256
```

With 8 server CPUs, *N* = 8, so the client counts become 1, 8 and 32. Docker on native Linux adds only cgroup accounting, not a VM; the in-container `/proc/stat` is the host's, so the interference monitor sees every process on the machine.

**Without Docker.** `inside.sh` is written for the PGDG Debian image (it builds the extension, runs `initdb` as `postgres` with `gosu` and expects `/src` and `/out`). To run without Docker, use the same image's steps on the host: install the PGDG packages of the major (`postgresql-18`, `postgresql-server-dev-18`, `postgresql-contrib`), `gosu`, `util-linux` (`taskset`) and `python3`, then run it as root with `PSSC_SRC` set to the checkout and `PSSC_OUT` to an empty output directory, and the `BENCH_*` variables that `run.sh` would pass. It creates its cluster under `/var/lib/postgresql/bench`. `run.sh`'s `campaign.json` and `--save` steps then have to be done by hand. This path is untested; Docker with `--cpuset` is the supported way.

**Before committing**, check in the generated report that the Validation table has no retries (or few), that the baselines' TPS spread is small (a few percent), and that `campaign.json`'s `quiet_check` shows no other containers.
