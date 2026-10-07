# pg_stat_statement_context — Backlog

This backlog breaks [DESIGN.md](DESIGN.md) (Draft) into concrete, implementable
tasks. Section references (§N) point to DESIGN.md.

## How to read this backlog

**Task IDs** use the format `YYYYMMDD-HHMMSS-N`. The `YYYYMMDD-HHMMSS` part is
the local time at which a batch of tasks was written, and `N` numbers the tasks
in that batch sequentially from 1. Every task in this file shares the prefix
`20261005-091225`. To add tasks, use your own current timestamp as the prefix
and start again at 1, so that people adding tasks at the same time don't create
the same ID. Never renumber or reuse an ID.

**Fields** for each task:

- **Description**: what to build, scoped so that one engineer or agent can
  complete it, with references to DESIGN.md.
- **Acceptance criteria**: brief, testable conditions for done.
- **Depends on**: task IDs that must be finished first, or `none`.
- **Open questions**: undecided design questions that must be answered
  **before** the task can start, or `none`. These cover only things that
  DESIGN.md leaves open, ambiguous, or TBD. Smaller choices that an implementer
  can make on their own, then document and confirm in review, are listed as
  *Design notes* instead and don't block the task.
- **Status**:
  - `ready`: every dependency is done and there are no open questions.
  - `blocked-on-deps`: there are no open questions, but at least one dependency
    is unfinished.
  - `blocked-on-questions`: the task has open questions. This status takes
    precedence when a task also has unfinished dependencies, because the
    questions can be answered now, in parallel with the dependency work.
    Dependencies are still listed in the table.

The backlog has two parts: **v1** (tasks 1–29, plus later additions
20261005-101154-1 and 20261005-103941-1) and the **post-v1 roadmap** (tasks
30, 32–35, 38, 39, 41, 42, 45, and 46, §8). Roadmap tasks 31, 36, 37, 40, 43,
and 44 were dropped on 2026-10-05 as out of scope for a `pg_stat_statements`
companion; they are archived under "Dropped" in BACKLOG-COMPLETE.md. Any
roadmap task that adds SQL objects or columns must ship
an extension upgrade script (for example `--1.0--1.1.sql`) rather than edit the
frozen 1.0 script.

**Scope decision (2026-10-05):** the extension is a companion to
`pg_stat_statements`, not a replacement. Per (queryid × context) it stores only
`calls` and `total_exec_time`; every other statistic comes from pgss via a join
on `(userid, dbid, queryid, toplevel)` (DESIGN.md §5.1, §7).

## Summary

| ID | Title | Depends on | Has open questions | Status |
|----|-------|------------|--------------------|--------|
| 20261005-091225-29 | v1 release readiness | 20261005-091225-3, 20261005-091225-11, 20261005-091225-22, 20261005-091225-23, 20261005-091225-24, 20261005-091225-25, 20261005-091225-26, 20261005-091225-28, 20261005-213120-1, 20261006-010149-1, 20261005-091225-32 | no | ready |
| 20261005-091225-33 | Roadmap: exemplars for excluded high-cardinality keys | 20261005-091225-17, 20261005-091225-20 | no | ready |
| 20261005-091225-45 | Roadmap: distribution packaging and provider outreach | 20261005-091225-29 | no | blocked-on-deps |
| 20261005-091225-46 | Roadmap: upstream proposal for a statement-comment hook | 20261005-091225-26, 20261005-091225-29 | no | blocked-on-deps |

### How the DESIGN.md §11 open questions were resolved

All seven §11 questions were answered by the project owner on 2026-10-05.

| §11 question | Resolution | Recorded in task |
|--------------|------------|------------------|
| Q1: record the empty tag set by default? | No: `untagged = skip` is the default | 20261005-091225-29 |
| Q2: `jsonb` vs fixed columns for `tags` | `jsonb` | 20261005-091225-20 |
| Q3: DSL in one GUC vs a `config_file` | GUCs only; `ALTER SYSTEM` + `pg_reload_conf()` from SQL | 20261005-091225-31 (dropped), 20261005-091225-28 |
| Q4: is `utility_textid` worth it? | No; pgss has the same PG14/15 behavior | 20261005-091225-37 (dropped) |
| Q5: `bucket_id` in key vs per-entry bucket ring | Per-entry counter ring | 20261005-091225-13, -14, -15 |
| Q6: error-count dedup rules; cancellations separate? | Moot: error counts dropped | 20261005-091225-40 (dropped) |
| Q7: load-order violation: `WARNING` vs disable utility tracking | `WARNING` only | 20261005-091225-19 |

