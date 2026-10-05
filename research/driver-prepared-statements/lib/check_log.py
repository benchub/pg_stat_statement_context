#!/usr/bin/env python3
"""Classify and assert a scenario from a PostgreSQL jsonlog captured with
log_min_duration_statement = 0.

Every repro client runs a fixed workload. Each call puts `ctx=cN` (or `ctx:cN`,
`ctx='cN'`) in a SQL comment AND sends the same `cN` as a bind parameter (or a
string literal when the driver interpolates). For every *execution* the server
logs the source text it actually runs, i.e. what pg_stat_statement_context
would see (DESIGN.md §6.3):

    duration: 0.020 ms  execute S_1: SELECT ... /*ctx=c0*/
    DETAIL:  Parameters: $1 = 'c1'

Only SELECT executions carrying an intended ctx are "attributable"; setup
statements and driver bookkeeping are ignored.

Verdict (asserted against <expect>):
  fresh        every attributable execution's comment matches the intended ctx
  stale        at least one execution ran with another call's comment
  uncommented  no attributable execution carried a ctx comment at all
  partial      some commented (all matching), some not

Prepared-state metrics (asserted exactly against <invariants>, "k=v k=v ..."),
so a PASS proves the scenario really exercised the driver's caching:
  exec             attributable executions (complete workload)
  stale            executions whose comment ctx != intended ctx
  uncommented      executions without a ctx comment
  named            distinct (backend pid, statement name) named statements executed
  named_exec       executions of named statements
  named_parses     Parse messages for those (pid, name)  (== contexts => one Parse each)
  named_hits       named executions with no Parse since that statement's previous
                   execution (cache hits: the saved source text is reused)
  unnamed_exec     extended-protocol executions of the unnamed statement
  unnamed_reparsed unnamed executions whose text was Parsed right before (same pid)
  unnamed_reused   unnamed executions WITHOUT a fresh Parse
  simple_exec      simple-protocol executions (`statement:`)
  shared           named statements Parsed once on a backend and executed under
                   >= 2 distinct application_names (pgbouncer server-side sharing)
  pids             distinct backend pids among executions
  apps             distinct application_names among executions

Usage:
  check_log.py <scenario> <expect> "<invariants>" < postgres.json
  check_log.py --selftest <scenario> <expect> "<invariants>" < postgres.json
      Checks the real log passes, then applies mutations (strip Parses, drop
      named executions, make everything unnamed, collapse clients, truncate the
      workload, inject one stale comment, and the review-round-1 mutation: no
      Parses and no named executions) and requires each applicable mutation
      to FAIL.
Exit 0 on success; 1 on assertion failure; 2 if the log is vacuous.
"""
import copy
import json
import re
import sys

MSG_RE = re.compile(
    r"^(?P<prefix>duration: [0-9.]+ ms  )"
    r"(?P<kind>parse|bind|execute(?: fetch from)?|statement) ?"
    r"(?P<name>[^:]*?): (?P<text>.*)$",
    re.S,
)
CTX_RE = re.compile(r"""ctx\s*[=:]\s*'?(c\d+)""")
PARAM_RE = re.compile(r"""\$\d+ = '(c\d+)'""")
LITERAL_RE = re.compile(r"""'(c\d+)'""")
COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
SELECT_RE = re.compile(r"\s*SELECT\b", re.I)
UNNAMED = "<unnamed>"

MIN_EXECUTIONS = 10
MIN_CONTEXTS = 3
VERDICTS = ("fresh", "stale", "uncommented", "partial")


def intended_ctx(text, detail):
    m = PARAM_RE.search(detail or "")
    if m:
        return m.group(1)
    m = LITERAL_RE.search(COMMENT_RE.sub("", text))
    return m.group(1) if m else None


def parse_msg(rec):
    m = MSG_RE.match(rec.get("message", ""))
    if not m or not SELECT_RE.match(COMMENT_RE.sub("", m.group("text"))):
        return None
    name = m.group("name").strip().split("/")[0] or "-"  # drop "/portal"
    return m.group("kind"), name, m.group("text")


