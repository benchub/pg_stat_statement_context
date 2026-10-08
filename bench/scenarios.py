#!/usr/bin/env python3
"""Scenario matrix, run plan and pgbench scripts for bench/run.sh. Standard
library only; tested by bench/test_analyze.py.

A scenario is one pgbench workload (script family, protocol, client count).
Each has a baseline (pg_stat_statements alone) for every script it uses, and
configurations with this extension that are compared with that baseline on
the same script. The plan runs every configuration of every scenario once
per block; the blocks are the pairing unit of the statistics
(bench/analyze.py). Within a block the scenario order and, within a
scenario, the configuration order are shuffled (seeded), so a run and its
baseline are a few minutes apart at most and neither systematically goes
first.

  scenarios.py plan --ncpu N --duration S --blocks B --seed X [--high-clients]
                    [--only REGEX] --out-tsv FILE --out-plan-json FILE --out-json FILE
      the plan (one run per line, for bench/inside.sh; and by seq, for the
      run metadata) and the scenario definitions (for campaign.json)
  scenarios.py scripts DIR [--inlist-n 10000]
  scenarios.py setup-sql
  scenarios.py exporter-sql QUERIES_YAML OUT
"""
import argparse
import json
import random
import re
import sys

EXT = "pg_stat_statement_context"

# Six sqlcommenter tags; action and controller pass the default allowlist,
# route and db_driver are used by the regex/normalize configuration.
COMMENT = ("/*action='show',controller='users',db_driver='psycopg2',framework='django%3A4.2.1',"
           "route='%2Fusers%2F42',traceparent='00-5bd66ef5095369c7b0d1f8f4bd33716a-c532cb4098ac3dd2-01'*/")
EVICT_COMMENT = ("/*action='show',controller='users:r',db_driver='psycopg2',framework='django%3A4.2.1',"
                 "traceparent='00-5bd66ef5095369c7b0d1f8f4bd33716a-c532cb4098ac3dd2-01'*/")
INNER_COMMENT = "/*controller='nested',action='loop'*/"
NESTED_N = 4  # bench_nested() runs NESTED_N^2 inner SELECTs

SET_AID = "\\set aid random(1, 100000 * :scale)"
POINT = "SELECT abalance FROM pgbench_accounts WHERE aid = :aid"
TPCB = [
    "UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid",
    "SELECT abalance FROM pgbench_accounts WHERE aid = :aid",
    "UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid",
    "UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid",
    "INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP)",
]
TPCB_SET = ["\\set aid random(1, 100000 * :scale)", "\\set bid random(1, 1 * :scale)",
            "\\set tid random(1, 10 * :scale)", "\\set delta random(-5000, 5000)"]

ANY = "%s.extractors = 'sqlcommenter(position=any), marginalia(position=any)'" % EXT
REGEX_NORMALIZE = [
    "%s.tags = 'action, controller, job, route, driver'" % EXT,
    "%s.extractors = 'sqlcommenter, regex(pattern=''db_driver=.(\\\\w+)'', keys=driver, merge=on)'" % EXT,
    "%s.normalize = 'route: ''/\\\\d+'' => ''/:id'''" % EXT,
]
STORE_ENTRIES = 5000

TSV_FIELDS = ["seq", "block", "scenario", "config", "baseline", "script", "protocol", "clients",
              "preload", "settings", "checks", "extra", "stmts_per_txn"]


