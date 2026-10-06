#!/usr/bin/env python3
"""Checks of scripts/test-integrations.sh (stdlib only).

Polls until every check passes or --timeout expires:
  1. each exporter's /metrics (postgres_exporter, sql_exporter, otelcol) has
     the recipe's series with sensible values for the sample workload
     (test/integrations/workload.sql);
  2. Prometheus has scraped them under each job;
  3. Grafana has provisioned the dashboard, and every query of every panel
     returns data through /api/ds/query, once per exporter job.

--phase rollback: the store's clock has been stepped ahead of now() (as after
a backward clock step); postgres_exporter and sql_exporter, which query on
every scrape, must export no per-second series. --phase recovered: once the
clock has caught up, the exporter checks of the main phase pass again.
--require-running: containers (the workload) that must keep running; the
checks fail at once if one exits.
"""
import argparse
import base64
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

LINE_RE = re.compile(r'^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{(.*)\})?\s+(\S+)(\s+\d+)?$')
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)"')

JOBS = ["postgres_exporter", "sql_exporter", "otelcol"]
DASHBOARD_UID = "pg-stat-statement-context"


def http(url, data=None, auth=None, timeout=10):
    req = urllib.request.Request(url, data=data)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if auth:
        req.add_header("Authorization",
                       "Basic " + base64.b64encode(auth.encode()).decode())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def parse_metrics(text):
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        m = LINE_RE.match(line)
        if not m:
            continue
        labels = dict(LABEL_RE.findall(m.group(3) or ""))
        out.setdefault(m.group(1), []).append((labels, float(m.group(4))))
    return out


def pick(metrics, name, **want):
    """Sum of the samples of `name` whose labels include `want`."""
    rows = [v for lbl, v in metrics.get(name, [])
            if all(lbl.get(k) == w for k, w in want.items())]
    return sum(rows) if rows else None


class Checks:
    def __init__(self):
        self.failures = []

    def ok(self, cond, msg):
        if not cond:
            self.failures.append(msg)
        return cond


