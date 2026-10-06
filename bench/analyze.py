#!/usr/bin/env python3
"""Analysis for the pgbench benchmarks (bench/run.sh). Standard library only.

  analyze.py --self-test
      run bench/test_analyze.py
  analyze.py run --meta JSON --pgbench-out FILE --out FILE [--interval-s 1]
                 [--offset-s 946684800] [--window-ms 5] LOG...
      summarize one pgbench run from its per-transaction logs (--log, no
      aggregation): TPS (from pgbench's output), avg/p50/p99/max latency, and
      the same for transactions within +-window of a bucket boundary vs the
      rest. The boundary grid is offset + k * interval (Unix time); the
      extension's epoch is a multiple of bucket_interval since 2000-01-01.
  analyze.py report --out-md FILE --out-json FILE RUN_JSON...
      aggregate runs per configuration (median and spread over runs). Deltas
      vs the configuration's baseline (pgss alone on the same workload) are
      paired by round (run / baseline run of the same round), then rounds
      2k-1 (forward) and 2k (reversed) are combined into a balanced pair by
      the geometric mean of their ratios, which cancels drift that is smooth
      in time; the median over pairs is reported. Unpaired rounds (odd round
      count) are dropped; with no pair (one round) the per-round ratio is
      reported and marked unpaired.

Percentiles use the nearest-rank method: pN is the ceil(N/100 * n)-th
smallest value. Latencies in pgbench logs are in microseconds.
"""
import argparse
import json
import math
import os
import re
import sys

EPOCH_2000 = 946684800


def percentile(values, p):
    if not values:
        return None
    xs = sorted(values)
    k = max(1, math.ceil(p / 100.0 * len(xs)))
    return xs[min(k, len(xs)) - 1]


def summarize(lats_us):
    n = len(lats_us)
    if n == 0:
        return {"n": 0, "avg_ms": None, "p50_ms": None, "p99_ms": None, "max_ms": None}
    xs = sorted(lats_us)

    def rank(p):
        return xs[max(1, math.ceil(p / 100.0 * n)) - 1] / 1000.0

    return {"n": n, "avg_ms": sum(xs) / n / 1000.0, "p50_ms": rank(50),
            "p99_ms": rank(99), "max_ms": xs[-1] / 1000.0}


def parse_log_lines(lines):
    """Returns (end times in us since the Unix epoch, latencies in us)."""
    ends, lats = [], []
    for line in lines:
        f = line.split()
        if not f:
            continue
        if len(f) < 6 or not f[2].isdigit():
            raise ValueError("unexpected pgbench log line (failed/skipped transaction?): %r" % line)
        lats.append(int(f[2]))
        ends.append(int(f[4]) * 1_000_000 + int(f[5]))
    return ends, lats


def parse_pgbench_tps(text):
    m = re.search(r"^tps = ([0-9.]+) \(without initial connection time\)", text, re.M)
    if not m:
        raise ValueError("no 'tps = ... (without initial connection time)' line in pgbench output")
    return float(m.group(1))