def scripts(inlist_n=10000):
    """pgbench scripts by name. pgbench sends a statement's trailing ';'
    (stmt_len > 0); a last statement without one is sent as is, so the
    server sees stmt_len = 0, the strlen() case (inlist0)."""
    inlist = ",".join(str(-i) for i in range(inlist_n, 0, -1))
    tpcb = lambda c: "\n".join(TPCB_SET + ["BEGIN;"] + ["%s%s;" % (s, c) for s in TPCB] + ["END;"]) + "\n"
    return {
        "ro-plain": "%s\n%s;\n" % (SET_AID, POINT),
        "ro": "%s\n%s %s;\n" % (SET_AID, POINT, COMMENT),
        "rw-plain": tpcb(""),
        "rw": tpcb(" " + COMMENT),
        "nested": "%s\nSELECT bench_nested(%d, :aid) %s;\n" % (SET_AID, NESTED_N, COMMENT),
        "inlist": "%s\n%s AND abalance NOT IN (%s) %s;\n" % (SET_AID, POINT, inlist, COMMENT),
        "inlist0": "%s\n%s AND abalance NOT IN (%s) %s\n" % (SET_AID, POINT, inlist, COMMENT),
        "evict": "%s\n\\set r random(1, 1000000000)\n%s %s;\n" % (SET_AID, POINT, EVICT_COMMENT),
    }


def setup_sql():
    """Objects the scripts need, created once after pgbench -i."""
    return ("CREATE FUNCTION bench_nested(n int, a int) RETURNS bigint LANGUAGE plpgsql AS $$\n"
            "DECLARE s bigint := 0; b int;\n"
            "BEGIN\n"
            "  FOR i IN 1..n LOOP\n"
            "    FOR j IN 1..n LOOP\n"
            "      SELECT abalance INTO b FROM pgbench_accounts WHERE aid = (a + i * n + j) %% 100000 + 1 %s;\n"
            "      s := s + b;\n"
            "    END LOOP;\n"
            "  END LOOP;\n"
            "  RETURN s;\n"
            "END $$;\n" % INNER_COMMENT)


def _cfg(name, baseline, preload, script, label, settings=(), checks=(), extra="", stmts=1):
    return {"name": name, "baseline": baseline, "preload": preload, "script": script, "label": label,
            "settings": list(settings), "checks": list(checks), "extra": extra, "stmts_per_txn": stmts}


