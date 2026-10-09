#!/usr/bin/env python3
"""Analysis for the pgbench benchmarks (bench/run.sh). Standard library only.

  analyze.py --self-test
      run bench/test_analyze.py
  analyze.py run --meta JSON --pgbench-out FILE --out FILE [--interval-s 1]
                 [--offset-s 946684800] [--window-ms 5] [--plan PLAN_JSON --seq N] LOG...
      summarize one pgbench run from its per-transaction logs (--log, no
      aggregation): TPS (from pgbench's output), avg/p50/p95/p99/max latency,
      the same for transactions within +-window of a bucket boundary vs the
      rest, and the server CPU per transaction and per statement (from the
      meta's server_cpu_ticks, clk_tck, duration_s and stmts_per_txn). The
      boundary grid is offset + k * interval (Unix time); the extension's
      epoch is a multiple of bucket_interval since 2000-01-01.
  analyze.py collect --out runs.jsonl RUN_JSON...
      one compact JSON line per run: the raw results kept in bench/results/.
  analyze.py report DIR [--out FILE]
      the Markdown report of one campaign directory (campaign.json,
      runs.jsonl, optional NOTES.md).
  analyze.py doc --results bench/results --template bench/benchmarks.md.in
                 --out docs/benchmarks.md [--check-fresh]
      docs/benchmarks.md: the template with {{campaigns}} replaced by the
      report of every campaign under --results, newest first. --check-fresh
      fails if the newest campaign's commit is not an ancestor of HEAD or if
      the extension's code (src/, sql/, Makefile, the control file) differs
      from that commit.

Statistics. Every configuration is compared with its baseline
(pg_stat_statements alone, same script, same scenario) run in the same
block; blocks are the pairs. For ratios (TPS, CPU, latency percentiles) the
mean of the per-pair log ratios gets a Student t 95% confidence interval,
reported as a percent change (exp(x) - 1). The CPU difference per statement
(microseconds) gets a t interval on the per-pair differences. Pairs with a
missing side are dropped. Percentiles use the nearest-rank method: pN is the
ceil(N/100 * n)-th smallest value. Latencies in pgbench logs are in
microseconds.
"""
import argparse
import json
import math
import os
import re
import subprocess
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
        return {"n": 0, "avg_ms": None, "p50_ms": None, "p95_ms": None, "p99_ms": None, "max_ms": None}
    xs = sorted(lats_us)

    def rank(p):
        return xs[max(1, math.ceil(p / 100.0 * n)) - 1] / 1000.0

    return {"n": n, "avg_ms": sum(xs) / n / 1000.0, "p50_ms": rank(50),
            "p95_ms": rank(95), "p99_ms": rank(99), "max_ms": xs[-1] / 1000.0}


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


def run_meta(measured, plan_row):
    """The metadata of one run: its plan row (scenario, config, block, ...)
    overlaid with what was measured. The row's check specification is kept as
    check_spec; "checks" is the measured result."""
    if not plan_row:
        return dict(measured)
    m = dict(plan_row)
    if "checks" in m:
        m["check_spec"] = m.pop("checks")
    m.update(measured)
    return m


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
    # Server CPU: utime + stime of the postmaster and its children, in clock
    # ticks, over the measured run (bench/inside.sh), per transaction.
    ticks, hz = meta.get("server_cpu_ticks"), meta.get("clk_tck")
    if ticks is not None and hz and lats:
        cpu_us = ticks / float(hz) * 1e6
        res["cpu_us_per_txn"] = cpu_us / len(lats)
        res["cpu_us_per_stmt"] = res["cpu_us_per_txn"] / meta.get("stmts_per_txn", 1)
        if meta.get("duration_s"):
            res["server_cores"] = cpu_us / 1e6 / meta["duration_s"]
    return res


def spread_pct_none(xs):
    xs = [x for x in xs if x is not None]
    return spread_pct(xs) if xs else None


# --- statistics -------------------------------------------------------------

def _betacf(a, b, x):
    """Continued fraction of the incomplete beta function (modified Lentz)."""
    tiny, qab, qap, qam = 1e-300, a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return h