def check_exporter(c, name, url):
    try:
        m = parse_metrics(http(url + "/metrics"))
    except Exception as e:  # noqa: BLE001
        c.ok(False, f"{name}: cannot fetch /metrics: {e}")
        return
    top = dict(datname="shop", toplevel="true")
    show = pick(m, "pssc_tag_calls_per_second", tag_controller="users", tag_action="show", **top)
    index = pick(m, "pssc_tag_calls_per_second", tag_controller="posts", tag_action="index", **top)
    orders = pick(m, "pssc_tag_calls_per_second", tag_controller="orders", tag_action="create", **top)
    nested = pick(m, "pssc_tag_calls_per_second", datname="shop", toplevel="false",
                  tag_controller="orders", tag_action="create")
    cleanup = pick(m, "pssc_tag_calls_per_second", tag_job="cleanup", **top)
    check = pick(m, "pssc_tag_calls_per_second", tag_controller="", tag_action="check", **top)
    if not c.ok(show and show > 0, f"{name}: no pssc_tag_calls_per_second for users#show (toplevel)"):
        return

    # One workload iteration runs users#show 5 times, posts#index twice, and
    # orders#create (3 nested statements each), job=cleanup and the invalid
    # tag once. A bucket holds whole iterations unless one straddles its
    # boundary; the checks are polled, so the bands are tight enough that a
    # missing or extra workload statement can never pass.
    def ratio(a, b, want, what):
        c.ok(a and b and abs(a / b - want) <= want * 0.04,
             f"{name}: {what} call-rate ratio {a}/{b} is not {want}")

    ratio(show, index, 2.5, "users#show/posts#index")
    ratio(index, orders, 2.0, "posts#index/orders#create")
    ratio(nested, orders, 3.0, "nested/top-level orders#create")
    ratio(cleanup, orders, 1.0, "job=cleanup/orders#create")
    ratio(check, orders, 1.0, "action=check (invalid controller tag)/orders#create")
    c.ok((pick(m, "pssc_tag_exec_seconds_per_second", tag_controller="users",
               tag_action="show", **top) or 0) > 0,
         f"{name}: pssc_tag_exec_seconds_per_second for users#show is missing or 0")
    c.ok(all(lbl.get("toplevel") in ("true", "false")
             for lbl, _ in m.get("pssc_tag_calls_per_second", [])),
         f"{name}: toplevel label is not true/false")
    q = [lbl for lbl, v in m.get("pssc_query_calls_per_second", [])
         if lbl.get("tag_controller") == "users" and lbl.get("toplevel") == "true"
         and re.fullmatch(r"-?\d+", lbl.get("queryid", "")) and v > 0]
    c.ok(q, f"{name}: no pssc_query_calls_per_second series with a queryid for users#show")
    c.ok(pick(m, "pssc_query_exec_seconds_per_second") is not None,
         f"{name}: pssc_query_exec_seconds_per_second missing")

    def info(metric, cond, what):
        v = pick(m, metric)
        c.ok(v is not None and cond(v), f"{name}: {metric} = {v}, expected {what}")

    info("pssc_info_max_entries", lambda v: v == 10000, "10000")
    info("pssc_info_buckets", lambda v: v == 6, "6")
    info("pssc_info_bucket_interval_seconds", lambda v: v == 10, "10")
    info("pssc_info_entries", lambda v: v > 0, "> 0")
    info("pssc_info_live_entries", lambda v: 0 < v <= pick(m, "pssc_info_entries"), "in (0, entries]")
    info("pssc_info_dealloc_total", lambda v: v >= 0, ">= 0")
    info("pssc_info_evicted_entries_total", lambda v: v >= 0, ">= 0")
    info("pssc_info_invalid_tags_total", lambda v: v > 0, "> 0 (the workload sends an invalid tag)")
    info("pssc_info_dropped_tags_total", lambda v: v >= 0, ">= 0")
    info("pssc_info_heuristic_scans_total", lambda v: v >= 0, ">= 0")
    info("pssc_info_regex_compile_failures_total", lambda v: v == 0, "0")
    info("pssc_info_utility_missing_queryid_total", lambda v: v == 0, "0")
    info("pssc_info_oldest_bucket_age_seconds", lambda v: 0 <= v <= 70, "in [0, 70]")
    info("pssc_info_stats_reset_timestamp_seconds", lambda v: abs(v - time.time()) < 3600,
         "within the last hour")


def check_rollback(c, name, url):
    try:
        m = parse_metrics(http(url + "/metrics"))
    except Exception as e:  # noqa: BLE001
        c.ok(False, f"{name}: cannot fetch /metrics: {e}")
        return
    for metric in ("pssc_tag_calls_per_second", "pssc_query_calls_per_second"):
        c.ok(not m.get(metric),
             f"{name}: {metric} is still exported while the clock is behind the store "
             f"(an older, already exported bucket): {m.get(metric, [])[:3]}")
    c.ok(pick(m, "pssc_info_entries") is not None, f"{name}: pssc_info_entries missing")


def check_running(c, containers):
    for n in containers:
        r = subprocess.run(["docker", "inspect", "-f", "{{.State.Running}} {{.State.ExitCode}}", n],
                           capture_output=True, text=True)
        c.ok(r.stdout.startswith("true"),
             f"container {n} is not running (state: {r.stdout.strip() or r.stderr.strip()})")


def check_prometheus(c, url):
    for job in JOBS:
        q = (f'pssc_tag_calls_per_second{{job="{job}",toplevel="true",'
             f'tag_controller="users",tag_action="show"}}')
        try:
            r = json.loads(http(url + "/api/v1/query?" + urllib.parse.urlencode({"query": q})))
        except Exception as e:  # noqa: BLE001
            c.ok(False, f"prometheus: query failed: {e}")
            return
        c.ok(r.get("status") == "success" and r["data"]["result"],
             f"prometheus: no users#show series for job {job}")


def panels(dash):
    for p in dash.get("panels", []):
        yield p
        yield from p.get("panels", [])  # collapsed rows


def substitute(s, values):
    for k, v in sorted(values.items(), key=lambda kv: -len(kv[0])):
        s = s.replace("${" + k + "}", v).replace("[[" + k + "]]", v).replace("$" + k, v)
    return s