def split_boundary(ends, lats, interval_us, offset_us, window_us):
    """Splits latencies into transactions whose [start, end] lies within
    window_us of a boundary (near) and the others (far)."""
    near, far = [], []
    for end, lat in zip(ends, lats):
        lo = end - lat - window_us - offset_us
        b = offset_us + (-(-lo // interval_us)) * interval_us  # first boundary >= lo
        (near if b <= end + window_us else far).append(lat)
    return near, far


def boundaries_crossed(ends, lats, interval_us, offset_us):
    """Boundaries b with first start < b <= last end."""
    if not ends:
        return 0
    first = min(e - l for e, l in zip(ends, lats)) - offset_us
    last = max(ends) - offset_us
    return last // interval_us - first // interval_us


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def spread_pct(xs):
    """(max - min) / median, in percent."""
    m = median(xs)
    return 0.0 if len(xs) < 2 or not m else (max(xs) - min(xs)) / m * 100.0


def analyze_run(logs, pgbench_out, meta, interval_us, offset_us, window_us):
    ends, lats = [], []
    for path in logs:
        with open(path) as f:
            e, l = parse_log_lines(f)
        ends.extend(e)
        lats.extend(l)
    with open(pgbench_out) as f:
        tps = parse_pgbench_tps(f.read())
    near, far = split_boundary(ends, lats, interval_us, offset_us, window_us)
    res = dict(meta)
    res.update({
        "tps": tps,
        "all": summarize(lats),
        "near": summarize(near),
        "far": summarize(far),
        "boundaries_crossed": boundaries_crossed(ends, lats, interval_us, offset_us),
        "grid": {"interval_us": interval_us, "offset_us": offset_us, "window_us": window_us},
    })
    return res


def _rel(x, base):
    if x is None or base is None or base == 0:
        return None
    return (x / base - 1.0) * 100.0


def aggregate(runs):
    """One row per config (first-appearance order): medians over runs,
    spread, worst max, and deltas vs the baseline config's medians."""
    order, by = [], {}
    for r in runs:
        if r["config"] not in by:
            order.append(r["config"])
            by[r["config"]] = []
        by[r["config"]].append(r)

    def med(rs, f):
        vals = [f(r) for r in rs if f(r) is not None]
        return median(vals) if vals else None

    rows = {}
    for c in order:
        rs = by[c]
        row = {k: rs[0].get(k) for k in ("config", "workload", "baseline", "label")}
        # the checks shown are those of the greatest round (the last one run)
        row["checks"] = max(rs, key=lambda r: r.get("run", 0)).get("checks")
        row.update({
            "runs": len(rs),
            "tps": med(rs, lambda r: r["tps"]),
            "tps_spread_pct": spread_pct([r["tps"] for r in rs]),
            "avg_ms": med(rs, lambda r: r["all"]["avg_ms"]),
            "p50_ms": med(rs, lambda r: r["all"]["p50_ms"]),
            "p99_ms": med(rs, lambda r: r["all"]["p99_ms"]),
            "max_ms": med(rs, lambda r: r["all"]["max_ms"]),
            "max_worst_ms": max(r["all"]["max_ms"] for r in rs),
            "foreign_cores_max": max((r["foreign_cores"] for r in rs if r.get("foreign_cores") is not None),
                                     default=None),
            "retries": sum(r.get("attempts", 1) - 1 for r in rs),
        })
        if "near" in rs[0]:
            for part in ("near", "far"):
                row[part] = {
                    "n": med(rs, lambda r: r[part]["n"]),
                    "p50_ms": med(rs, lambda r: r[part]["p50_ms"]),
                    "p99_ms": med(rs, lambda r: r[part]["p99_ms"]),
                    "max_ms": med(rs, lambda r: r[part]["max_ms"]),
                    "max_worst_ms": max((r[part]["max_ms"] for r in rs if r[part]["max_ms"] is not None),
                                        default=None),
                }
            row["boundaries_crossed"] = med(rs, lambda r: r["boundaries_crossed"])
            row["window_ms"] = rs[0]["grid"]["window_us"] / 1000.0
            row["grid_s"] = rs[0]["grid"]["interval_us"] / 1e6
        rows[c] = row
    for c in order:
        row, base = rows[c], rows.get(rows[c]["baseline"])
        mine = {r["run"]: r for r in by[c]}
        base_runs = {r["run"]: r for r in by.get(row["baseline"], [])}
        common = sorted(set(mine) & set(base_runs))
        # Rounds alternate direction (odd forward, even reversed), so round
        # 2k-1 and 2k form a balanced pair: the geometric mean of their two
        # same-round ratios cancels a drift that is smooth in time. Rounds
        # without a partner (odd count, missing baseline) are dropped; with
        # no pair at all (--quick) the unpaired same-round ratios are used.
        pairs = [(k, k + 1) for k in common if k % 2 == 1 and k + 1 in common]
        paired = bool(pairs)
        row["rel_paired"] = paired
        row["rel_pairs"] = len(pairs)
        row["rel_rounds_dropped"] = len(mine) - 2 * len(pairs) if paired else 0
        for k, get in (("tps_rel", lambda r: r["tps"]), ("avg_rel", lambda r: r["all"]["avg_ms"]),
                       ("p99_rel", lambda r: r["all"]["p99_ms"]), ("max_rel", lambda r: r["all"]["max_ms"])):
            def ratio(n):
                x, y = get(mine[n]), get(base_runs[n])
                return None if x is None or y is None or y == 0 or x <= 0 else x / y
            if paired:
                vals = [(ratio(a), ratio(b)) for a, b in pairs]
                rel = [(math.sqrt(x * y) - 1.0) * 100.0 for x, y in vals if x is not None and y is not None]
            else:
                rel = [(q - 1.0) * 100.0 for q in map(ratio, common) if q is not None]
            row[k + "_pct"] = median(rel) if base and rel else None
            row[k + "_min_pct"] = min(rel) if base and rel else None
            row[k + "_max_pct"] = max(rel) if base and rel else None
    return [rows[c] for c in order]


def _f(x, fmt="%.3f"):
    return "–" if x is None else fmt % x


def _pct(x):
    return "–" if x is None else "%+.1f%%" % x


def render_markdown(rows):
    unpaired = any(not r.get("rel_paired", True) for r in rows if r.get("tps_rel_pct") is not None)
    dropped = max((r.get("rel_rounds_dropped", 0) for r in rows), default=0)
    note = ("Δ columns: rounds alternate direction (odd forward, even reversed); each run is compared with "
            "the baseline's run of the same round, and rounds 2k-1 and 2k are combined into one balanced pair "
            "(geometric mean of the two ratios), which cancels drift that is smooth in time. Shown: the median "
            "over pairs; ΔTPS also shows [min, max] over pairs.")
    if dropped:
        note += " Rounds without a partner (odd round count) are left out of the Δ columns (up to %d per configuration)." % dropped
    if unpaired:
        note += " † unpaired: fewer than two rounds, so Δ is the single-round (or per-round median) ratio, not drift-corrected."
    out = [note, "",
           "| Configuration | Runs | TPS (median) | TPS spread | ΔTPS vs pgss | avg ms | Δavg | p99 ms | Δp99 | max ms (median / worst) |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        out.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s / %s |" % (
            r.get("label") or r["config"], r["runs"], _f(r["tps"], "%.0f"),
            _f(r["tps_spread_pct"], "%.1f%%"),
            _pct(r["tps_rel_pct"]) + ("" if r["tps_rel_pct"] is None else
                                      " †" if not r.get("rel_paired", True) else
                                      "" if r.get("rel_pairs", 0) < 2 else
                                      " [%s, %s]" % (_pct(r["tps_rel_min_pct"]), _pct(r["tps_rel_max_pct"]))),
            _f(r["avg_ms"]), _pct(r["avg_rel_pct"]), _f(r["p99_ms"]), _pct(r["p99_rel_pct"]),
            _f(r["max_ms"], "%.2f"), _f(r["max_worst_ms"], "%.2f")))
    out += ["", "Boundary windows (transactions overlapping ±window of a boundary on the grid vs the rest; medians over runs):", "",
            "| Configuration | grid | window | boundaries/run | near n | near p99 ms | near max ms (median / worst) | far p99 ms | far max ms (median / worst) |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        if "near" not in r:
            continue
        out.append("| %s | %gs | ±%g ms | %s | %s | %s | %s / %s | %s | %s / %s |" % (
            r.get("label") or r["config"], r["grid_s"], r["window_ms"], _f(r["boundaries_crossed"], "%.0f"),
            _f(r["near"]["n"], "%.0f"), _f(r["near"]["p99_ms"]),
            _f(r["near"]["max_ms"], "%.2f"), _f(r["near"]["max_worst_ms"], "%.2f"),
            _f(r["far"]["p99_ms"]), _f(r["far"]["max_ms"], "%.2f"), _f(r["far"]["max_worst_ms"], "%.2f")))
    out += ["", "Validation (checks of the run in the greatest round of each configuration, i.e. the last one run; every run was checked; foreign CPU = cores used by other containers during a run, max over runs; retries = runs repeated because of it):", "",
            "| Configuration | checks | foreign CPU (max) | retries |", "|---|---|---:|---:|"]
    for r in rows:
        out.append("| %s | %s | %s | %d |" % (r.get("label") or r["config"],
                                              ", ".join("%s=%s" % kv for kv in (r.get("checks") or {}).items()) or "–",
                                              _f(r.get("foreign_cores_max"), "%.2f"), r.get("retries", 0)))
    return "\n".join(out) + "\n"


def main(argv):
    if argv[:1] == ["--self-test"]:
        import unittest
        here = os.path.dirname(os.path.abspath(__file__))
        suite = unittest.defaultTestLoader.discover(here, pattern="test_analyze.py")
        ok = unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful()
        return 0 if ok else 1
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--meta", required=True, help="JSON object merged into the result")
    r.add_argument("--pgbench-out", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--interval-s", type=float, default=1.0)
    r.add_argument("--offset-s", type=int, default=EPOCH_2000)
    r.add_argument("--window-ms", type=float, default=5.0)
    r.add_argument("logs", nargs="+")
    p = sub.add_parser("report")
    p.add_argument("--out-md", required=True)
    p.add_argument("--out-json", required=True)
    p.add_argument("runs", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "run":
        interval_us = int(round(a.interval_s * 1e6))
        res = analyze_run(a.logs, a.pgbench_out, json.loads(a.meta), interval_us,
                          (a.offset_s * 1_000_000) % interval_us, int(round(a.window_ms * 1000)))
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
        s = res["all"]
        print("  %-28s tps=%.0f avg=%.3fms p99=%.3fms max=%.2fms near(n=%d p99=%s max=%s)" % (
            res["config"], res["tps"], s["avg_ms"], s["p99_ms"], s["max_ms"], res["near"]["n"],
            _f(res["near"]["p99_ms"]), _f(res["near"]["max_ms"], "%.2f")))
        return 0
    runs = []
    for path in a.runs:
        with open(path) as f:
            runs.append(json.load(f))
    runs.sort(key=lambda r: (r.get("order", 0), r["run"]))
    rows = aggregate(runs)
    with open(a.out_json, "w") as f:
        json.dump(rows, f, indent=1)
    with open(a.out_md, "w") as f:
        f.write(render_markdown(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