def _betainc(a, b, x):
    """Regularized incomplete beta I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbt = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1.0 - x)
    if x < (a + 1.0) / (a + b + 2.0):
        return math.exp(lbt) * _betacf(a, b, x) / a
    return 1.0 - math.exp(lbt) * _betacf(b, a, 1.0 - x) / b


def t_cdf(t, df):
    p = 0.5 * _betainc(df / 2.0, 0.5, df / (df + t * t))
    return 1.0 - p if t >= 0 else p


def t_quantile(p, df):
    """Quantile of Student's t with df degrees of freedom (bisection)."""
    if not 0.0 < p < 1.0 or df < 1:
        raise ValueError("t_quantile(%r, %r)" % (p, df))
    if p < 0.5:
        return -t_quantile(1.0 - p, df)
    lo, hi = 0.0, 1.0
    while t_cdf(hi, df) < p:
        hi *= 2.0
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if t_cdf(mid, df) < p:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def mean_ci(xs, conf=0.95):
    """Mean and two-sided t confidence interval; lo/hi are None below n = 2."""
    xs = [float(x) for x in xs if x is not None]
    n = len(xs)
    if n == 0:
        return None
    m = sum(xs) / n
    if n < 2:
        return {"n": n, "mean": m, "lo": None, "hi": None, "sd": None}
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    hw = t_quantile(0.5 + conf / 2.0, n - 1) * sd / math.sqrt(n)
    return {"n": n, "mean": m, "lo": m - hw, "hi": m + hw, "sd": sd}


def ratio_ci(pairs, conf=0.95):
    """Percent change of x over base: t interval of the mean log ratio,
    transformed back (a geometric mean of the ratios)."""
    logs = [math.log(x / b) for x, b in pairs if x is not None and b is not None and x > 0 and b > 0]
    ci = mean_ci(logs, conf)
    if ci is None:
        return None
    pct = lambda v: None if v is None else (math.exp(v) - 1.0) * 100.0
    return {"n": ci["n"], "mean": pct(ci["mean"]), "lo": pct(ci["lo"]), "hi": pct(ci["hi"])}


def verdict(ci, higher_is_better=False, unit="%", fmt="%+.1f"):
    """What a confidence interval of a change says, in words."""
    if not ci or ci.get("lo") is None:
        return "no CI (fewer than 2 pairs)"
    lo, hi = ci["lo"], ci["hi"]
    worse_lo, worse_hi = (-hi, -lo) if higher_is_better else (lo, hi)
    f = lambda v: (fmt % v) + unit
    if worse_lo > 0:
        return "costlier: %s to %s" % (f(lo), f(hi))
    if worse_hi < 0:
        return "cheaper: %s to %s" % (f(lo), f(hi))
    return "not resolved: between %s and %s" % (f(lo), f(hi))


def _check_unique(runs):
    """Runs are keyed by (scenario, config, block); a duplicate would silently
    replace another run's data."""
    seen = set()
    for r in runs:
        k = (r["scenario"], r["config"], r["block"])
        if k in seen:
            raise ValueError("duplicate run for scenario %s, config %s, block %s" % k)
        seen.add(k)


def compare(runs, conf=0.95):
    """One row per (scenario, configuration) that is not its own baseline,
    paired with the baseline's run of the same block; intervals at level conf."""
    _check_unique(runs)
    order, by = [], {}
    for r in runs:
        k = (r["scenario"], r["config"])
        if k not in by:
            order.append(k)
            by[k] = {}
        by[k][r["block"]] = r
    rows = []
    for k in order:
        sc, cfg = k
        mine = by[k]
        first = next(iter(mine.values()))
        if first["baseline"] == cfg:
            continue
        base = by.get((sc, first["baseline"]), {})
        blocks = sorted(set(mine) & set(base))
        pairs = [(mine[b], base[b]) for b in blocks]
        get = lambda r, *path: _dig(r, path)
        row = {"scenario": sc, "config": cfg, "baseline": first["baseline"], "runs": len(mine),
               "n": len(pairs), "stmts_per_txn": first.get("stmts_per_txn", 1),
               "base_tps": median([b["tps"] for _, b in pairs]) if pairs else None,
               "base_cpu_us_per_stmt": median_none([b.get("cpu_us_per_stmt") for _, b in pairs]),
               "tps": ratio_ci([(m["tps"], b["tps"]) for m, b in pairs], conf),
               "cpu_rel": ratio_ci([(m.get("cpu_us_per_txn"), b.get("cpu_us_per_txn")) for m, b in pairs], conf),
               "cpu_diff_us": mean_ci([m["cpu_us_per_txn"] - b["cpu_us_per_txn"] for m, b in pairs
                                       if m.get("cpu_us_per_txn") is not None
                                       and b.get("cpu_us_per_txn") is not None], conf),
               "cpu_stmt_diff_us": mean_ci([m["cpu_us_per_stmt"] - b["cpu_us_per_stmt"] for m, b in pairs
                                            if m.get("cpu_us_per_stmt") is not None
                                            and b.get("cpu_us_per_stmt") is not None], conf)}
        for p in ("p50", "p95", "p99"):
            row["base_" + p + "_ms"] = median_none([get(b, "all", p + "_ms") for _, b in pairs])
            row[p] = ratio_ci([(get(m, "all", p + "_ms"), get(b, "all", p + "_ms")) for m, b in pairs], conf)
        rows.append(row)
    return rows


