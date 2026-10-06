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

    def test_aggregate_relative_to_baseline(self):
        runs = []
        for i, (bt, et) in enumerate([(1000, 900), (1100, 990), (1050, 945)]):
            runs.append({"config": "w/pgss", "workload": "w", "baseline": "w/pgss", "run": i,
                         "tps": bt, "all": {"n": 10, "avg_ms": 1.0, "p50_ms": 1.0, "p99_ms": 2.0, "max_ms": 5.0}})
            runs.append({"config": "w/ext", "workload": "w", "baseline": "w/pgss", "run": i,
                         "tps": et, "all": {"n": 10, "avg_ms": 1.1, "p50_ms": 1.0, "p99_ms": 2.2, "max_ms": 6.0 + i}})
        agg = {a["config"]: a for a in analyze.aggregate(runs)}
        self.assertEqual(agg["w/ext"]["runs"], 3)
        self.assertEqual(agg["w/ext"]["tps"], 945)
        self.assertAlmostEqual(agg["w/ext"]["tps_rel_pct"], -10.0)
        self.assertAlmostEqual(agg["w/ext"]["avg_rel_pct"], 10.0)
        self.assertAlmostEqual(agg["w/ext"]["p99_rel_pct"], 10.0)
        self.assertEqual(agg["w/ext"]["max_ms"], 7.0)        # median of 6, 7, 8
        self.assertEqual(agg["w/ext"]["max_worst_ms"], 8.0)
        self.assertAlmostEqual(agg["w/ext"]["tps_spread_pct"], (990 - 900) / 945 * 100)
        self.assertAlmostEqual(agg["w/pgss"]["tps_rel_pct"], 0.0)
        # Order of first appearance is kept.
        self.assertEqual([a["config"] for a in analyze.aggregate(runs)], ["w/pgss", "w/ext"])

    @staticmethod
    def _run(config, baseline, rnd, tps, p99=2.0, checks=None):
        return {"config": config, "workload": "w", "baseline": baseline, "run": rnd, "tps": tps,
                "checks": checks,
                "all": {"n": 10, "avg_ms": 1000.0 / tps, "p50_ms": 1, "p99_ms": p99, "max_ms": 5.0}}

    def test_aggregate_paired_by_round(self):
        # Rounds 2k-1 (forward) and 2k (reverse) form a pair; the pair's
        # ratio is the geometric mean of its two per-round ratios, then the
        # median (and min/max) over pairs. Round 5 has no partner: dropped.
        R = self._run
        ratios = {1: 0.9, 2: 1.0, 3: 0.8, 4: 0.8, 5: 0.5}
        runs = []
        for rnd, q in ratios.items():
            base = 1000.0 * rnd   # rounds drift; only same-round ratios matter
            runs += [R("w/pgss", "w/pgss", rnd, base), R("w/ext", "w/pgss", rnd, base * q, p99=2.0 / q)]
        ext = {a["config"]: a for a in analyze.aggregate(runs)}["w/ext"]
        pair1, pair2 = (0.9 ** 0.5 - 1) * 100, -20.0
        self.assertAlmostEqual(ext["tps_rel_pct"], (pair1 + pair2) / 2)
        self.assertAlmostEqual(ext["tps_rel_min_pct"], pair2)
        self.assertAlmostEqual(ext["tps_rel_max_pct"], pair1)
        self.assertAlmostEqual(ext["p99_rel_pct"], ((1 / 0.9 ** 0.5 - 1) * 100 + 25.0) / 2)
        self.assertEqual(ext["rel_pairs"], 2)
        self.assertEqual(ext["rel_rounds_dropped"], 1)
        self.assertTrue(ext["rel_paired"])
        # A baseline round missing breaks its pair: only pair 1 counts.
        runs = [r for r in runs if not (r["config"] == "w/pgss" and r["run"] == 4)]
        ext = {a["config"]: a for a in analyze.aggregate(runs)}["w/ext"]
        self.assertAlmostEqual(ext["tps_rel_pct"], pair1)
        self.assertEqual(ext["rel_pairs"], 1)
        self.assertEqual(ext["rel_rounds_dropped"], 3)

    def test_aggregate_drift_cancels(self):
        # Reviewer's case: identical performance, a smooth 1% slowdown per
        # position in the round, 20 configs, 5 rounds (odd rounds forward,
        # even rounds reversed). Every delta must be 0, not e.g. -2.97%.
        configs = ["c%02d" % i for i in range(20)]
        runs = []
        for rnd in range(1, 6):
            seq = configs if rnd % 2 == 1 else configs[::-1]
            for pos, c in enumerate(seq):
                tps = 1000.0 * 0.99 ** pos
                runs.append(self._run(c, "c04", rnd, tps, p99=1.0 / tps))
        for row in analyze.aggregate(runs):
            for k in ("tps_rel_pct", "avg_rel_pct", "p99_rel_pct", "tps_rel_min_pct", "tps_rel_max_pct"):
                self.assertAlmostEqual(row[k], 0.0, places=9, msg="%s %s" % (row["config"], k))
            self.assertEqual(row["rel_rounds_dropped"], 1)

    def test_aggregate_single_round_unpaired(self):
        # --quick: one round, no pair. Fall back to that round's ratio and say so.
        runs = [self._run("w/pgss", "w/pgss", 1, 1000.0), self._run("w/ext", "w/pgss", 1, 900.0)]
        rows = analyze.aggregate(runs)
        ext = rows[1]
        self.assertAlmostEqual(ext["tps_rel_pct"], -10.0)
        self.assertFalse(ext["rel_paired"])
        self.assertEqual(ext["rel_pairs"], 0)
        self.assertIn("unpaired", analyze.render_markdown(rows))

    def test_aggregate_checks_from_last_round(self):
        R = self._run
        runs = [R("w/ext", "w/ext", rnd, 1.0, checks={"round": rnd}) for rnd in (1, 3, 2)]
        row = analyze.aggregate(runs)[0]
        self.assertEqual(row["checks"], {"round": 3})
        self.assertIn("greatest round", analyze.render_markdown([row]))

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

    def test_aggregate_interference(self):
        mk = lambda run, fc, att: {"config": "w/ext", "workload": "w", "baseline": "w/ext", "run": run,
                                   "foreign_cores": fc, "attempts": att, "tps": 1,
                                   "all": {"n": 1, "avg_ms": 1, "p50_ms": 1, "p99_ms": 1, "max_ms": 1}}
        row = analyze.aggregate([mk(1, 0.1, 1), mk(2, 0.4, 3), mk(3, None, 1)])[0]
        self.assertAlmostEqual(row["foreign_cores_max"], 0.4)
        self.assertEqual(row["retries"], 2)
        row = analyze.aggregate([mk(1, None, 1)])[0]
        self.assertIsNone(row["foreign_cores_max"])
        self.assertIn("foreign", analyze.render_markdown([row]))

    def test_aggregate_missing_baseline(self):
        runs = [{"config": "w/none", "workload": "w", "baseline": "w/pgss", "run": 0,
                 "tps": 10, "all": {"n": 1, "avg_ms": 1, "p50_ms": 1, "p99_ms": 1, "max_ms": 1}}]
        self.assertIsNone(analyze.aggregate(runs)[0]["tps_rel_pct"])

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
                meta={"config": "c", "workload": "w", "baseline": "w/pgss", "run": 0},
                interval_us=S, offset_us=0, window_us=5000)
            json.dumps(res)  # serializable
            self.assertEqual(res["tps"], 3.0)
            self.assertEqual(res["all"]["n"], 3)
            self.assertAlmostEqual(res["all"]["max_ms"], 9.0)
            self.assertEqual(res["near"]["n"], 1)
            self.assertAlmostEqual(res["near"]["max_ms"], 9.0)
            self.assertEqual(res["far"]["n"], 2)
            self.assertAlmostEqual(res["far"]["max_ms"], 0.3)
            self.assertEqual(res["boundaries_crossed"], 1)
            self.assertEqual(res["config"], "c")


if __name__ == "__main__":
    unittest.main()