def analyze(records):
    executions = []
    parses = {}            # (pid, name) -> count
    since_exec = {}        # (pid, name) -> parsed since last execution?
    last_unnamed = {}      # pid -> text of the latest unnamed Parse not yet executed
    for rec in records:
        p = parse_msg(rec)
        if not p:
            continue
        kind, name, text = p
        pid = rec.get("pid")
        if kind == "parse":
            if name == UNNAMED:
                last_unnamed[pid] = text
            else:
                parses[(pid, name)] = parses.get((pid, name), 0) + 1
                since_exec[(pid, name)] = True
            continue
        if not (kind.startswith("execute") or kind == "statement"):
            continue
        want = intended_ctx(text, rec.get("detail", ""))
        if want is None:
            continue
        got = CTX_RE.search(text)
        e = dict(kind=kind, name=name, pid=pid, app=rec.get("application_name", ""),
                 got=got and got.group(1), want=want,
                 msg=rec["message"], detail=rec.get("detail", ""))
        if kind == "statement":
            e["cls"] = "simple"
        elif name == UNNAMED:
            e["cls"] = "unnamed"
            e["reparsed"] = last_unnamed.pop(pid, None) == text
        else:
            e["cls"] = "named"
            e["hit"] = not since_exec.get((pid, name), False)
            since_exec[(pid, name)] = False
        executions.append(e)

    named_keys = {(e["pid"], e["name"]) for e in executions if e["cls"] == "named"}
    apps_per_key = {}
    for e in executions:
        if e["cls"] == "named":
            apps_per_key.setdefault((e["pid"], e["name"]), set()).add(e["app"])
    unnamed = [e for e in executions if e["cls"] == "unnamed"]
    metrics = {
        "exec": len(executions),
        "stale": sum(1 for e in executions if e["got"] and e["got"] != e["want"]),
        "uncommented": sum(1 for e in executions if not e["got"]),
        "named": len(named_keys),
        "named_exec": sum(1 for e in executions if e["cls"] == "named"),
        "named_parses": sum(parses.get(k, 0) for k in named_keys),
        "named_hits": sum(1 for e in executions if e.get("hit")),
        "unnamed_exec": len(unnamed),
        "unnamed_reparsed": sum(1 for e in unnamed if e["reparsed"]),
        "unnamed_reused": sum(1 for e in unnamed if not e["reparsed"]),
        "simple_exec": sum(1 for e in executions if e["cls"] == "simple"),
        "shared": sum(1 for k in named_keys
                      if parses.get(k, 0) == 1 and len(apps_per_key[k]) >= 2),
        "pids": len({e["pid"] for e in executions}),
        "apps": len({e["app"] for e in executions}),
    }
    return executions, metrics


def verdict_of(executions, metrics):
    if metrics["stale"]:
        return "stale"
    if metrics["uncommented"] == metrics["exec"]:
        return "uncommented"
    if metrics["uncommented"]:
        return "partial"
    return "fresh"


def parse_invariants(spec):
    inv = {}
    for item in ([] if spec.strip() == "-" else spec.split()):
        k, v = item.split("=")
        inv[k] = int(v)
    return inv


def check(records, scenario, expect, inv, quiet=False):
    """Return (exit_code, list of failure strings)."""
    executions, metrics = analyze(records)
    out = print if not quiet else (lambda *a, **k: None)
    contexts = {e["want"] for e in executions}
    out(f"== {scenario}")
    out("   metrics: " + " ".join(f"{k}={v}" for k, v in metrics.items()))
    names = sorted({e["name"] for e in executions})
    out(f"   statement names: {', '.join(names[:6])}{' ...' if len(names) > 6 else ''}")
    stale = [e for e in executions if e["got"] and e["got"] != e["want"]]
    nocmt = [e for e in executions if not e["got"]]
    for e in (stale[:2] or nocmt[:2] or executions[-2:]):
        tag = "STALE" if e in stale else "NOCMT" if e in nocmt else "ok   "
        out(f"   {tag} want={e['want']} got={e['got']} app={e['app']}: {e['msg'][:170]}"
            + (f" | {e['detail']}" if e["detail"] else ""))

    if len(executions) < MIN_EXECUTIONS or len(contexts) < MIN_CONTEXTS:
        out(f"   VACUOUS: need >= {MIN_EXECUTIONS} attributable executions over "
            f">= {MIN_CONTEXTS} contexts")
        return 2, ["vacuous"]
    failures = []
    verdict = verdict_of(executions, metrics)
    if verdict != expect:
        failures.append(f"verdict={verdict} expected={expect}")
    for k, v in inv.items():
        if k not in metrics:
            failures.append(f"unknown invariant {k}")
        elif metrics[k] != v:
            failures.append(f"{k}={metrics[k]} expected {v}")
    out(f"   verdict={verdict} expected={expect}; invariants "
        f"{'hold' if not any('expected ' in f for f in failures) else 'VIOLATED'}"
        f" -> {'PASS' if not failures else 'FAIL: ' + '; '.join(failures)}")
    return (0 if not failures else 1), failures