CORE = re.compile(r"^(ro|rw)-(simple|prepared)-c\d+$")


def _excludes_zero(ci):
    return bool(ci) and ci.get("lo") is not None and (ci["lo"] > 0 or ci["hi"] < 0)


def tagged_headline(rows):
    """The default tagged configuration (`ext`) over the core scenarios, split
    into those whose CPU-per-statement interval excludes zero and the rest."""
    core = [r for r in rows if r["config"] == "ext" and CORE.match(r["scenario"])]
    res = [r for r in core if _excludes_zero(r["cpu_stmt_diff_us"])]
    unres = [r for r in core if not _excludes_zero(r["cpu_stmt_diff_us"])]
    rng = lambda xs: (min(xs), max(xs)) if xs else None
    return {"total": len(core), "resolved": res, "unresolved": unres,
            "cpu_mean": rng([r["cpu_stmt_diff_us"]["mean"] for r in res]),
            "cpu_ci": rng([r["cpu_stmt_diff_us"]["lo"] for r in res] + [r["cpu_stmt_diff_us"]["hi"] for r in res]),
            "tps_mean": rng([r["tps"]["mean"] for r in res if r["tps"].get("mean") is not None]),
            "tps_ci": rng([r["tps"][k] for r in res for k in ("lo", "hi") if r["tps"].get(k) is not None]),
            "cpu_rel_mean": rng([r["cpu_rel"]["mean"] for r in res if r.get("cpu_rel")
                                 and r["cpu_rel"].get("mean") is not None])}


def render_headline(rows):
    h = tagged_headline(rows)
    if not h["total"]:
        return "No core scenario with the tagged configuration in this campaign."
    f = lambda v: "%+.1f" % v
    out = []
    if h["resolved"]:
        out.append("**%s to %s µs** of server CPU per statement in the %d of %d core workloads where the "
                   "interval excludes zero (their 95%% CIs lie within %s to %s µs; %s%% to %s%% of the "
                   "baseline's CPU; ΔTPS means %s%% to %s%%, CIs within %s%% to %s%%)" % (
                       f(h["cpu_mean"][0]), f(h["cpu_mean"][1]), len(h["resolved"]), h["total"],
                       f(h["cpu_ci"][0]), f(h["cpu_ci"][1]),
                       f(h["cpu_rel_mean"][0]) if h["cpu_rel_mean"] else "?",
                       f(h["cpu_rel_mean"][1]) if h["cpu_rel_mean"] else "?",
                       f(h["tps_mean"][0]), f(h["tps_mean"][1]), f(h["tps_ci"][0]), f(h["tps_ci"][1])))
    else:
        out.append("No core workload resolved a cost (0 of %d)" % h["total"])
    if h["unresolved"]:
        parts = []
        for r in h["unresolved"]:
            c, t = r["cpu_stmt_diff_us"], r["tps"]
            if c.get("lo") is None:
                parts.append("`%s` (fewer than 2 pairs)" % r["scenario"])
                continue
            parts.append("`%s` %s µs [%s, %s] (ΔTPS %s%% [%s, %s])" % (
                r["scenario"], f(c["mean"]), f(c["lo"]), f(c["hi"]), f(t["mean"]),
                f(t["lo"]) if t.get("lo") is not None else "?", f(t["hi"]) if t.get("hi") is not None else "?"))
        out.append("Not resolved, so bounded only by the upper end of the interval: " + "; ".join(parts))
    return ". ".join(out) + "."