def scenarios(ncpu, duration, high_clients=False):
    """The scenario matrix. ncpu is the number of CPUs the server runs on."""
    out = []
    clients = [1, ncpu, 4 * ncpu] + ([256] if high_clients else [])
    for wl, stmts, tagged in (("ro", 1, 1), ("rw", 7, 5)):
        for proto in ("simple", "prepared"):
            for c in clients:
                cfgs = [
                    _cfg("pgss", "pgss", "pgss", wl, "pg_stat_statements only", checks=["pgss"], stmts=stmts),
                    _cfg("ext", "pgss", "ext", wl, "+ extension, tagged, defaults",
                         checks=["tagged:%d" % tagged], stmts=stmts),
                ]
                if c == ncpu and (wl == "ro" or proto == "simple"):
                    cfgs += [
                        _cfg("plain-pgss", "plain-pgss", "pgss", wl + "-plain",
                             "pg_stat_statements only, no comments", checks=["pgss"], stmts=stmts),
                        _cfg("plain-ext", "plain-pgss", "ext", wl + "-plain",
                             "+ extension, untagged (untagged = skip)", checks=["untagged"], stmts=stmts),
                        _cfg("ext-regex-normalize", "pgss", "ext", wl,
                             "+ extension, regex extractor (merge) and a normalize rule",
                             settings=REGEX_NORMALIZE, checks=["regexnorm:%d" % tagged], stmts=stmts),
                    ]
                if c == ncpu and wl == "ro" and proto == "simple":
                    cfgs += [
                        _cfg("ext-store5k", "pgss", "ext", wl,
                             "+ extension, store pre-populated with %d entries" % STORE_ENTRIES,
                             checks=["tagged:1", "store:%d" % STORE_ENTRIES], extra="store:%d" % STORE_ENTRIES),
                        _cfg("ext-reader15s", "pgss", "ext", wl,
                             "+ extension, exporter queries every 15 s",
                             checks=["tagged:1", "reader"], extra="reader:15"),
                        _cfg("ext-reader1s", "pgss", "ext", wl,
                             "+ extension, exporter queries every 1 s",
                             checks=["tagged:1", "reader"], extra="reader:1"),
                        _cfg("ext-1s-buckets", "pgss", "ext", wl,
                             "+ extension, 1 s buckets (a rollover every second)",
                             settings=["%s.bucket_interval = '1s'" % EXT,
                                       "%s.bucket_count = %d" % (EXT, duration + 30)],
                             checks=["tagged:1", "buckets"]),
                    ]
                out.append({"name": "%s-%s-c%d" % (wl, proto, c), "workload": wl, "protocol": proto,
                            "clients": c, "configs": cfgs})
    n2 = NESTED_N * NESTED_N
    all_pgss = "pg_stat_statements.track = all"
    out.append({"name": "nested-simple-c%d" % ncpu, "workload": "nested", "protocol": "simple",
                "clients": ncpu, "configs": [
                    _cfg("pgss", "pgss", "pgss", "nested", "pg_stat_statements only (track = top)",
                         checks=["pgss"], stmts=n2 + 1),
                    _cfg("ext", "pgss", "ext", "nested", "+ extension, defaults (track = top)",
                         checks=["tagged:1"], stmts=n2 + 1),
                    _cfg("pgss-all", "pgss-all", "pgss", "nested", "pg_stat_statements only (track = all)",
                         settings=[all_pgss], checks=["pgss"], stmts=n2 + 1),
                    _cfg("ext-all-inherit", "pgss-all", "ext", "nested",
                         "+ extension, track = all, nested_tags = inherit",
                         settings=[all_pgss, "%s.track = all" % EXT], checks=["tagged:1", "nested:inherit:%d" % n2],
                         stmts=n2 + 1),
                    _cfg("ext-all-scan", "pgss-all", "ext", "nested",
                         "+ extension, track = all, nested_tags = scan",
                         settings=[all_pgss, "%s.track = all" % EXT, "%s.nested_tags = scan" % EXT],
                         checks=["tagged:1", "nested:scan:%d" % n2], stmts=n2 + 1),
                ]})
    out.append({"name": "inlist-simple-c%d" % ncpu, "workload": "inlist", "protocol": "simple",
                "clients": ncpu, "configs": [
                    _cfg("inlist-pgss", "inlist-pgss", "pgss", "inlist",
                         "10k IN list + ';': pg_stat_statements only", checks=["pgss"]),
                    _cfg("inlist-ext", "inlist-pgss", "ext", "inlist",
                         "10k IN list + ';': position = append (heuristic tail scan)",
                         checks=["tagged:1", "heuristic"]),
                    _cfg("inlist-ext-any", "inlist-pgss", "ext", "inlist",
                         "10k IN list + ';': position = any (full scan)", settings=[ANY],
                         checks=["tagged:1", "exact"]),
                    _cfg("inlist0-pgss", "inlist0-pgss", "pgss", "inlist0",
                         "10k IN list, no ';' (stmt_len = 0): pg_stat_statements only", checks=["pgss"]),
                    _cfg("inlist0-ext", "inlist0-pgss", "ext", "inlist0",
                         "10k IN list, no ';': position = append (strlen + tail scan)",
                         checks=["tagged:1", "heuristic"]),
                    _cfg("inlist0-ext-any", "inlist0-pgss", "ext", "inlist0",
                         "10k IN list, no ';': position = any (full scan)", settings=[ANY],
                         checks=["tagged:1", "exact"]),
                ]})
    out.append({"name": "evict-simple-c%d" % ncpu, "workload": "evict", "protocol": "simple",
                "clients": ncpu, "configs": [
                    _cfg("pgss", "pgss", "pgss", "evict", "pg_stat_statements only", checks=["pgss"]),
                    _cfg("ext-max10000", "pgss", "ext", "evict",
                         "+ extension, a new tag set every statement, max_entries = 10000 (sustained eviction)",
                         settings=["%s.max_entries = 10000" % EXT], checks=["evict"]),
                ]})
    for s in out:
        for c in s["configs"]:
            for x in c["settings"]:
                assert ";" not in x and "\t" not in x and "|" not in x, x
    return out


