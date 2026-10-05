# Driver prepared-statement behavior vs. SQL comment context

Research for backlog item **20261005-091225-27** (DESIGN.md §6.3). The question:
does a target driver **reuse one server-side prepared statement across different
SQL-comment contexts**? If it does, the comment saved at Parse time would be
stale for later executions, and `pg_stat_statement_context` would attribute them
to the wrong tags. That outcome would move `tags_override`
(20261005-091225-30) into v1.

**Result: no target driver does this.** Every driver keys its statement cache
on the full SQL text, comment included, or re-Parses on every call. Native Rails
`query_log_tags` (ActiveRecord ≥ 7.1) goes further and turns prepared
statements off. Stale context only shows up when the *application* holds on to
one prepared handle and reuses it across requests. The two control scenarios
reproduce that case.
**Recommendation: NO-GO** on moving `tags_override` into v1 (see
[Decision](#go--no-go-tags_override-20261005-091225-30)).

## How to re-check

```sh
research/driver-prepared-statements/run.sh                # all 24 scenarios + mutation self-tests, exit 0 iff all match
research/driver-prepared-statements/run.sh pgx_ jdbc_t1   # name-prefix filter
FLIP=1 research/driver-prepared-statements/run.sh         # inverted verdicts: must exit 1, every scenario FAIL
```

You only need Docker on the host. Everything runs in `--rm` containers on a
private network that is removed at exit. The runner uses these local images:
`postgres:17`, `golang:1.27`, and `ruby:3.4`. Packages are installed inside the
containers via apt/pip/gem/go and Maven Central, and download caches plus the raw
server logs go to `<repo>/tmp/driver-prepared-statements/`. A full run takes
about 10–12 min.

### Method

- **Fresh server per scenario.** Each scenario starts a new PostgreSQL 17.11
  server with `log_min_duration_statement = 0`, `log_parameter_max_length = -1`,
  and `log_destination = jsonlog`. With these settings the server logs every
  **parse / bind / execute** separately, with the statement name, the statement's
  *saved source text*, and the bind parameters. The extension isn't installed.
- **Same workload for every client.** Each client makes 30 calls: 7×`c0`, 7×`c1`,
  7×`c2`, then `c0,c1,c2` interleaved 3 times. That's enough to cross every
  driver's prepare threshold before the context changes. Each call puts the
  context in a comment (`/*ctx=cN*/`, or the ORM's format) **and** sends the same
  `cN` as a bind parameter. Each client connection sets
  `application_name = repro-<i>`, so the log can tell clients apart, even behind
  pgbouncer.
- **The checker.** [`lib/check_log.py`](lib/check_log.py) looks at each logged
  `execute <name>: <text>` (or simple-protocol `statement:`) and compares the
  ctx in the executed source text, which is what the extension would see, with
  the parameter, which is what the caller intended. It assigns one of four
  verdicts:
  - `fresh`: every execution matches.
  - `stale`: at least one execution ran with another call's comment.
  - `uncommented`: no comment reached the server.
  - `partial`: some executions had a comment and some didn't.

  It exits non-zero if the verdict differs from the one recorded in
  [`expectations.tsv`](expectations.tsv). It also exits non-zero (as vacuous)
  if it sees fewer than 10 attributable executions or fewer than 3 contexts.
- **Prepared-state invariants.** A `fresh` verdict alone doesn't prove the
  driver's cache was exercised. If the log held only pre-threshold unnamed
  executions, it would still pass. So the checker also computes the metrics
  below, and `expectations.tsv` asserts **every one exactly** for every
  scenario:

  | Metric | What it counts |
  |---|---|
  | `exec` | Attributable executions; asserting it proves the complete workload ran. |
  | `named` | Distinct named statements, per (backend pid, name). |
  | `named_exec` | Executions of named statements. |
  | `named_parses` | Parse messages for those named statements. `named_parses == named == #contexts` means one Parse per context. |
  | `named_hits` | Executions of a named statement with no new Parse since its previous execution: real cache hits, where the server reused the saved source text. |
  | `unnamed_exec` | Executions of the unnamed statement. |
  | `unnamed_reparsed` | Unnamed executions immediately preceded, on the same backend, by a Parse of identical text. |
  | `unnamed_reused` | Unnamed executions without that fresh Parse. |
  | `simple_exec` | Simple-protocol executions. |
  | `shared` | Named statements Parsed once on a server backend and executed under ≥ 2 distinct `application_name`s, i.e. pgbouncer sharing one server statement between both clients. |
  | `pids` | Distinct server backends among executions. |
  | `apps` | Distinct `application_name`s among executions. |
  | `stale`, `uncommented` | Counts behind the verdict. |

  So, for example, the threshold crossing is asserted as "named statements
  exist, with hits", and pgbouncer sharing as `pids=1 apps=2 shared=3`.
- **Mutation self-test** (`check_log.py --selftest`, run by `run.sh` after
  every scenario). The real log must pass, and each applicable mutation of it
  must FAIL:
  - `strip_parses`: remove every Parse.
  - `drop_named_execs`: remove every named execution.
  - `reviewer`: both at once, leaving only unnamed executions and no Parses.
    That's the review-round-1 counterexample.
  - `unname_all`: rename every statement to `<unnamed>`.
  - `one_client`: collapse all `application_name`s into one.
  - `truncate`: drop the last 3 executions.
  - `inject_stale`: rewrite one executed comment.

  A mutation is `n/a` only when it changes nothing in that scenario, for
  example no Parses exist in simple protocol, or there's only one client. On
  the final run, every applicable mutation was caught in all 24 scenarios. For
  example, `jdbc_default` reports `strip_parses=caught(named_parses=0)
  reviewer=caught(exec=12)`, and `pgbouncer_mps_jdbc` reports
  `one_client=caught(shared=0)`.
- **Showing the assertions can fail.**
  - `FLIP=1` inverts every verdict. All 24 scenarios then FAIL and the runner
    exits 1 (verified).
  - It also happened for real: my first guess for `rails_marginalia_ar72` was
    `uncommented`, and the run failed with `verdict=fresh expected=uncommented
    -> FAIL`. The same happened for `rails_marginalia_ar80`
    (`verdict=uncommented expected=fresh -> FAIL`) before I corrected both
    expectations to the observed behavior.

## Versions tested (2026-10-05)

| Component | Version |
|---|---|
| PostgreSQL server | 17.11 (Debian 17.11-1.pgdg13+2, image `postgres:17`) |
| Go / pgx | go1.27.1, `github.com/jackc/pgx/v5` **v5.11.0** ([`pgx/go.mod`](pgx/go.mod), `go.sum`) |
| JDBC | pgjdbc **42.7.13** (Maven Central), OpenJDK 21.0.12.1 |
| psycopg | **3.3.6** (`psycopg[binary]`, bundled libpq 18.6), Python 3.13 |
| Rails | **activerecord 8.1.4** (and 7.0.8.7), pg gem **1.7.0** (libpq 18.6), Ruby 3.4.11; marginalia **1.11.1** with activerecord 7.0.8.7 / 7.2.3.2 / 8.0.5.1 / 8.1.4 |
| pgbouncer | **1.26.0** (PGDG `1.26.0-4.pgdg13+1`), `pool_mode = transaction` |

## Findings

All 24 scenarios match `expectations.tsv` (`scenarios run: 24, failed: 0`,
including the mutation self-tests). The table abbreviates the asserted
invariants; the full values are in `expectations.tsv`.

| Scenario | Verdict | exec | named / parses / hits | unnamed (re-Parsed) | other |
|---|---|---|---|---|---|
| `pgx_default` (CacheStatement) | fresh | 30 | 3 / 3 / 27 | 0 | |
| `pgx_cache_describe`, `pgx_describe_exec`, `pgx_exec` | fresh | 30 | 0 | 30 (30) | |
| `pgx_simple` | fresh | 30 | 0 | 0 | simple 30 |
| **`pgx_app_prepared` (control)** | **stale** (20) | 30 | 1 / 1 / 29 | 0 | |
| `jdbc_default` (prepareThreshold=5) | fresh | 30 | 3 / 3 / 15 | 12 (12) | |
| `jdbc_t1` (prepareThreshold=1) | fresh | 30 | 3 / 3 / 27 | 0 | |
| **`jdbc_t1_reused_ps` (control)** | **stale** (20) | 30 | 1 / 1 / 29 | 0 | |
| `psycopg_default` (prepare_threshold=5) | fresh | 30 | 3 / 3 / 12 | 15 (15) | |
| `psycopg_t0` (prepare_threshold=0) | fresh | 30 | 3 / 3 / 27 | 0 | |
| `psycopg_none` (prepare_threshold=None) | fresh | 30 | 0 | 30 (30) | |
| **`rails_query_log_tags`** (AR 8.1, native railtie setup) | fresh | 60 | 0 | 0 | simple 60 (prepared statements disabled by Rails) |
| `rails_query_log_tags_ar70` (AR 7.0, native) | fresh | 60 | 6 / 6 / 54 | 0 | |
| `rails_query_log_tags_prepared_override` (*override experiment*) | fresh | 60 | 6 / 6 / 54 | 0 | |
| `rails_marginalia_ar70`, `_ar72` (ORM path) | fresh | 60 | 6 / 6 / 54 | 0 | |
| `rails_marginalia_ar80`, `rails_marginalia` (AR 8.1) (ORM path) | **uncommented** | 60 | 2 / 2 / 58 | 0 | no comment on `pick`/`find_by` |
| `rails_marginalia_exec_query` (AR 8.1, `exec_query(prepare: true)`) | fresh | 30 | 3 / 3 / 27 | 0 | |
| `pgbouncer_mps_{pgx,psycopg,jdbc}` (max_prepared_statements=200) | fresh | 30 | 3 / 3 / 27 | 0 | pids=1 apps=2 **shared=3** |
| `pgbouncer_nomps_pgx` (max_prepared_statements=0) | fresh | 30 | 3 / 3 / 27 | 0 | |

`unnamed_reused` is 0 in every scenario: every unnamed execution was preceded
by a fresh Parse of the same text.

### pgx v5.11.0: no reuse across contexts

- **Default mode (`QueryExecModeCacheStatement`).** pgx keeps a per-connection
  LRU cache keyed by the SQL string. The statement name is
  `stmtcache_<sha256(sql)>`, so each distinct comment gets its own named
  statement:
  ```
  parse   stmtcache_50750ae3…: SELECT $1::text AS want /*ctx=c0*/
  execute stmtcache_50750ae3…: SELECT $1::text AS want /*ctx=c0*/ | Parameters: $1 = 'c0'
  parse   stmtcache_cdbcc9c3…: SELECT $1::text AS want /*ctx=c1*/
  execute stmtcache_cdbcc9c3…: SELECT $1::text AS want /*ctx=c1*/ | Parameters: $1 = 'c1'
  ```
- **Other modes.** `CacheDescribe`, `DescribeExec`, and `Exec` send an unnamed
  Parse on every call. `SimpleProtocol` interpolates on the client. All are fresh.
- **Control.** An application that calls `conn.Prepare(ctx, "app_stmt", sql)`
  once and then executes `"app_stmt"` from every context is stale. That is the
  documented §6.3 limitation, not a driver behavior:
  ```
  execute app_stmt: SELECT $1::text AS want /*ctx=c0*/ | Parameters: $1 = 'c1'   <- STALE
  ```

### pgjdbc 42.7.13: no reuse across contexts

- **Server-side cache.** The server-prepared statement cache
  (`preparedStatementCacheQueries`) is keyed by the SQL string. The
  `prepareThreshold` counter is per SQL string too. With the default of 5, the
  first 4 uses of each text get an unnamed Parse, and later uses go to a named
  `S_n` for that text:
  ```
  parse   <unnamed>: SELECT $1::text AS want /*ctx=c0*/      (uses 1-4)
  parse   S_1: SELECT $1::text AS want /*ctx=c0*/            (use 5)
  parse   <unnamed>: SELECT $1::text AS want /*ctx=c1*/      (new text: counter restarts)
  execute S_2: SELECT $1::text AS want /*ctx=c1*/ | Parameters: $1 = 'c1'
  ```
- **`prepareThreshold=1`.** It's the same picture: one `S_n` per distinct
  comment, starting from the first use.
- **Control.** Keeping a single `PreparedStatement` object and re-binding it for
  other requests is stale (`execute S_1: …ctx=c0… | $1 = 'c1'`). That's
  inherent: the object's SQL text is fixed when it's created.

### psycopg 3.3.6: no reuse across contexts

- **Cache key.** The prepared-statement cache is keyed by the query bytes, comment
  included. The statement is prepared as `_pg3_n` once that query has run
  `prepare_threshold` times. Each comment variant has its own counter and its own
  `_pg3_n`.
- **Thresholds.** `prepare_threshold=0` prepares on first use, one `_pg3_n` per
  comment. `None` sends an unnamed Parse every time.

### Rails / ActiveRecord with the pg gem: no reuse

**query_log_tags, native Rails behavior.** In ActiveRecord 7.1 and later,
enabling `config.active_record.query_log_tags_enabled` **turns prepared
statements off**. The railtie's `active_record.query_log_tags_config`
initializer sets `ActiveRecord.disable_prepared_statements = true`: in
activerecord 8.1.4 that's `railtie.rb:402`, and the same line is present in
7.1.6, 7.2.3.2 and 8.0.5.1 but absent in 7.0.8.7. The adapter then reports
`prepared_statements=false` even though the PostgreSQL adapter default is true.

[`rails/query_log_tags.rb`](rails/query_log_tags.rb) with `QLT_MODE=native`
reads the installed `railtie.rb` and applies the same settings. With AR 8.1.4,
values are interpolated and every query is a simple-protocol statement carrying
its own comment (60/60 `simple_exec`, no Parse/named statements):
```
statement: SELECT "widgets"."tag" FROM "widgets" WHERE "widgets"."tag" = 'c1' LIMIT 1 /*ctx='c1'*/
```
So with native Rails ≥ 7.1 query_log_tags, there's no prepared statement that
could carry a stale comment.

**AR 7.0 native** (`rails_query_log_tags_ar70`). Here prepared statements stay
on. The comment is added in the adapter's query preprocessing *before* the
`StatementPool` lookup, so the pool key (`sql_key(sql)`) includes it. The
result is fresh, with one `aN` per query shape × context: 6 named statements,
6 Parses, 54 hits.

**Override experiment, not native Rails**
(`rails_query_log_tags_prepared_override`). AR 8.1.4 with query_log_tags and
`ActiveRecord.disable_prepared_statements` forced back to false behaves like
AR 7.0. It's fresh, with 6 statements, `a1..a6`:
```
parse   a1: SELECT "widgets"."tag" FROM "widgets" WHERE "widgets"."tag" = $1 LIMIT $2 /*ctx='c0'*/
execute a4: SELECT "widgets".* FROM "widgets" WHERE "widgets"."tag" = $1 LIMIT $2 /*ctx='c1'*/ | Parameters: $1 = 'c1', $2 = '1'
```

**marginalia 1.11.1** (`prepared_statements: true`; marginalia doesn't touch
`disable_prepared_statements`). marginalia aliases adapter methods:
`execute_and_clear` on PostgreSQL when that private method exists, otherwise
`exec_query`/`execute`. It annotates the SQL before it reaches the
`StatementPool`, so wherever the hook *is* on the query path, the pool key
includes the comment:
- **AR 7.0 and 7.2, ORM path** (`where(...).pick`, `find_by`): fresh, 6
  statements per 2 shapes × 3 contexts.
- **AR 8.0 and 8.1, ORM path:** the integration is **incomplete**. These
  queries no longer pass through any method marginalia aliases. They run as
  named prepared statements (`a1`, `a2`) **without any comment** (60/60
  uncommented). There's no stale context, but also no context.
- **AR 8.1, `connection.exec_query(sql, "SQL", binds, prepare: true)`:** this
  path still goes through marginalia's `exec_query` alias, so it gets
  `/*ctx:cN*/` and one named statement per context. It's fresh: 3 statements,
  3 Parses, 27 hits. Note that marginalia's `exec_query` wrapper sets
  `prepare ||= false`, so only callers that explicitly pass `prepare: true` are
  prepared there.

On Rails ≥ 7.1, use `query_log_tags`. That's the built-in successor to
marginalia (since 7.0), and marginalia 1.11.1 is its last release.

### pgbouncer 1.26.0, transaction mode: no reuse across contexts

- **`max_prepared_statements = 200`, `default_pool_size = 1`.** Two client
  connections (`repro-0`, `repro-1`) share one server connection. pgbouncer rewrites client statement
  names to `PGBOUNCER_n` and shares server statements between clients **by
  query text**, comment included. pgx, psycopg (`prepare_threshold=0`), and
  pgjdbc (`prepareThreshold=1`) each got exactly one server Parse per comment
  variant, shared by both clients. The invariants are asserted as
  `pids=1 apps=2 named=3 named_parses=3 shared=3`: each `PGBOUNCER_n` was
  executed under both `application_name`s, which pgbouncer re-sends when it
  switches clients:
  ```
  parse   PGBOUNCER_1: SELECT $1::text AS want /*ctx=c0*/
  parse   PGBOUNCER_2: SELECT $1::text AS want /*ctx=c1*/
  execute PGBOUNCER_2: SELECT $1::text AS want /*ctx=c1*/ | Parameters: $1 = 'c1'
  ```
- **`max_prepared_statements = 0`.** pgbouncer passes the driver's protocol
  through unchanged, so the driver's behavior holds (pgx `stmtcache_*`). This
  scenario used a single client, because named statements across multiple
  clients aren't supported in transaction mode without `max_prepared_statements`.
  That restriction is a pgbouncer limit unrelated to comment context.

## GO / NO-GO: `tags_override` (20261005-091225-30)

**Recommendation: NO-GO.** Keep `tags_override` on the v2 roadmap rather than
pulling it into v1.

Reasoning:
1. **The §6.3 trigger isn't met.** The trigger is "if they reuse prepared plans
   across contexts", and none of the five targets does that in any tested
   configuration. That covers 24 scenarios, including each driver's default
   and its most aggressive prepare setting, and the asserted invariants show
   the caches really were exercised (named statements, cache hits, pgbouncer
   cross-client sharing). Every driver and pgbouncer either keys its cache on
   the full SQL text, comment included, or re-Parses on every call.
2. **Native Rails needs nothing.** With query_log_tags on AR ≥ 7.1, Rails
   disables prepared statements altogether, so it can't have stale comments.
3. **Only application-held handles go stale.** That's `pgx_app_prepared`,
   `jdbc_t1_reused_ps`, and SQL-level `PREPARE`/`EXECUTE`, which is already the
   documented §6.3 limitation.
4. **marginalia on AR 8.x is a coverage gap, not a staleness problem.** Model
   queries carry no comment at all. Those queries are still correctly
   attributed (they just have no tags), and the fix is to use query_log_tags.
5. **The remaining use cases are what §8 v2 already describes `tags_override`
   for.** They are application-held prepared handles and drivers or ORMs that
   can't add comments. Neither one makes v1 attribution wrong for
   comment-based instrumentation through these drivers.

This item doesn't change any docs. Things the user documentation item
(20261005-091225-28) should cover:
- **Comment cardinality costs prepared statements.** Where prepared statements
  are used, the comment is part of the cache key. So every distinct comment
  value creates its own server-side prepared statement per query shape. That's
  observed for pgx, pgjdbc, psycopg, AR 7.0 and prepared-override Rails, and
  pgbouncer. High-cardinality values such as request IDs or `traceparent` in
  the comment defeat statement caching and churn the caches:
  - pgx LRU,
  - pgjdbc `preparedStatementCacheQueries`,
  - psycopg `prepared_max`,
  - Rails `statement_limit`,
  - pgbouncer `max_prepared_statements`.

  Recommend low-cardinality tags (controller/action/route/job).
- **Rails ≥ 7.1 `query_log_tags` disables prepared statements.** That's Rails'
  own trade-off, worth mentioning because it changes protocol and plan-caching
  behavior.
- **marginalia 1.11.1 on Rails ≥ 8.0 doesn't annotate ordinary model queries.**
  `where`/`pick`/`find_by` have no comment; only paths through the aliased
  adapter methods (e.g. `exec_query`) do. Use `query_log_tags` instead.
- **Application-level prepared-statement reuse stays stale** (unchanged §6.3
  text).

## Not verified / limits

- **Server version.** Only PostgreSQL 17 was used as the server. Drivers decide
  whether to re-Parse on the client side, and the server's
  parse/bind/execute logging is the same in 14–18, so I don't expect differences.
  I didn't run the scenarios on other server versions.
- **Configurations not covered:**
  - pgjdbc `preferQueryMode` values other than the default (`extended`).
  - pgx through `pgxpool`. The cache is per connection, same as here.
  - psycopg pipeline mode and `executemany`.
  - Rails `cache_query_log_tags = true`. It caches the *comment string* per
    execution context, which is an application-side caching choice and not
    prepared-statement reuse.
  - marginalia on AR 8.x beyond the two tested paths (ORM `pick`/`find_by`,
    and `exec_query(prepare: true)`), e.g. `execute`, `insert`, `update`.
  - AR 7.1 wasn't run. Only its `railtie.rb` was inspected, and it contains
    the `disable_prepared_statements` line.
  - Rails with `prepared_statements: false`. Values are interpolated, so it's
    trivially fresh.
- **Not run in a full app.** Rails was exercised through ActiveRecord with the
  same initializer steps the railties perform (for query_log_tags, derived from
  the installed `railtie.rb`), not inside a generated Rails app with
  controllers. The context came from a custom tag/component (`ctx`), not from
  `controller`/`action`, which take the same code path.
- **Unpinned base images.** Images were the ones present locally (`postgres:17`,
  `golang:1.27`, `ruby:3.4`), not pinned by digest. Driver versions are pinned
  in `run.sh`, `pgx/go.mod`, and the Ruby `gemfile` blocks.

## Files

- `run.sh`: the runner (Docker only): per-scenario Postgres, optional pgbouncer,
  the driver container, and the checker.
- `expectations.tsv`: the expected verdict and exact invariants per scenario.
  These are the findings above.
- `lib/check_log.py`: the jsonlog parser, the assertion, and `--selftest`
  (mutation self-test).
- `pgx/main.go` (+ `go.mod`, `go.sum`): pgx client, `-mode` and `-clients`.
- `jdbc/Repro.java`: pgjdbc client, run with the JDK single-file source launcher.
- `psycopg/repro.py`: psycopg 3 client.
- `rails/query_log_tags.rb`, `rails/marginalia.rb`: ActiveRecord via
  `bundler/inline`. `AR_VERSION` selects the version; `QLT_MODE`
  (native/prepared_override) and `MARG_PATH` (orm/exec_query) select the
  variant.