def latency_exclusions(rows):
    """(intervals that exclude zero, intervals) over the p50/p95/p99 columns."""
    n = k = 0
    for r in rows:
        for p in ("p50", "p95", "p99"):
            ci = r.get(p)
            if ci and ci.get("lo") is not None:
                n += 1
                k += _excludes_zero(ci)
    return k, n


def _dig(r, path):
    for p in path:
        if r is None:
            return None
        r = r.get(p)
    return r


def median_none(xs):
    xs = [x for x in xs if x is not None]
    return median(xs) if xs else None


def absolute(runs):
    """Medians over blocks per (scenario, configuration)."""
    _check_unique(runs)
    order, by = [], {}
    for r in runs:
        k = (r["scenario"], r["config"])
        if k not in by:
            order.append(k)
            by[k] = []
        by[k].append(r)
    rows = []
    for k in order:
        rs = by[k]
        row = {"scenario": k[0], "config": k[1], "runs": len(rs),
               "tps": median([r["tps"] for r in rs]), "tps_spread_pct": spread_pct([r["tps"] for r in rs]),
               "server_cores": median_none([r.get("server_cores") for r in rs]),
               "cpu_us_per_txn": median_none([r.get("cpu_us_per_txn") for r in rs]),
               "cpu_us_per_stmt": median_none([r.get("cpu_us_per_stmt") for r in rs]),
               "max_worst_ms": max((_dig(r, ("all", "max_ms")) or 0) for r in rs),
               "foreign_cores_max": max((r["foreign_cores"] for r in rs if r.get("foreign_cores") is not None),
                                        default=None),
               "retries": sum(r.get("attempts", 1) - 1 for r in rs),
               "checks": max(rs, key=lambda r: r.get("block", 0)).get("checks")}
        for p in ("p50", "p95", "p99"):
            row[p + "_ms"] = median_none([_dig(r, ("all", p + "_ms")) for r in rs])
        for part in ("near", "far"):
            if all(part in r for r in rs):
                row[part] = {"n": median_none([r[part]["n"] for r in rs]),
                             "p99_ms": median_none([r[part]["p99_ms"] for r in rs]),
                             "max_worst_ms": max((r[part]["max_ms"] or 0) for r in rs)}
        rows.append(row)
    return rows


# --- raw results and reports ------------------------------------------------

# Fields kept per run in runs.jsonl (bench/results/): everything a report
# needs, not the per-transaction logs.
_KEEP = ("seq", "block", "scenario", "config", "baseline", "script", "protocol", "clients", "preload",
         "settings", "extra", "stmts_per_txn", "duration_s", "foreign_cores", "attempts", "checks",
         "tps", "txns", "server_cpu_ticks", "clk_tck", "cpu_us_per_txn", "cpu_us_per_stmt",
         "server_cores", "all", "near", "far", "boundaries_crossed", "grid")


def collect(paths, out):
    runs = []
    for p in paths:
        with open(p) as f:
            r = json.load(f)
        runs.append({k: r[k] for k in _KEEP if k in r})
    runs.sort(key=lambda r: r.get("seq", 0))
    with open(out, "w") as f:
        for r in runs:
            f.write(json.dumps(r, sort_keys=True, separators=(",", ":")) + "\n")


def load_campaign(d):
    with open(os.path.join(d, "campaign.json")) as f:
        camp = json.load(f)
    runs = []
    with open(os.path.join(d, "runs.jsonl")) as f:
        for line in f:
            if line.strip():
                runs.append(json.loads(line))
    notes = None
    if os.path.exists(os.path.join(d, "NOTES.md")):
        with open(os.path.join(d, "NOTES.md")) as f:
            notes = f.read().strip()
    return camp, runs, notes


def _f(x, fmt="%.3f"):
    return "–" if x is None else fmt % x


def _ci(ci, fmt="%+.1f", unit="%"):
    if not ci:
        return "–"
    if ci.get("lo") is None:
        return (fmt % ci["mean"]) + unit + " (n=1)"
    return "%s%s [%s, %s]" % (fmt % ci["mean"], unit, fmt % ci["lo"], fmt % ci["hi"])


