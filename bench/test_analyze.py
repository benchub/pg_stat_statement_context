"""Self-test for bench/analyze.py with known inputs (stdlib unittest only).

Run: python3 bench/analyze.py --self-test   (or python3 -m unittest bench/test_analyze.py)
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402
import scenarios  # noqa: E402


class Percentile(unittest.TestCase):
    def test_nearest_rank(self):
        xs = list(range(1, 101))  # 1..100
        self.assertEqual(analyze.percentile(xs, 50), 50)
        self.assertEqual(analyze.percentile(xs, 99), 99)
        self.assertEqual(analyze.percentile(xs, 100), 100)
        self.assertEqual(analyze.percentile(xs, 0.5), 1)
        self.assertEqual(analyze.percentile([7], 99), 7)
        # 1000 values: p99 is the 990th smallest, not an interpolation.
        self.assertEqual(analyze.percentile(list(range(1000)), 99), 989)
        # Input order does not matter.
        self.assertEqual(analyze.percentile([5, 1, 4, 2, 3], 50), 3)

    def test_empty(self):
        self.assertIsNone(analyze.percentile([], 99))

    def test_summary(self):
        s = analyze.summarize([400, 100, 300, 200])  # microseconds
        self.assertEqual(s["n"], 4)
        self.assertAlmostEqual(s["avg_ms"], 0.25)
        self.assertAlmostEqual(s["p50_ms"], 0.2)
        self.assertAlmostEqual(s["p99_ms"], 0.4)
        self.assertAlmostEqual(s["max_ms"], 0.4)
        self.assertEqual(analyze.summarize([])["n"], 0)


class ParseLog(unittest.TestCase):
    def test_parse_lines(self):
        lines = [
            "0 0 250 0 1700000000 100\n",
            "1 0 1250 0 1700000000 999999\n",
            "0 1 300 0 1700000001 5 \n",
            "\n",
        ]
        ends, lats = analyze.parse_log_lines(lines)
        self.assertEqual(lats, [250, 1250, 300])
        self.assertEqual(ends, [1700000000000100, 1700000000999999, 1700000001000005])

    def test_failed_transaction_rejected(self):
        with self.assertRaises(ValueError):
            analyze.parse_log_lines(["0 0 failed 0 1700000000 100\n"])
        with self.assertRaises(ValueError):
            analyze.parse_log_lines(["0 0 skipped 0 1700000000 100\n"])

    def test_pgbench_tps(self):
        out = ("number of failed transactions: 0 (0.000%)\n"
               "latency average = 0.123 ms\n"
               "initial connection time = 3.2 ms\n"
               "tps = 48765.432100 (without initial connection time)\n")
        self.assertAlmostEqual(analyze.parse_pgbench_tps(out), 48765.4321)
        with self.assertRaises(ValueError):
            analyze.parse_pgbench_tps("no tps here\n")


class Boundary(unittest.TestCase):
    # Grid: 1 s buckets, offset 0, window +-5 ms. Times in microseconds.
    S = 1_000_000

    def test_near_far(self):
        S = self.S
        ends = [
            10 * S + 3000,      # ends 3 ms after boundary 10 s: near
            10 * S - 6000,      # ends 6 ms before 10 s, short: far
            10 * S + 500_000,   # middle of the bucket: far
            11 * S - 4000,      # ends 4 ms before 11 s: near
            12 * S + 20_000,    # started 30 ms earlier, spans 12 s: near
            13 * S + 5000,      # starts 4.9 ms after 13 s: near
            13 * S + 5002,      # starts 5.001 ms after 13 s: far
        ]
        lats = [100, 100, 100, 100, 30_000, 100, 1]
        near, far = analyze.split_boundary(ends, lats, interval_us=S, offset_us=0, window_us=5000)
        self.assertEqual(sorted(near), sorted([100, 100, 30_000, 100]))
        self.assertEqual(sorted(far), sorted([100, 100, 1]))

    def test_offset(self):
        S = self.S
        # Grid 2 s with offset 1 s: boundaries at odd seconds.
        near, far = analyze.split_boundary([10 * S + 1000, 11 * S + 1000], [10, 20],
                                           interval_us=2 * S, offset_us=S, window_us=5000)
        self.assertEqual((near, far), ([20], [10]))

    def test_boundaries_crossed(self):
        S = self.S
        # Span [10 s, 13 s + 2 us]: boundaries strictly after the first start.
        self.assertEqual(analyze.boundaries_crossed([10 * S + 1, 13 * S + 2], [1, 1],
                                                    interval_us=S, offset_us=0), 3)
        self.assertEqual(analyze.boundaries_crossed([], [], interval_us=S, offset_us=0), 0)


class Report(unittest.TestCase):
    def test_median_spread(self):
        self.assertEqual(analyze.median([3, 1, 2]), 2)
        self.assertEqual(analyze.median([4, 1, 2, 3]), 2.5)
        self.assertAlmostEqual(analyze.spread_pct([90, 100, 110]), 20.0)
        self.assertEqual(analyze.spread_pct([100]), 0.0)

    def test_run_sh_forwards_documented_env(self):
        # Every BENCH_* variable documented in inside.sh's header must be
        # passed into the container by run.sh (-e BENCH_X or -e BENCH_X=...).
        import re
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "inside.sh")) as f:
            header = []
            for line in f:
                if line.startswith("set -"):
                    break
                header.append(line)
        documented = set(re.findall(r"\bBENCH_[A-Z_]+", "".join(header)))
        self.assertGreater(len(documented), 10)
        with open(os.path.join(here, "run.sh")) as f:
            forwarded = set(re.findall(r"-e (BENCH_[A-Z_]+)", f.read()))
        self.assertEqual(sorted(documented - forwarded), [])

    def test_analyze_run_end_to_end(self):
        S = 1_000_000
        with tempfile.TemporaryDirectory() as d:
            # Two thread log files; one slow txn straddling a boundary.
            with open(os.path.join(d, "log.123"), "w") as f:
                f.write("0 0 200 0 100 400000\n")
                f.write("0 1 9000 0 101 2000\n")
            with open(os.path.join(d, "log.123.1"), "w") as f:
                f.write("1 0 300 0 101 500000\n")
            with open(os.path.join(d, "pgbench.out"), "w") as f:
                f.write("tps = 3.000000 (without initial connection time)\n")
            res = analyze.analyze_run(
                logs=[os.path.join(d, "log.123"), os.path.join(d, "log.123.1")],
                pgbench_out=os.path.join(d, "pgbench.out"),
                meta={"config": "c", "scenario": "s", "baseline": "pgss", "block": 1,
                      "server_cpu_ticks": 150, "clk_tck": 100, "duration_s": 2.0,
                      "stmts_per_txn": 3},
                interval_us=S, offset_us=0, window_us=5000)
            json.dumps(res)  # serializable
            self.assertEqual(res["tps"], 3.0)
            self.assertEqual(res["all"]["n"], 3)
            self.assertAlmostEqual(res["all"]["max_ms"], 9.0)
            self.assertAlmostEqual(res["all"]["p95_ms"], 9.0)
            self.assertEqual(res["near"]["n"], 1)
            self.assertAlmostEqual(res["near"]["max_ms"], 9.0)
            self.assertEqual(res["far"]["n"], 2)
            self.assertAlmostEqual(res["far"]["max_ms"], 0.3)
            self.assertEqual(res["boundaries_crossed"], 1)
            self.assertEqual(res["config"], "c")
            # 150 ticks at 100 Hz = 1.5 s of server CPU over 3 transactions,
            # 3 statements each; 1.5 s over a 2 s run = 0.75 cores.
            self.assertAlmostEqual(res["cpu_us_per_txn"], 500000.0)
            self.assertAlmostEqual(res["cpu_us_per_stmt"], 500000.0 / 3)
            self.assertAlmostEqual(res["server_cores"], 0.75)


class Stats(unittest.TestCase):
    def test_t_quantile_known(self):
        # Two-sided 95% critical values of Student's t (standard tables).
        for df, q in [(1, 12.7062), (2, 4.3027), (4, 2.7764), (7, 2.3646), (9, 2.2622),
                      (29, 2.0452), (1000, 1.9623)]:
            self.assertAlmostEqual(analyze.t_quantile(0.975, df), q, places=3, msg="df=%d" % df)
        self.assertAlmostEqual(analyze.t_quantile(0.95, 10), 1.8125, places=3)
        self.assertAlmostEqual(analyze.t_quantile(0.995, 5), 4.0321, places=3)

    def test_mean_ci_known(self):
        ci = analyze.mean_ci([1, 2, 3, 4, 5])
        self.assertEqual(ci["n"], 5)
        self.assertAlmostEqual(ci["mean"], 3.0)
        # sd = sqrt(2.5), se = sqrt(0.5), t(0.975, 4) = 2.776445
        self.assertAlmostEqual(ci["lo"], 3 - 2.776445 * 0.5 ** 0.5, places=4)
        self.assertAlmostEqual(ci["hi"], 3 + 2.776445 * 0.5 ** 0.5, places=4)
        ci = analyze.mean_ci([2.0, 2.0, 2.0])
        self.assertEqual((ci["lo"], ci["hi"]), (2.0, 2.0))

    def test_mean_ci_too_few(self):
        ci = analyze.mean_ci([4.0])
        self.assertEqual(ci["mean"], 4.0)
        self.assertIsNone(ci["lo"])
        self.assertIsNone(ci["hi"])
        self.assertIsNone(analyze.mean_ci([]))

    def test_ratio_ci_known(self):
        # Ratios e^0.1, e^0.2, e^0.3: log mean 0.2, sd 0.1, se 0.1/sqrt(3),
        # t(0.975, 2) = 4.302653. Reported as percent change.
        import math
        pairs = [(100 * math.exp(0.1), 100), (50 * math.exp(0.2), 50), (10 * math.exp(0.3), 10)]
        ci = analyze.ratio_ci(pairs)
        hw = 4.302653 * 0.1 / math.sqrt(3)
        self.assertEqual(ci["n"], 3)
        self.assertAlmostEqual(ci["mean"], (math.exp(0.2) - 1) * 100, places=4)
        self.assertAlmostEqual(ci["lo"], (math.exp(0.2 - hw) - 1) * 100, places=3)
        self.assertAlmostEqual(ci["hi"], (math.exp(0.2 + hw) - 1) * 100, places=3)
        # A constant ratio has a zero-width interval; pairs with a missing or
        # non-positive value are left out.
        ci = analyze.ratio_ci([(90, 100), (45, 50), (None, 3), (1, 0)])
        self.assertEqual(ci["n"], 2)
        self.assertAlmostEqual(ci["lo"], -10.0)
        self.assertAlmostEqual(ci["hi"], -10.0)

    def test_verdict(self):
        V = analyze.verdict
        # Higher is worse (CPU, latency):
        self.assertTrue(V({"n": 8, "mean": 3.0, "lo": 1.0, "hi": 5.0}).startswith("costlier"))
        self.assertTrue(V({"n": 8, "mean": -3.0, "lo": -5.0, "hi": -1.0}).startswith("cheaper"))
        v = V({"n": 8, "mean": 1.0, "lo": -2.0, "hi": 4.0})
        self.assertTrue(v.startswith("not resolved"))
        self.assertIn("+4.0", v)            # the bound the data does give
        self.assertTrue(V({"n": 1, "mean": 1.0, "lo": None, "hi": None}).startswith("no CI"))
        self.assertTrue(V(None).startswith("no CI"))
        # Higher is better (TPS): a significant drop is the cost.
        self.assertTrue(V({"n": 8, "mean": -3.0, "lo": -5.0, "hi": -1.0}, higher_is_better=True)
                        .startswith("costlier"))


def _run(scenario, config, baseline, block, tps, cpu=100.0, p99=1.0, stmts=1, **kw):
    r = {"scenario": scenario, "config": config, "baseline": baseline, "block": block,
         "pos": kw.pop("pos", 0), "tps": tps, "cpu_us_per_txn": cpu, "cpu_us_per_stmt": cpu / stmts,
         "server_cores": 1.0, "stmts_per_txn": stmts, "workload": "w", "protocol": "simple",
         "clients": 5, "foreign_cores": kw.pop("foreign", 0.1), "attempts": kw.pop("attempts", 1),
         "checks": kw.pop("checks", {"txns": 10}),
         "all": {"n": 10, "avg_ms": 1.0, "p50_ms": p99 / 2, "p95_ms": p99 * 0.9, "p99_ms": p99,
                 "max_ms": 5.0}}
    r.update(kw)
    return r


class Compare(unittest.TestCase):
    def test_paired_by_block(self):
        runs = []
        cpu_diffs = [2.0, 3.0, 4.0, 3.0]
        for b in range(1, 5):
            base = 1000.0 * (1 + 0.1 * b)        # drift between blocks cancels in pairs
            runs.append(_run("s", "pgss", "pgss", b, base, cpu=100.0 + b, p99=1.0 * b))
            runs.append(_run("s", "ext", "pgss", b, base * 0.9, cpu=100.0 + b + cpu_diffs[b - 1],
                             p99=1.2 * b))
        # Same config name in another scenario: kept apart.
        runs.append(_run("t", "pgss", "pgss", 1, 50.0))
        runs.append(_run("t", "ext", "pgss", 1, 60.0))
        rows = {(r["scenario"], r["config"]): r for r in analyze.compare(runs)}
        self.assertNotIn(("s", "pgss"), rows)       # baselines are not compared to themselves
        ext = rows[("s", "ext")]
        self.assertEqual(ext["n"], 4)
        self.assertAlmostEqual(ext["tps"]["mean"], -10.0)
        self.assertAlmostEqual(ext["tps"]["lo"], -10.0)
        self.assertAlmostEqual(ext["p99"]["mean"], 20.0)
        ci = analyze.mean_ci(cpu_diffs)
        self.assertAlmostEqual(ext["cpu_diff_us"]["mean"], 3.0)
        self.assertAlmostEqual(ext["cpu_diff_us"]["lo"], ci["lo"])
        self.assertAlmostEqual(ext["cpu_stmt_diff_us"]["hi"], ci["hi"])
        self.assertAlmostEqual(ext["base_tps"], analyze.median([1100.0, 1200.0, 1300.0, 1400.0]))
        self.assertEqual(rows[("t", "ext")]["n"], 1)
        self.assertIsNone(rows[("t", "ext")]["tps"]["lo"])

    def test_missing_baseline_block_dropped(self):
        runs = [_run("s", "pgss", "pgss", 1, 100.0), _run("s", "ext", "pgss", 1, 90.0),
                _run("s", "ext", "pgss", 2, 10.0),           # no baseline in block 2
                _run("s", "pgss", "pgss", 3, 100.0), _run("s", "ext", "pgss", 3, 90.0)]
        ext = analyze.compare(runs)[0]
        self.assertEqual(ext["n"], 2)
        self.assertEqual(ext["runs"], 3)
        self.assertAlmostEqual(ext["tps"]["mean"], -10.0)

    def test_cpu_per_statement(self):
        runs = []
        for b, d in enumerate([7.0, 14.0, 21.0], 1):
            runs += [_run("rw", "pgss", "pgss", b, 100.0, cpu=700.0, stmts=7),
                     _run("rw", "ext", "pgss", b, 100.0, cpu=700.0 + d, stmts=7)]
        ext = analyze.compare(runs)[0]
        self.assertAlmostEqual(ext["cpu_diff_us"]["mean"], 14.0)
        self.assertAlmostEqual(ext["cpu_stmt_diff_us"]["mean"], 2.0)
        self.assertAlmostEqual(ext["cpu_rel"]["mean"], analyze.ratio_ci(
            [(707.0, 700.0), (714.0, 700.0), (721.0, 700.0)])["mean"])


class Campaign(unittest.TestCase):
    def _write(self, d, notes="Notes for this campaign."):
        runs = []
        for b in range(1, 4):
            for sc in ("ro-simple-c5", "rw-simple-c5"):
                runs.append(_run(sc, "pgss", "pgss", b, 1000.0 + b, cpu=50.0))
                runs.append(_run(sc, "ext", "pgss", b, 950.0 + b, cpu=52.0 + b * 0.1,
                                 checks={"txns": 10, "ext_calls": 10}))
        with open(os.path.join(d, "runs.jsonl"), "w") as f:
            for r in runs:
                f.write(json.dumps(r) + "\n")
        with open(os.path.join(d, "campaign.json"), "w") as f:
            json.dump({"commit": "abc1234", "dirty": False, "date": "2026-10-08",
                       "host": "Apple M1 Max, 10 CPUs", "pg_version": "PostgreSQL 18.0",
                       "blocks": 3, "duration_s": 12, "warmup_s": 3, "seed": 7,
                       "wall_time_s": 3600, "quiet_check": "docker ps: none",
                       "server_cpus": "0-4", "client_cpus": "5-7", "ncpu": 5,
                       "scenarios": [{"name": "ro-simple-c5", "workload": "ro", "protocol": "simple",
                                      "clients": 5, "configs": [
                                          {"name": "pgss", "baseline": "pgss", "settings": "",
                                           "label": "pg_stat_statements only"},
                                          {"name": "ext", "baseline": "pgss", "settings": "",
                                           "label": "tagged, defaults"}]},
                                     {"name": "rw-simple-c5", "workload": "rw", "protocol": "simple",
                                      "clients": 5, "configs": [
                                          {"name": "pgss", "baseline": "pgss", "settings": ""},
                                          {"name": "ext", "baseline": "pgss", "settings": ""}]}]}, f)
        with open(os.path.join(d, "env-host.txt"), "w") as f:
            f.write("host: test\n")
        if notes is not None:
            with open(os.path.join(d, "NOTES.md"), "w") as f:
                f.write(notes + "\n")

    def test_render_campaign(self):
        with tempfile.TemporaryDirectory() as d:
            self._write(d)
            md = analyze.render_campaign(d)
            self.assertIn("abc1234", md)
            self.assertIn("95% CI", md)
            self.assertIn("ro-simple-c5", md)
            self.assertIn("rw-simple-c5", md)
            self.assertIn("tagged, defaults", md)
            self.assertIn("Notes for this campaign.", md)
            self.assertIn("docker ps: none", md)
            self.assertIn("costlier", md)        # +2..+2.3 us of CPU, every block
            # p50/p95/p99 and CPU per statement columns
            for col in ("p50", "p95", "p99", "CPU", "TPS"):
                self.assertIn(col, md)

    def test_build_doc(self):
        with tempfile.TemporaryDirectory() as root:
            for name in ("2026-10-01-old-host", "2026-10-08-new-host"):
                os.mkdir(os.path.join(root, name))
                self._write(os.path.join(root, name), notes="notes of " + name)
            tmpl = "# Benchmarks\n\nintro\n\n{{campaigns}}\n\nstatic tail\n"
            doc = analyze.build_doc(tmpl, root)
            self.assertTrue(doc.startswith("<!-- Generated by bench/analyze.py"))
            self.assertIn("intro", doc)
            self.assertIn("static tail", doc)
            # newest campaign first
            self.assertLess(doc.index("notes of 2026-10-08-new-host"), doc.index("notes of 2026-10-01-old-host"))
            self.assertNotIn("{{campaigns}}", doc)
            with self.assertRaises(ValueError):
                analyze.build_doc("no marker", root)

    def test_collect_compacts_runs(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, r in enumerate([_run("s", "pgss", "pgss", 1, 1.0), _run("s", "ext", "pgss", 1, 1.0)]):
                paths.append(os.path.join(d, "%d.json" % i))
                with open(paths[-1], "w") as f:
                    json.dump(r, f, indent=1)
            out = os.path.join(d, "runs.jsonl")
            analyze.collect(paths, out)
            with open(out) as f:
                lines = f.read().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(json.loads(lines[1])["config"], "ext")


class FreshCheck(unittest.TestCase):
    def test_stale_paths(self):
        import subprocess
        here = os.path.dirname(os.path.abspath(__file__))
        try:
            head = subprocess.check_output(["git", "-C", here, "rev-parse", "HEAD"], text=True,
                                           stderr=subprocess.DEVNULL).strip()
            old = subprocess.check_output(["git", "-C", here, "log", "-1", "--format=%H", "--", "src"],
                                          text=True, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("no git repository")
        root = os.path.dirname(here)
        self.assertEqual(analyze.stale_paths(head, root), [])
        # The parent of the last commit that touched src/ differs in src/.
        self.assertTrue(any(p.startswith("src/") for p in analyze.stale_paths(old + "~1", root)))


class Plan(unittest.TestCase):
    def test_scenarios_cover_the_matrix(self):
        scs = scenarios.scenarios(ncpu=5, duration=12)
        by = {s["name"]: s for s in scs}
        for wl in ("ro", "rw"):
            for proto in ("simple", "prepared"):
                for c in (1, 5, 20):
                    name = "%s-%s-c%d" % (wl, proto, c)
                    self.assertIn(name, by)
                    self.assertEqual(by[name]["clients"], c)
                    self.assertEqual(by[name]["protocol"], proto)
                    cfgs = {x["name"] for x in by[name]["configs"]}
                    self.assertTrue({"pgss", "ext"} <= cfgs, name)
        self.assertFalse(any(s["clients"] == 256 for s in scs))
        high = {s["name"] for s in scenarios.scenarios(ncpu=5, duration=12, high_clients=True)}
        self.assertTrue({"ro-simple-c256", "rw-simple-c256"} <= high)
        cfgs = {(s["name"], c["name"]) for s in scs for c in s["configs"]}
        for want in [("ro-simple-c5", "plain-ext"), ("ro-simple-c5", "ext-regex-normalize"),
                     ("ro-prepared-c5", "plain-ext"), ("ro-prepared-c5", "ext-regex-normalize"),
                     ("rw-simple-c5", "plain-ext"), ("rw-simple-c5", "ext-regex-normalize"),
                     ("ro-simple-c5", "ext-store5k"), ("ro-simple-c5", "ext-reader15s"),
                     ("ro-simple-c5", "ext-reader1s"), ("ro-simple-c5", "ext-1s-buckets"),
                     ("nested-simple-c5", "ext-all-inherit"), ("nested-simple-c5", "ext-all-scan"),
                     ("inlist-simple-c5", "inlist-ext"), ("inlist-simple-c5", "inlist-ext-any"),
                     ("inlist-simple-c5", "inlist0-ext"), ("inlist-simple-c5", "inlist0-ext-any"),
                     ("evict-simple-c5", "ext-max10000")]:
            self.assertIn(want, cfgs)

    def test_baselines(self):
        for s in scenarios.scenarios(ncpu=4, duration=12, high_clients=True):
            names = {c["name"]: c for c in s["configs"]}
            for c in s["configs"]:
                b = names.get(c["baseline"])
                self.assertIsNotNone(b, "%s/%s" % (s["name"], c["name"]))
                self.assertEqual(b["preload"], "pgss")
                self.assertEqual(b["baseline"], b["name"])
                # a config is compared with pgss alone on the same script
                self.assertEqual(b["script"], c["script"], "%s/%s" % (s["name"], c["name"]))
                self.assertIn(c["preload"], ("pgss", "ext"))
                self.assertGreaterEqual(c["stmts_per_txn"], 1)

    def test_plan_interleaved_blocks(self):
        scs = scenarios.scenarios(ncpu=5, duration=12)
        plan = scenarios.make_plan(scs, blocks=4, seed=1)
        allcfg = {(s["name"], c["name"]) for s in scs for c in s["configs"]}
        orders = {}
        for b in range(1, 5):
            rows = [r for r in plan if r["block"] == b]
            self.assertEqual(sorted((r["scenario"], r["config"]) for r in rows), sorted(allcfg))
            # a scenario's runs are contiguous within its block, so each run
            # is close in time to the baseline it is paired with
            seq = [r["scenario"] for r in rows]
            seen = []
            for x in seq:
                if not seen or seen[-1] != x:
                    self.assertNotIn(x, seen)
                    seen.append(x)
            orders[b] = [(r["scenario"], r["config"]) for r in rows]
        self.assertGreater(len({tuple(o) for o in orders.values()}), 1)   # randomized per block
        self.assertEqual(plan, scenarios.make_plan(scs, blocks=4, seed=1))
        self.assertNotEqual(plan, scenarios.make_plan(scs, blocks=4, seed=2))
        self.assertEqual([r["seq"] for r in plan], list(range(1, len(plan) + 1)))

    def test_plan_only(self):
        scs = scenarios.scenarios(ncpu=5, duration=12)
        plan = scenarios.make_plan(scs, blocks=2, seed=1, only=r"^inlist-")
        self.assertTrue(plan)
        self.assertTrue(all(r["scenario"].startswith("inlist-") for r in plan))

    def test_plan_tsv_roundtrip(self):
        scs = scenarios.scenarios(ncpu=5, duration=12)
        plan = scenarios.make_plan(scs, blocks=1, seed=3)
        lines = scenarios.plan_tsv(plan).splitlines()
        self.assertEqual(len(lines), len(plan))
        for line, r in zip(lines, plan):
            f = line.split("\t")
            self.assertEqual(len(f), len(scenarios.TSV_FIELDS))
            self.assertEqual(f[scenarios.TSV_FIELDS.index("config")], r["config"])
            self.assertNotIn("\n", line)

    def test_exporter_queries(self):
        here = os.path.dirname(os.path.abspath(__file__))
        qs = scenarios.exporter_queries(os.path.join(
            here, "..", "docs", "integrations", "postgres_exporter", "queries.yaml"))
        self.assertEqual(len(qs), 3)
        self.assertIn("pg_stat_statement_context_last_bucket", qs[0])
        self.assertIn("pg_stat_statement_context_counters()", qs[2])
        for q in qs:
            self.assertNotIn("metrics:", q)
            self.assertNotIn("usage:", q)
            self.assertTrue(q.lstrip().startswith(("WITH", "SELECT")))
        self.assertEqual(scenarios.exporter_queries_sql(qs).count(";\n"), 3)


if __name__ == "__main__":
    unittest.main()