def make_plan(scs, blocks, seed, only=None):
    rng = random.Random(seed)
    plan, seq = [], 0
    pat = re.compile(only) if only else None
    for b in range(1, blocks + 1):
        order = [s for s in scs if not pat or pat.search(s["name"])]
        rng.shuffle(order)
        for s in order:
            cfgs = list(s["configs"])
            rng.shuffle(cfgs)
            for c in cfgs:
                seq += 1
                plan.append({"seq": seq, "block": b, "scenario": s["name"], "config": c["name"],
                             "baseline": c["baseline"], "script": c["script"], "protocol": s["protocol"],
                             "clients": s["clients"], "preload": c["preload"],
                             "settings": ";".join(c["settings"]), "checks": ",".join(c["checks"]),
                             "extra": c["extra"], "stmts_per_txn": c["stmts_per_txn"]})
    return plan


def plan_tsv(plan):
    """One line per run; an empty field is written as "-" (bash's read
    collapses consecutive tabs)."""
    return "".join("\t".join(str(r[k]) if str(r[k]) else "-" for k in TSV_FIELDS) + "\n" for r in plan)


def exporter_queries(path):
    """The SQL of every `query: |` block of a postgres_exporter queries.yaml."""
    with open(path) as f:
        lines = f.read().split("\n")
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"^(\s*)query: \|\s*$", lines[i])
        i += 1
        if not m:
            continue
        indent, body = len(m.group(1)), []
        while i < len(lines) and (not lines[i].strip() or len(lines[i]) - len(lines[i].lstrip()) > indent):
            body.append(lines[i])
            i += 1
        while body and not body[-1].strip():
            body.pop()
        cut = min(len(x) - len(x.lstrip()) for x in body if x.strip())
        out.append("\n".join(x[cut:] for x in body))
    return out


def exporter_queries_sql(qs):
    return "".join(q.rstrip().rstrip(";") + ";\n" for q in qs)


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--ncpu", type=int, required=True)
    p.add_argument("--duration", type=int, required=True)
    p.add_argument("--blocks", type=int, required=True)
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--high-clients", action="store_true")
    p.add_argument("--only")
    p.add_argument("--out-tsv", required=True)
    p.add_argument("--out-plan-json", required=True)
    p.add_argument("--out-json", required=True)
    s = sub.add_parser("scripts")
    s.add_argument("dir")
    s.add_argument("--inlist-n", type=int, default=10000)
    sub.add_parser("setup-sql")
    e = sub.add_parser("exporter-sql")
    e.add_argument("yaml")
    e.add_argument("out")
    a = ap.parse_args(argv)
    if a.cmd == "plan":
        scs = scenarios(a.ncpu, a.duration, a.high_clients)
        plan = make_plan(scs, a.blocks, a.seed, a.only)
        if not plan:
            print("no scenario matches --only %r" % a.only, file=sys.stderr)
            return 1
        names = {r["scenario"] for r in plan}
        with open(a.out_tsv, "w") as f:
            f.write(plan_tsv(plan))
        with open(a.out_plan_json, "w") as f:
            json.dump({str(r["seq"]): r for r in plan}, f)
        with open(a.out_json, "w") as f:
            json.dump([x for x in scs if x["name"] in names], f, indent=1)
    elif a.cmd == "scripts":
        import os
        for name, text in scripts(a.inlist_n).items():
            with open(os.path.join(a.dir, name + ".sql"), "w") as f:
                f.write(text)
    elif a.cmd == "setup-sql":
        sys.stdout.write(setup_sql())
    else:
        with open(a.out, "w") as f:
            f.write(exporter_queries_sql(exporter_queries(a.yaml)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