def _sort_key(camp):
    pos = {}
    for i, s in enumerate(camp.get("scenarios", [])):
        for j, c in enumerate(s["configs"]):
            pos[(s["name"], c["name"])] = (i, j)
    return lambda r: pos.get((r["scenario"], r["config"]), (1 << 30, 0))


def _labels(camp):
    return {(s["name"], c["name"]): c.get("label") or c["name"]
            for s in camp.get("scenarios", []) for c in s["configs"]}


def render_campaign(d):
    camp, runs, notes = load_campaign(d)
    key, labels = _sort_key(camp), _labels(camp)
    cmp_rows = sorted(compare(runs), key=key)
    abs_rows = sorted(absolute(runs), key=key)
    name = os.path.basename(os.path.normpath(d))
    lab = lambda r: "`%s` %s" % (r["config"], labels.get((r["scenario"], r["config"]), ""))
    out = ["## Campaign `%s`" % name, ""]
    meta = [
        ("Commit measured", "`%s`%s%s" % (camp.get("commit"), " (dirty tree)" if camp.get("dirty") else "",
                                           " (measured as `%s` before a rebase that left the measured code unchanged)"
                                           % camp["rebased_from"] if camp.get("rebased_from") else "")),
        ("Date", camp.get("date")), ("Host", camp.get("host")), ("Docker", camp.get("docker")),
        ("PostgreSQL", camp.get("pg_version")), ("Build", camp.get("build")),
        ("CPUs", "server on %s (%s CPUs), pgbench on %s" % (camp.get("server_cpus"), camp.get("ncpu"),
                                                             camp.get("client_cpus"))),
        ("Design", "%s blocks; each block runs every configuration once for %s s (after a %s s warmup), "
                   "in a seeded random order (seed %s), scenario by scenario" % (
                       camp.get("blocks"), camp.get("duration_s"), camp.get("warmup_s"), camp.get("seed"))),
        ("Wall time", None if camp.get("wall_time_s") is None else "%.0f min" % (camp["wall_time_s"] / 60.0)),
        ("Quiet-machine check", camp.get("quiet_check")),
        ("Raw data", "[`bench/results/%s/`](../bench/results/%s/)" % (name, name)),
    ]
    out += ["- **%s:** %s" % (k, v) for k, v in meta if v is not None]
    out.append("")
    out += ["**Tagged statements, default configuration** (generated from the table below): "
            + render_headline(cmp_rows), ""]
    if notes:
        out += [notes, ""]
    # Bonferroni: every interval of a table at level 1 - 0.05/M holds simultaneously with 95% confidence.
    m_cpu = sum(1 for r in cmp_rows if (r["cpu_stmt_diff_us"] or {}).get("lo") is not None)
    lat_k, m_lat = latency_exclusions(cmp_rows)
    sim_cpu = {(r["scenario"], r["config"]): r for r in compare(runs, 1 - 0.05 / max(m_cpu, 1))}
    sim_lat = {(r["scenario"], r["config"]): r for r in compare(runs, 1 - 0.05 / max(m_lat, 1))}
    dag = lambda sim, r, k: " †" if _excludes_zero(sim[(r["scenario"], r["config"])][k]) else ""
    out += ["### Cost per statement and throughput", "",
            "Each configuration against pg_stat_statements alone on the same script, paired by block. "
            "Mean change and its 95%% CI (Student t over the pairs). Server CPU is the CPU time of all "
            "PostgreSQL processes during the measured run, divided by the transactions and by the "
            "statements per transaction. The verdict is on the CPU per statement: *not resolved* means the "
            "interval includes zero, and its upper end is the largest overhead the data is consistent with. "
            "Each interval is pointwise; † marks a CPU difference that also excludes zero when all %d CPU "
            "intervals of this table are made simultaneous (Bonferroni, each at %.2f%%)." % (
                m_cpu, 100 * (1 - 0.05 / max(m_cpu, 1))),
            "",
            "| Scenario | Configuration | pairs | pgss CPU µs/stmt | ΔCPU µs/stmt [95% CI] | ΔCPU % [95% CI] "
            "| ΔTPS % [95% CI] | Verdict (CPU per statement) |",
            "|---|---|---:|---:|---:|---:|---:|---|"]
    for r in cmp_rows:
        out.append("| %s | %s | %d | %s | %s | %s | %s | %s |" % (
            r["scenario"], lab(r), r["n"], _f(r["base_cpu_us_per_stmt"], "%.2f"),
            _ci(r["cpu_stmt_diff_us"], "%+.2f", "") + dag(sim_cpu, r, "cpu_stmt_diff_us"), _ci(r["cpu_rel"]),
            _ci(r["tps"]),
            verdict(r["cpu_stmt_diff_us"], unit=" µs", fmt="%+.2f")))
    out += ["", "### Latency", "",
            "Transaction latency percentiles from pgbench's per-transaction log, per run; change against the "
            "paired baseline run, mean and 95%% CI over the pairs. %d of these %d pointwise intervals exclude "
            "zero; if no configuration changed latency, about %.0f would by chance. † marks an interval that "
            "still excludes zero when all %d are made simultaneous (Bonferroni, each at %.3f%%): those effects "
            "are unlikely to be chance findings of this many comparisons, though they remain subject to the "
            "VM caveats above." % (lat_k, m_lat, 0.05 * m_lat, m_lat, 100 * (1 - 0.05 / max(m_lat, 1))), "",
            "| Scenario | Configuration | pgss p50 / p95 / p99 ms | Δp50 % [95% CI] | Δp95 % [95% CI] "
            "| Δp99 % [95% CI] |",
            "|---|---|---:|---:|---:|---:|"]
    for r in cmp_rows:
        out.append("| %s | %s | %s / %s / %s | %s | %s | %s |" % (
            r["scenario"], lab(r), _f(r["base_p50_ms"]), _f(r["base_p95_ms"]), _f(r["base_p99_ms"]),
            _ci(r["p50"]) + dag(sim_lat, r, "p50"), _ci(r["p95"]) + dag(sim_lat, r, "p95"),
            _ci(r["p99"]) + dag(sim_lat, r, "p99")))
    out += ["", "### Absolute values", "",
            "Medians over blocks. Absolute numbers describe this host only.", "",
            "| Scenario | Configuration | runs | TPS | TPS spread | server cores | CPU µs/txn | CPU µs/stmt "
            "| p50 ms | p95 ms | p99 ms | worst max ms |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in abs_rows:
        out.append("| %s | `%s` | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["scenario"], r["config"], r["runs"], _f(r["tps"], "%.0f"), _f(r["tps_spread_pct"], "%.1f%%"),
            _f(r["server_cores"], "%.2f"), _f(r["cpu_us_per_txn"], "%.1f"), _f(r["cpu_us_per_stmt"], "%.2f"),
            _f(r["p50_ms"]), _f(r["p95_ms"]), _f(r["p99_ms"]), _f(r["max_worst_ms"], "%.2f")))
    bnd = [r for r in abs_rows if "near" in r and any(
        x["scenario"] == r["scenario"] and "bucket_interval" in (x.get("settings") or "") for x in runs)]
    if bnd:
        out += ["", "### Bucket boundaries", "",
                "Transactions whose execution overlaps ±%s ms of a whole second (a bucket boundary with "
                "1 s buckets) against the rest; medians over blocks. Configurations without 1 s buckets "
                "are controls." % _f(runs[0]["grid"]["window_us"] / 1000.0, "%g"), "",
                "| Scenario | Configuration | near n | near p99 ms | near worst max ms | far p99 ms "
                "| far worst max ms |", "|---|---|---:|---:|---:|---:|---:|"]
        for r in bnd:
            out.append("| %s | `%s` | %s | %s | %s | %s | %s |" % (
                r["scenario"], r["config"], _f(r["near"]["n"], "%.0f"), _f(r["near"]["p99_ms"]),
                _f(r["near"]["max_worst_ms"], "%.2f"), _f(r["far"]["p99_ms"]), _f(r["far"]["max_worst_ms"], "%.2f")))
    out += ["", "### Validation", "",
            "Every run was checked (bench/inside.sh); shown are the values of the last block. Foreign CPU: "
            "cores used outside the benchmark container during a run (max over runs); retries: runs repeated "
            "because of it.", "",
            "| Scenario | Configuration | checks (last block) | foreign CPU (max) | retries |",
            "|---|---|---|---:|---:|"]
    for r in abs_rows:
        out.append("| %s | `%s` | %s | %s | %d |" % (
            r["scenario"], r["config"], ", ".join("%s=%s" % kv for kv in (r.get("checks") or {}).items()) or "–",
            _f(r["foreign_cores_max"], "%.2f"), r["retries"]))
    return "\n".join(out) + "\n"