Other blocking questions came up while decomposing the design. Those on tasks
8, 9, 12, 21, 30, 32, 35, 38, and 41 were answered on 2026-10-05 (see each
task's **Decisions**). No task currently has open questions.

### Dependency overview (v1)

Short labels: `T1` = `20261005-091225-1`, and so on.

```mermaid
graph TD
  T1[1 skeleton] --> T2[2 compat.h]
  T1 --> T3[3 CI]
  T4[4 lexer] --> T5[5 positional scan]
  T1 --> T7[7 core GUCs]
  T2 --> T7
  T7 --> T8[8 DSL]
  T5 --> T9[9 tag pipeline]
  T6[6 pair parsers] --> T9
  T8 --> T9
  T8 --> T10[10 regex]
  T9 --> T10
  T9 --> T11[11 debug fn + regress]
  T10 --> T11
  T2 --> T12[12 counters]
  T2 --> T13[13 store core]
  T7 --> T13
  T12 --> T13
  T13 --> T14[14 buckets]
  T14 --> T15[15 eviction]
  T9 --> T16[16 frames]
  T12 --> T17[17 executor hooks]
  T14 --> T17
  T16 --> T17
  T17 --> T18[18 ProcessUtility]
  T18 --> T19[19 load order]
  T12 --> T20[20 SRF + views]
  T14 --> T20
  T15 --> T21[21 info/reset]
  T20 --> T21
  T18 --> T22[22 TAP lifecycle]
  T20 --> T22
  T17 --> T23[23 TAP store]
  T21 --> T23
  T17 --> T24[24 SQL iface tests]
  T21 --> T24
  T5 --> T25[25 fuzzing]
  T6 --> T25
  T11 --> T25
  T15 --> T26[26 benchmarks]
  T18 --> T26
  T20 --> T26
  T27[27 driver validation] --> T28[28 docs]
  T10 --> T28
  T19 --> T28
  T21 --> T28
  T3 --> T29[29 release]
  T11 --> T29
  T22 --> T29
  T23 --> T29
  T24 --> T29
  T25 --> T29
  T26 --> T29
  T28 --> T29
  T17 --> C1[compat shim trim]
```

`C1` = `20261005-103941-1`. No v1 task has open questions any more (so none is
marked `?`). Every dependency points to a lower-numbered task, or (for `C1`)
to an older task, so the graph is acyclic. 20261005-101154-1 has no
dependencies and is not shown.

---

## v1 tasks

### 20261005-091225-29: v1 release readiness

**Description:** Prepare and cut the v1.0 release:
- Confirm the defaults (including `untagged = skip`), then freeze `--1.0.sql`. Later changes go into upgrade scripts.
- Verify `make install` from a clean checkout on every CI cell, including the assert and Valgrind jobs.
- Check that the benchmark numbers are published.
- Check that DESIGN.md §11 records how each question was resolved (done on 2026-10-05) and that the release notes summarize them.
- Add a CHANGELOG, tag `v1.0.0`, and create a GitHub release.

**Acceptance criteria:**
- The checklist is complete, CI is green, the tag and release exist, and the README install steps work from a clean checkout.

**Decisions:**
- 2026-10-05 (§11 Q1): Untagged statements are **skipped** by default (`untagged = skip`); `untagged = record` stays available. The sizing guidance assumes this default.
- 2026-10-06 (Q1): Finish 20261005-213120-1 and 20261006-010149-1 before freezing `--1.0.sql`.
- 2026-10-06 (Q2): Moot; 20261006-043919-1 landed. 20261006-075124-1 is not a v1 blocker.
- 2026-10-06 (Q3): The **owner** pushes, tags `v1.0.0` and creates the GitHub release. The agent prepares everything locally (CHANGELOG, release notes, freeze) and stops before pushing.
- 2026-10-06: v1.0 ships the roadmap features already built: appname, normalize, activity view, tags_override, and per-key cardinality caps (20261005-091225-32).

**Progress (2026-10-06, agent):** everything up to the owner's steps is done:
- `untagged` defaults to `skip` (`src/guc.c`). `--1.0.sql` is frozen (header comment, `sql/frozen.sha256`, `scripts/check-frozen-sql.sh` in CI and `docker/run-tests.sh`; DESIGN.md §7).
- `CHANGELOG.md` and `docs/release-notes/v1.0.0.md` added; the notes summarize the §11 resolutions. §11 is accurate. Benchmark numbers are committed in `docs/benchmarks.md`.
- Local matrix in Docker: PGDG 14–18, `--assert` 14–18 and `--valgrind 18` all pass; README install + quick start verbatim from a fresh `git clone` on PG 14 and 18.
- Fixed on the way: `docker/Dockerfile.source` passed the release as `ARG PG_VERSION`, which the base image's `ENV PG_VERSION` overrides under the legacy builder (tarball 404); renamed to `PG_SOURCE_VERSION`.
- **Remaining (owner):** push `main`, wait for CI to be green, tag `v1.0.0`, create the GitHub release from `docs/release-notes/v1.0.0.md`, then run `scripts/backlog-complete.py 20261005-091225-29`.

**Depends on:** 20261005-091225-3, 20261005-091225-11, 20261005-091225-22, 20261005-091225-23, 20261005-091225-24, 20261005-091225-25, 20261005-091225-26, 20261005-091225-28, 20261005-213120-1, 20261006-010149-1, 20261005-091225-32
**Open questions:** none
**Status:** ready

---

## Post-v1 roadmap tasks (§8)

### 20261005-091225-33: Roadmap: exemplars for excluded high-cardinality keys

**Description:** Store the most recent value of explicitly listed high-cardinality keys (for example `traceparent`) per entry, so users can jump from an aggregate to a real trace (§8 v1.x). The visibility rules from §6.11 apply.

*Owner's note (2026-10-05):* the task is kept. An exemplar stores the most recent value of a high-cardinality key (e.g. `traceparent`) per entry.

**Acceptance criteria:**
- The exemplar column shows the latest value without adding new entries.
- It is `NULL` for unprivileged roles viewing other roles' rows.
- Only keys in the exemplar GUC are stored. Total exemplar memory never exceeds the configured cap.
- The upgrade script is provided.

**Depends on:** 20261005-091225-17, 20261005-091225-20
**Decisions (2026-10-05):**
- Exemplar keys come from an explicit list in a dedicated GUC (e.g. `pg_stat_statement_context.exemplar_keys`). The denylist (`exclude_tags`) does not double as the exemplar list. A key may need to be both denylisted (so it isn't grouped by) and listed as an exemplar.
- Exemplar storage has a memory cap set by a config value (a postmaster-level GUC, since it sizes shared memory). Values that would exceed the cap are truncated or dropped (implementer's choice, documented and counted in `_info()`).

**Decisions (implementation, 2026-10-07; DESIGN.md §4.1, §5.1, §6.11, §6.13, §7):**
- GUCs: `exemplar_keys` (postmaster, default `''` = off and no memory, at most 8 keys of at most 63 bytes, no `*`, case-sensitive, duplicates ignored) and `exemplar_memory` (postmaster, kB, default 2MB). `max_tag_value_len` is not reused: exemplars are a separate budget.
- Sizing: each entry gets `exemplar_memory / max_entries` bytes (rounded down to MAXALIGN), split evenly per key; a value may take that share minus a 2-byte length, at most 256 bytes. Total (`_info().exemplar_shmem_bytes`) ≤ `exemplar_memory`.
- Overflow: **dropped**, not truncated (a truncated trace id is useless); the slot keeps its previous value; counted in `_info().exemplar_values_dropped`.
- Capture: after the step-4 rename, before the step-5 allowlist/denylist, independently of it; per-extractor `keys` apply, normalize/truncation/caps don't. First occurrence in a statement wins (override > comments > footer > appname). Nested statements with `nested_tags = inherit` inherit the outer exemplars.
- Storage: fixed per-entry slots after the counter ring, in exemplar_keys order; written under the entry spinlock with the call (no table-lock upgrade). Not saved across restarts; cleared by `_reset()`.
- SQL: extension 1.1 adds `exemplars jsonb` as the last column of the SRF/`_last_bucket`/the three views and `exemplar_shmem_bytes`, `exemplar_value_bytes`, `exemplar_values_dropped` at the end of `_info()`, via `--1.0--1.1.sql` (drop and recreate, new `_1_1` C symbols; the 1.0 symbols stay). `exemplars` is `NULL` exactly when `tags` is.

**Open questions:** none

**Status:** ready

### 20261005-091225-45: Roadmap: distribution packaging and provider outreach

**Description:** Package and distribute the extension (§8 v3):
- PGXN `META.json` and an upload
- PGDG apt/yum packaging requests
- a Homebrew formula
- Docker images based on the official postgres images

Also contact the managed providers (RDS, Cloud SQL, Azure) about adding the extension to their allowlists (§6.12).

**Acceptance criteria:**
- Each package installs, and `CREATE EXTENSION` works.
- The outreach is logged, with contacts and status.

**Depends on:** 20261005-091225-29
**Open questions:** none
**Status:** blocked-on-deps

### 20261005-091225-46: Roadmap: upstream proposal for a statement-comment hook

**Description:** Write and send a pgsql-hackers proposal for a core hook or field for statement comments, or a query-tag mechanism, that would benefit pgss and similar extensions (§8 v3). Use the benchmark data and the limitations (§6.2, §6.3, §6.6) as motivation.

**Acceptance criteria:**
- The proposal is posted, and the thread link and outcome are recorded in the repository.

**Depends on:** 20261005-091225-26, 20261005-091225-29
**Open questions:** none
**Status:** blocked-on-deps
