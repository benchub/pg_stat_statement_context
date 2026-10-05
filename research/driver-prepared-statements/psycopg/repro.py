"""Repro client for psycopg 3: does the comment seen at Parse time go stale?

Usage: repro.py <prepare_threshold: default|none|N> [clients]   (conninfo from $DSN)

Each call runs  SELECT %s::text AS want /*ctx=cN*/  with the parameter 'cN'.
psycopg keeps a per-connection cache keyed by the query bytes and prepares a
named statement (_pg3_N) once a query has run `prepare_threshold` times
(default 5; 0 = prepare on first use; None = never prepare).
"""
import os
import sys

import psycopg


def workload():
    w = [f"c{c}" for c in range(3) for _ in range(7)]
    return w + [f"c{i % 3}" for i in range(9)]


def main():
    threshold = sys.argv[1]
    clients = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    conns = [psycopg.connect(os.environ["DSN"], autocommit=True, application_name=f"repro-{i}")
             for i in range(clients)]
    for c in conns:
        if threshold == "none":
            c.prepare_threshold = None
        elif threshold != "default":
            c.prepare_threshold = int(threshold)
    print(f"psycopg {psycopg.__version__} impl={psycopg.pq.__impl__} "
          f"libpq={psycopg.pq.version()} prepare_threshold={conns[0].prepare_threshold} "
          f"clients={clients}")

    w = workload()
    for i, want in enumerate(w):
        c = conns[i % clients]
        sql = "SELECT %s::text AS want /*ctx=" + want + "*/"
        got = c.execute(sql, (want,)).fetchone()[0]
        assert got == want, (got, want)
    print(f"done: {len(w)} calls")


if __name__ == "__main__":
    main()