GENERATED = ("<!-- Generated by bench/analyze.py doc from bench/benchmarks.md.in and bench/results/. "
             "Do not edit; edit those and regenerate. -->\n")


def campaign_dirs(root):
    return sorted((os.path.join(root, d) for d in os.listdir(root)
                   if os.path.exists(os.path.join(root, d, "campaign.json"))), reverse=True)


def build_doc(template, root):
    if "{{campaigns}}" not in template:
        raise ValueError("template has no {{campaigns}} marker")
    dirs = campaign_dirs(root)
    body = "\n".join(render_campaign(d) for d in dirs)
    if "{{headline}}" in template:
        if not dirs:
            raise ValueError("{{headline}} needs at least one campaign")
        _, runs, _ = load_campaign(dirs[0])
        template = template.replace("{{headline}}", render_headline(compare(runs)))
    return GENERATED + template.replace("{{campaigns}}", body.rstrip("\n"))


CODE_PATHS = ["src", "sql", "Makefile", "pg_stat_statement_context.control"]


def stale_paths(commit, root):
    """Extension code files that differ between commit and the working tree."""
    out = subprocess.check_output(["git", "-C", root, "diff", "--name-only", commit, "--"] + CODE_PATHS,
                                  text=True)
    return [p for p in out.splitlines() if p]