# --- mutations for --selftest ------------------------------------------------

def _kind(rec):
    p = parse_msg(rec)
    return p and p[0]


def m_strip_parses(rs):
    return [r for r in rs if _kind(r) != "parse"]


def m_drop_named_execs(rs):
    out = []
    for r in rs:
        p = parse_msg(r)
        if p and p[0].startswith("execute") and p[1] != UNNAMED:
            continue
        out.append(r)
    return out


def m_unname_all(rs):
    rs = copy.deepcopy(rs)
    for r in rs:
        m = MSG_RE.match(r.get("message", ""))
        if m and m.group("kind") != "statement" and parse_msg(r):
            r["message"] = f"{m.group('prefix')}{m.group('kind')} {UNNAMED}: {m.group('text')}"
    return rs


def m_one_client(rs):
    rs = copy.deepcopy(rs)
    for r in rs:
        if "application_name" in r:
            r["application_name"] = "single"
    return rs


def m_truncate(rs):
    idx = [i for i, r in enumerate(rs)
           if _kind(r) and (_kind(r).startswith("execute") or _kind(r) == "statement")
           and intended_ctx(parse_msg(r)[2], r.get("detail", ""))]
    drop = set(idx[-3:])
    return [r for i, r in enumerate(rs) if i not in drop]


def m_inject_stale(rs):
    rs = copy.deepcopy(rs)
    for r in rs:
        p = parse_msg(r)
        if p and (p[0].startswith("execute") or p[0] == "statement") and CTX_RE.search(p[2]) \
                and intended_ctx(p[2], r.get("detail", "")):
            got = CTX_RE.search(p[2]).group(1)
            other = "c1" if got != "c1" else "c2"
            r["message"] = r["message"].replace(f"ctx={got}", f"ctx={other}") \
                .replace(f"ctx:{got}", f"ctx:{other}").replace(f"ctx='{got}'", f"ctx='{other}'")
            return rs
    return rs


def m_reviewer(rs):
    """Review round 1: keep only unnamed executions, no Parses at all."""
    return m_drop_named_execs(m_strip_parses(rs))


MUTATIONS = [m_strip_parses, m_drop_named_execs, m_reviewer, m_unname_all,
             m_one_client, m_truncate, m_inject_stale]


def selftest(records, scenario, expect, inv):
    code, _ = check(records, scenario, expect, inv, quiet=True)
    if code != 0:
        print(f"   selftest: real log does not pass (exit {code})")
        return 1
    bad = []
    results = []
    for mut in MUTATIONS:
        mutated = mut(records)
        # n/a: nothing to mutate (e.g. no Parses in simple protocol, one client)
        if mutated == records or (mut is m_one_client and inv.get("apps", 0) <= 1):
            results.append(f"{mut.__name__[2:]}=n/a")
            continue
        code, why = check(mutated, scenario, expect, inv, quiet=True)
        if code == 0:
            bad.append(mut.__name__[2:])
            results.append(f"{mut.__name__[2:]}=PASSED(!)")
        else:
            results.append(f"{mut.__name__[2:]}=caught({why[0].split(' expected')[0]})")
    print("   selftest: " + " ".join(results))
    if bad:
        print(f"   selftest FAIL: mutations not detected: {', '.join(bad)}")
        return 1
    return 0


def main():
    args = sys.argv[1:]
    self_test = bool(args) and args[0] == "--selftest"
    if self_test:
        args = args[1:]
    if len(args) != 3 or args[1] not in VERDICTS:
        sys.exit(__doc__)
    scenario, expect, inv = args[0], args[1], parse_invariants(args[2])
    records = [json.loads(l) for l in sys.stdin if l.strip().startswith("{")]
    if self_test:
        sys.exit(selftest(records, scenario, expect, inv))
    code, _ = check(records, scenario, expect, inv)
    sys.exit(code)


if __name__ == "__main__":
    main()