def check_grafana(c, url, auth, ds_uid):
    try:
        health = json.loads(http(url + "/api/health"))
        dash = json.loads(http(url + f"/api/dashboards/uid/{DASHBOARD_UID}", auth=auth))["dashboard"]
    except Exception as e:  # noqa: BLE001
        c.ok(False, f"grafana: dashboard {DASHBOARD_UID} not available: {e}")
        return
    c.ok(health.get("database") == "ok", f"grafana: unhealthy: {health}")
    values = {"__rate_interval": "1m", "__range": "5m", "__range_s": "300", "__interval": "15s"}
    for v in dash.get("templating", {}).get("list", []):
        if v["type"] == "datasource":
            values[v["name"]] = ds_uid
        elif v.get("includeAll"):
            values[v["name"]] = v.get("allValue") or ".*"
        else:
            values[v["name"]] = str(v.get("current", {}).get("value", ""))
    c.ok(values.get("toplevel") == "true", "grafana: the toplevel variable must default to true")
    nqueries = 0
    for p in panels(dash):
        for t in p.get("targets", []):
            if "expr" not in t:
                continue
            for job in JOBS:
                expr = substitute(t["expr"], dict(values, job=job))
                body = {"from": "now-5m", "to": "now", "queries": [{
                    "refId": "A", "datasource": {"type": "prometheus", "uid": ds_uid},
                    "expr": expr, "instant": bool(t.get("instant")),
                    "range": not t.get("instant"), "intervalMs": 15000,
                    "maxDataPoints": 100}]}
                what = f"grafana: panel '{p.get('title')}' ({job}): {expr}"
                try:
                    r = json.loads(http(url + "/api/ds/query", json.dumps(body).encode(), auth))
                except urllib.error.HTTPError as e:
                    c.ok(False, f"{what}: HTTP {e.code} {e.read()[:300]!r}")
                    continue
                res = r["results"]["A"]
                if not c.ok("error" not in res, f"{what}: {res.get('error')}"):
                    continue
                vals = [x for f in res.get("frames", [])
                        for col in f.get("data", {}).get("values", [])[1:]
                        for x in col if x is not None]
                c.ok(vals, f"{what}: no data")
                nqueries += 1
    c.ok(nqueries >= 10, f"grafana: only {nqueries} panel queries checked")
    return nqueries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--postgres-exporter", required=True)
    ap.add_argument("--sql-exporter", required=True)
    ap.add_argument("--otelcol", required=True)
    ap.add_argument("--prometheus", required=True)
    ap.add_argument("--grafana", required=True)
    ap.add_argument("--grafana-auth", default="admin:admin")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--phase", choices=["main", "rollback", "recovered"], default="main")
    ap.add_argument("--require-running", action="append", default=[])
    a = ap.parse_args()
    deadline = time.time() + a.timeout
    while True:
        c = Checks()
        check_running(c, a.require_running)
        if c.failures:
            return report(c)
        if a.phase == "rollback":
            check_rollback(c, "postgres_exporter", a.postgres_exporter)
            check_rollback(c, "sql_exporter", a.sql_exporter)
            if not c.failures:
                print("OK: postgres_exporter and sql_exporter export no per-second series "
                      "while the clock is behind the store")
                return 0
            if time.time() > deadline:
                return report(c)
            time.sleep(2)
            continue
        check_exporter(c, "postgres_exporter", a.postgres_exporter)
        check_exporter(c, "sql_exporter", a.sql_exporter)
        check_exporter(c, "otelcol", a.otelcol)
        check_prometheus(c, a.prometheus)
        n = None
        if a.phase == "recovered":
            if not c.failures:
                print("OK: all 3 exporters recovered after the clock caught up")
                return 0
        elif not c.failures:
            n = check_grafana(c, a.grafana, a.grafana_auth, "prometheus")
        if not c.failures:
            print(f"OK: 3 exporters, Prometheus, and {n} Grafana panel queries "
                  f"({n // len(JOBS)} per exporter job) returned data")
            return 0
        if time.time() > deadline:
            return report(c)
        time.sleep(5)


def report(c):
    print("FAILED checks:", file=sys.stderr)
    for f in c.failures:
        print("  - " + f, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