def is_ancestor(commit, root):
    return subprocess.call(["git", "-C", root, "merge-base", "--is-ancestor", commit, "HEAD"],
                           stderr=subprocess.DEVNULL) == 0


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
    r.add_argument("--plan", help="plan.json of bench/scenarios.py: merge the row of --seq into the meta")
    r.add_argument("--seq")
    r.add_argument("logs", nargs="+")
    c = sub.add_parser("collect")
    c.add_argument("--out", required=True)
    c.add_argument("runs", nargs="+")
    p = sub.add_parser("report")
    p.add_argument("dir")
    p.add_argument("--out")
    d = sub.add_parser("doc")
    d.add_argument("--results", required=True)
    d.add_argument("--template", required=True)
    d.add_argument("--out", required=True)
    d.add_argument("--check-fresh", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "run":
        interval_us = int(round(a.interval_s * 1e6))
        meta = json.loads(a.meta)
        if a.plan:
            with open(a.plan) as f:
                meta = run_meta(meta, json.load(f)[a.seq])
        res = analyze_run(a.logs, a.pgbench_out, meta, interval_us,
                          (a.offset_s * 1_000_000) % interval_us, int(round(a.window_ms * 1000)))
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
        s = res["all"]
        print("  %-20s %-20s tps=%.0f cpu/stmt=%sus p50=%.3f p95=%.3f p99=%.3fms max=%.2fms" % (
            res.get("scenario"), res.get("config"), res["tps"], _f(res.get("cpu_us_per_stmt"), "%.2f"),
            s["p50_ms"], s["p95_ms"], s["p99_ms"], s["max_ms"]))
        return 0
    if a.cmd == "collect":
        collect(a.runs, a.out)
        return 0
    if a.cmd == "report":
        md = render_campaign(a.dir)
        if a.out:
            with open(a.out, "w") as f:
                f.write(md)
        else:
            sys.stdout.write(md)
        return 0
    with open(a.template) as f:
        doc = build_doc(f.read(), a.results)
    if a.check_fresh:
        dirs = campaign_dirs(a.results)
        if not dirs:
            print("no campaign under %s" % a.results, file=sys.stderr)
            return 1
        camp, _, _ = load_campaign(dirs[0])
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if not is_ancestor(camp["commit"], root):
            print("%s: commit %s is not an ancestor of HEAD" % (dirs[0], camp["commit"]), file=sys.stderr)
            return 1
        stale = stale_paths(camp["commit"], root)
        if stale:
            print("%s: the extension changed since %s: %s" % (dirs[0], camp["commit"], " ".join(stale)),
                  file=sys.stderr)
            return 1
    with open(a.out, "w") as f:
        f.write(doc)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
