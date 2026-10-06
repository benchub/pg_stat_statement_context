# Extractors: from SQL comments to tags

This page describes how the extension finds comments in a statement, how the
`pg_stat_statement_context.extractors` setting (the *extractor DSL*) turns them
into tags, and how to keep the number of distinct tag sets under control. The
[`appname`](#appname) extractor derives tags from `application_name` instead,
for clients that can't add comments.

Use `pg_stat_statement_context_extract()` (superuser-only by default, see
[the SQL interface](sql-interface.md#pg_stat_statement_context_extract)) to
check what the current configuration extracts from a statement. All examples
on this page use it.

## Where comments are found

The extension never parses SQL a second time. It runs a lexical scan of the
statement text, following PostgreSQL's own lexical rules: string literals
(including `E''`, `U&''`, and `standard_conforming_strings = off` escapes),
quoted identifiers, dollar quotes, nested `/* /* */ */` comments and `--` line
comments are all recognized, so comment-like text inside a string is not a
comment.

**Statement range.** In a multi-statement query string such as
`SELECT 1 /*a*/; SELECT 2 /*b*/`, each statement only sees the comments within
its own range. A comment after the final `;` belongs to no statement, with one
fallback: a statement whose own range yields no tags may use that trailing
comment, but only if nothing except `;`, whitespace and comments follows the
statement, which proves it is the last one. So in
`SELECT 1; SELECT 2; /*controller:x*/` only `SELECT 2` gets the tag.

**Position.** Each extractor looks at the comments in one place, set by its
`position` parameter:

| `position` | Comments examined |
|---|---|
| `append` | The trailing run: the last comment before the optional trailing `;` and whitespace, plus any block comments immediately before it separated only by whitespace. It must be a `/* */` block comment; a trailing `--` comment yields no tags. |
| `prepend` | The leading run: the comments (block or `--` line) before the first token, separated only by whitespace. |
| `any` | Every comment in the statement, wherever it is. |

A run (rather than a single comment) lets a context comment followed by a
free-text annotation, as marginalia writes it, still be found:

```sql
SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show*/ /*with_annotation free text*/') -> 'tags';
-- {"action": "show", "controller": "users"}
```

**Long statements and `scan_window`.** A statement no longer than
`scan_window` bytes (default 2 kB) is always lexed exactly. For a longer
statement (for example one with a 10,000-element `IN` list):

- `prepend` looks only at the first `scan_window` bytes and stays exact.
- `append` looks only at the last `scan_window` bytes. It cannot know the
  lexical state at the start of that window, so this path is **heuristic**:
  it walks backwards from the closing `*/` to the matching `/*`. A comment
  that crosses the window start yields no tags. Rarely, a string literal
  ending in `*/`, or a `--` line comment that began before the window, can
  make text that isn't a real comment look like one. Each use of this path is
  counted in `_info().heuristic_scans`.
- `any` always does a full forward scan of the statement, so its cost grows
  with the statement length.

At most 16 comments, and at most `scan_window` bytes of comment text, are
examined per statement.

## The extractor DSL

`pg_stat_statement_context.extractors` is a comma-separated list of
extractors, each with optional parameters:

```
extractor := name [ '(' param { ',' param } ')' ]
name      := sqlcommenter | marginalia | regex | appname
param     := key '=' value
```

The default is `'sqlcommenter, marginalia'`.

**Chain semantics.** Extractors run in list order. The first extractor that
*produces* wins, and later extractors are skipped, unless a later extractor
has `merge=on`, in which case it still runs and its tags are added. When the
same key comes from several places, the first occurrence wins: by extractor
order, then comment order, then pair order.

[`appname`](#appname) extractors form a **separate chain** over
`application_name`, with the same rules among themselves (the first one that
produces wins; later ones run only with `merge=on`). The two chains don't
affect each other: a comment extractor that produced doesn't stop an
`appname` extractor, and vice versa. Their tags are then combined, and on a
key conflict **the comment's value wins**, wherever the extractors appear in
the list. A comment pair that the pipeline drops (an invalid value, a failed
normalization, a key filtered out) doesn't block the `application_name` value
for that key.

The session's [`tags_override`](configuration.md#tags_override) tags are
added to both, and win every key conflict: **override > comment >
`application_name`**. They are not part of either chain (they don't stop
an extractor, and an override pair the pipeline drops blocks nothing).

An extractor *produces* when at least one of its tags is still a candidate
after [pipeline](#the-tag-pipeline) steps 1–7 (validation, the allowlists and
denylist, `rename`, normalization, truncation). That is decided **before** the
cardinality caps and the final `max_tags` / `max_tagset_bytes` limits (steps 8
and 9). A value a cap turns into `null` still counts as produced. If those limits then drop the
winner's tags, the skipped extractors are not tried again. The statement can
end up with fewer tags, or none at all. With `untagged = skip`, a statement
left with no tags is not recorded.

For example, with `max_tag_value_len = 1024` and `max_tagset_bytes = 128`, the
statement `SELECT 1 /*controller='xxx…'*/ /*action:show*/`, with a 200-byte
value, gets no tags at all (`dropped_tags` = 1). `sqlcommenter` wins with the
oversized `controller` tag, which step 9 then drops, so the `marginalia`
comment `action:show` is never used. To avoid this, keep `max_tagset_bytes`
comfortably above the largest possible tag set: the sum over your allowlisted
keys of `length(key) + max_tag_value_len + 2`. (The defaults give 217 bytes
for 3 keys, well within 512.) See
[`max_tagset_bytes`](configuration.md#max_tagset_bytes).

**Syntax.**

- Whitespace may surround every token. Extractor names, parameter names and
  keyword values (`append`, `on`, ...) are case-insensitive.
- An unquoted value runs up to whitespace, `,` or `)`, and may not contain a
  quote or `(`.
- A quoted value is `'...'`, with `''` standing for one quote, and is taken
  verbatim (it may contain spaces, commas and parentheses). Regex patterns
  almost always need quoting.
- Empty values are not allowed.
- Remember that the whole setting is itself a quoted string in SQL and in
  `postgresql.conf`, so each quote of a quoted value is doubled once more
  there: `'regex(pattern=''x=(\w+)'', keys=x)'`.

### Common parameters

| Parameter | Values | Meaning |
|---|---|---|
| `position` | `append`, `prepend`, `any` | Where to look (above). Default: `append` for `sqlcommenter` and `marginalia`, `any` for `regex`. Not accepted by `appname`. |
| `keys` | `a\|b\|c` | For `sqlcommenter` and `marginalia`: a per-extractor allowlist. It matches the **original** key names as written in the comment, and is applied **before** `rename`. For `regex`: the names of the capture groups (required). |
| `rename` | `old:new\|old2:new2` | Renames keys, for example to normalize `route` and `controller` from different formats to one key. |
| `merge` | `on`, `off` (default) | Add this extractor's tags to those of earlier extractors instead of being skipped once one has produced. |

### `sqlcommenter`

The [SQLCommenter] format: `/*key='value',key2='value2'*/`.

- Values must be single-quoted; a `,` inside quotes doesn't split.
- `\'` and `\\` are unescaped.
- With `url_decode=on` (the default), keys and values are URL-decoded: `%XX`
  is decoded and a raw `+` becomes a space, so form-encoding emitters (Go,
  Java) and `%20` emitters (Python, Node) give the same value; `%2B` is a
  literal `+`. An invalid `%` escape is kept literally. `url_decode=off`
  disables decoding.

| Parameter | Default |
|---|---|
| `url_decode` | `on` |

```sql
SELECT pg_stat_statement_context_extract(
         $$SELECT 1 /*controller='users%2Fshow',action='it\'s+here'*/$$) -> 'tags';
-- {"action": "it's here", "controller": "users/show"}
```

### `marginalia`

The [marginalia] / Rails `query_log_tags` legacy format:
`/*application:Foo,controller:users,action:show*/`.

- The comment is split into pairs on `pair_sep` first, then each pair on the
  **first** `kv_sep` only, because values such as `line:app/models/u.rb:12`
  contain colons.
- No decoding is done.

| Parameter | Default | Notes |
|---|---|---|
| `kv_sep` | `:` | 1–8 bytes; must not contain `pair_sep`. |
| `pair_sep` | `,` | 1–8 bytes. |

Separators that start with whitespace are allowed, for example `kv_sep=' :'`
(quote them).

### Rules for both `sqlcommenter` and `marginalia`

- ASCII whitespace is trimmed around the comment body, each pair, and each key
  and value. Empty segments are ignored.
- A segment without a separator, or whose key contains whitespace, is
  malformed and skipped. That keeps free-text annotations from turning into
  tags. It is counted in `_info().invalid_tags` only if the same comment also
  contained a well-formed pair for that extractor, so a comment in another
  extractor's format is not counted.

```sql
SELECT r -> 'tags' AS tags, r -> 'invalid_tags' AS invalid_tags
  FROM pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,free text,action:show*/') AS r;
-- tags: {"action": "show", "controller": "users"}, invalid_tags: 1
```

### `regex`

A custom format: `regex(pattern='...', keys='k1|k2', position=any)`.

- `pattern` (required) is a PostgreSQL advanced regular expression (the same
  engine as `~` and `regexp_matches`), evaluated with the C collation. It is
  applied **only to comment text**, never to the rest of the query.
- `keys` (required) names the capture groups: capture group *n* becomes key
  *n*. The number of keys must equal the number of capture groups.
- Every non-overlapping match in a comment is used, as with
  `regexp_matches(..., 'g')`; `^` anchors only at the start of the comment
  body. An optional group that didn't match produces no tag. The first value
  for a key wins.
- Limits: the pattern is at most 1 kB, has at most `max_tags` capture groups,
  may not use back-references, and must compile in at most **100 ms**.

Some short patterns take the regex engine seconds or minutes to compile,
for example a bounded repetition of a group that can match the empty string
(`((?:(?:$)|\Zda|(?<!1)|\S){0,255})`). The compile time limit keeps such a
pattern from stalling queries:

- When the value is set with `ALTER SYSTEM`, a pattern whose test compile
  takes longer than 100 ms is rejected (`DETAIL: Compiling the pattern of
  extractor "regex" took longer than 100 ms.`). `ALTER SYSTEM` stops the
  compile at the limit, so the value is never written.
- Values read from the configuration file (at startup, on reload and in
  `pg_file_settings`) are never rejected for compile time alone, so that
  every process ends up with the same configuration. The postmaster
  compiles the pattern to completion (a reload can take as long as that
  compile) and logs `compiling the pattern of extractor "regex" took N ms,
  longer than the 100 ms limit` if it was too slow; each backend checks the
  value again on reload, stops its compile at the limit and then accepts
  the value without the checks that need the compiled pattern. Each backend's own compile at run time (next
  bullet) then disables a pattern that is too slow. Parallel workers don't
  compile the patterns at all: they take the leader's value as it is and
  never extract tags.
- Each backend compiles the pattern again on its first tagged statement. In
  a client backend, a compile still running after 100 ms is stopped and
  counts as a compile failure (see below). The statement isn't cancelled
  and doesn't fail. Other processes (for example background workers) have
  no limit at run time.
- In a backend the limit is elapsed time. If a compile reaches it while
  having used less than half of it in CPU time, the backend was mostly
  waiting for the CPU (a loaded host), so the compile is retried, up to 3
  attempts in all, and a statement waits at most about 300 ms for a slow
  pattern. Inside a VM, time the host takes the virtual CPU away can count
  as CPU time: many hypervisors (Docker Desktop on macOS, for example)
  report no steal time to the guest, so nothing inside the VM can tell it
  apart. On a heavily overloaded host a normal pattern can therefore still
  be rejected by `ALTER SYSTEM` (try again) or disabled in a backend
  until the next configuration change.
- The limit is per pattern. Compile times vary between machines and with
  load, so a pattern close to the limit may be accepted when it's set and
  still be stopped in some backends. Keep patterns well below the limit:
  normal patterns compile in well under 1 ms.

If a regex fails to compile in a backend at run time (out of memory, or
over the compile time limit), that extractor is disabled in that backend
until the next configuration change, and the failure is counted in
`_info().regex_compile_failures`. The user's statement never fails. A
query cancel, `statement_timeout`, `transaction_timeout`, recovery conflict
or other interrupt that arrives during the compile is still handled as
usual.

### `appname`

`appname(format=sqlcommenter|marginalia|regex, ...)` derives tags from the
session's `application_name` instead of from comments, for drivers and tools
that can set `application_name` (e.g. `PGAPPNAME`, the `application_name`
connection parameter) but can't add comments to their queries.

- `format` (required) selects the parser. The whole `application_name` string
  is parsed as if it were a comment body, with that format's rules and
  parameters: `url_decode` for `sqlcommenter`; `kv_sep` and `pair_sep` for
  `marginalia`; `pattern` and `keys` (both required) for `regex`. A parameter
  of another format is rejected. `keys`, `rename` and `merge` work as for the
  other extractors; `position` is not accepted.
- The tags go through the same [pipeline](#the-tag-pipeline) (allowlists,
  `rename`, `normalize`, truncation, limits) as comment tags, and are combined
  with them as described in [chain semantics](#the-extractor-dsl): the
  comment wins a key conflict. A statement without any comment still gets the
  `application_name` tags, and counts as tagged for `untagged = skip`.
- Malformed segments are dropped and counted in `_info().invalid_tags` under
  the usual rule (only when the same value also contained a well-formed pair,
  so a plain name such as `psql` counts nothing); a value that decodes to a NUL
  byte or invalid text is always counted.
- The value used is `application_name` **when the statement starts
  executing**: a prepared statement uses the value at `EXECUTE` (or protocol Execute)
  time, not at `PREPARE`; a `SET application_name` statement itself is tagged
  with the previous value; `SET LOCAL` lasts until the end of the
  transaction. A nested statement with [`nested_tags`](configuration.md#nested_tags)
  `= inherit` gets the tags of its top-level statement, even if a function
  changed `application_name` in between; with `scan` it reads the value
  current when it starts.
- PostgreSQL replaces non-ASCII characters in `application_name` (with `?` on
  PostgreSQL 14 and 15, with `\xHH` escapes on 16 and later), so tag values
  derived from it are ASCII.
- Each backend caches the parsed result for the last `application_name` and
  configuration, so a statement normally costs one string comparison. The
  counters (`invalid_tags`, `normalized_tags`, ...) are added again for every
  statement, as if nothing were cached.
- `pg_stat_statement_context_extract()` uses the calling session's current
  `application_name`; its result has no separate field for these tags.
- For tags that a client sets explicitly for a session or a transaction,
  with `SET` / `SET LOCAL` and without changing `application_name`, see
  [`tags_override`](configuration.md#tags_override), whose tags win over
  both comments and `application_name`.

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  'sqlcommenter, appname(format=marginalia)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'service, job, controller';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SET application_name = 'service:billing,job:nightly';
SELECT pg_stat_statement_context_extract('SELECT 1') -> 'tags';
-- {"job": "nightly", "service": "billing"}
SELECT pg_stat_statement_context_extract($$SELECT 1 /*job='adhoc'*/$$) -> 'tags';
-- {"job": "adhoc", "service": "billing"}
RESET application_name;
```

A version string such as `billing/1.2.3`, with a regex:

```ini
pg_stat_statement_context.extractors = 'sqlcommenter, appname(format=regex, pattern=''^(\\w+)/([0-9.]+)$'', keys=app|version)'
```

### Validation

The setting is parsed and validated when it is set or reloaded; a malformed
value is rejected and the previous one stays in effect. It is rejected for:

- unknown extractors or parameters, a parameter given twice, or a parameter
  that the extractor doesn't take (e.g. `kv_sep` on `sqlcommenter`);
- more than 16 extractors;
- keys (in `keys` or `rename`) that are empty, longer than 63 bytes or contain
  whitespace, or a key renamed twice;
- empty separators, separators longer than 8 bytes, or a `kv_sep` that
  contains `pair_sep`;
- for `regex`: a missing `pattern` or `keys`, an invalid pattern, a pattern
  over 1 kB, a pattern that takes longer than 100 ms to compile,
  back-references, more capture groups than `max_tags`, or a number of `keys`
  different from the number of capture groups;
- for `appname`: a missing or unknown `format`, `position`, or a parameter of
  another format (e.g. `kv_sep` with `format=sqlcommenter`); the chosen
  format's own rules then apply (as for `regex` above with `format=regex`).

### Examples

Each example changes the configuration with `ALTER SYSTEM` and then checks it
with `pg_stat_statement_context_extract()`. `pg_reload_conf()` only signals
the server; each session applies the new values before its next command, so
when the statements run back-to-back from a script, a short pause is needed
first (see
[Changing the configuration from SQL](configuration.md#changing-the-configuration-from-sql)).

A Rails application (marginalia or the legacy `query_log_tags` format,
appended) that also keeps `application`:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'marginalia(position=append)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'application, controller, action, job';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*application:Shop,controller:users,action:show*/') -> 'tags';
-- {"action": "show", "controller": "users", "application": "Shop"}
```

A polyglot shop: Django/Flask through SQLCommenter (appended, key `route`),
and Rails through marginalia prepended (key `controller`), both normalized to
`endpoint`. The allowlist is applied after `rename`, so it names `endpoint`:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  'sqlcommenter(position=append, rename=route:endpoint), marginalia(position=prepend, rename=controller:endpoint)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'endpoint, action, job';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SELECT pg_stat_statement_context_extract(
         $$SELECT 1 /*route='%2Fusers%2F%3Aid'*/$$) -> 'tags';
-- {"endpoint": "/users/:id"}
SELECT pg_stat_statement_context_extract(
         '/*controller:users,action:show*/ SELECT 1') -> 'tags';
-- {"action": "show", "endpoint": "users"}
```

A custom house format, `/* svc=billing op=charge */`:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  'regex(pattern=''svc=(\w+)\s+op=(\w+)'', keys=service|operation)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'service, operation';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SELECT pg_stat_statement_context_extract(
         'SELECT 1 /* svc=billing op=charge */') -> 'tags';
-- {"service": "billing", "operation": "charge"}
```

The same setting in `postgresql.conf`. Values there undergo backslash-escape
processing, so the regex backslashes must be doubled (`ALTER SYSTEM` writes
this escaping for you):

```ini
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=service|operation)'
pg_stat_statement_context.tags       = 'service,operation'
```

Only original key names are matched by `keys`; here only `controller` is
taken from marginalia comments, and then renamed:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors =
  'marginalia(keys=controller, rename=controller:endpoint)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'endpoint, action';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show*/') -> 'tags';
-- {"endpoint": "users"}
```

Back to the defaults:

```sql
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
SELECT pg_reload_conf();
```

## The tag pipeline

Tag values are untrusted client input. They are never interpreted, and a bad
tag never raises an error in the user's statement. Each key/value pair an
extractor finds goes through these steps:

1. Decode (`sqlcommenter`: URL decoding and `\'`).
2. Reject pairs whose key or value contains a NUL byte or is invalid in the
   database encoding (counted in `invalid_tags`).
3. Apply the extractor's `keys` allowlist, matching the **original** key name.
4. Apply `rename`.
5. Apply the global `tags` allowlist, or the `exclude_tags` denylist when
   `tags = '*'`. Then drop keys longer than 63 bytes (counted in
   `invalid_tags`; a key that long can never match an allowlist entry, so
   with an allowlist it is simply not kept).
6. Normalize the value with the [`normalize`](configuration.md#normalize)
   rules for its (final) key, in order. A pair whose normalization fails is
   dropped (counted in the debug function's `normalize_failures`).
7. Truncate the value to `max_tag_value_len` bytes on a character boundary.
8. Apply the key's [cardinality cap](configuration.md#cardinality_cap), if
   it has one: once the key has had its cap of distinct values (counted
   server-wide, after the steps above), any other value is stored as JSON
   `null` (counted in `capped_tags`). In `max_tagset_bytes` a `null` value
   counts as 2 bytes (a string as its length plus 1). Only tags that step 9
   keeps are admitted, so a dropped tag never uses up a cap.
9. Sort and store within `max_tags` and `max_tagset_bytes`: tags are taken in
   priority order (`tags` list order, or sorted key order with `tags = '*'`).
   A tag is kept if it still fits; otherwise it is dropped (counted in
   `dropped_tags`) and the next one is tried, so an oversized tag never
   pushes out smaller lower-priority ones. This step runs after the extractor chain
   has chosen its winner (see [chain semantics](#the-extractor-dsl)), so a
   tag dropped here is not replaced by tags from a skipped extractor. The
   priority depends only on the key, not on whether the tag came from a
   comment or from `application_name`.

The [`tags_override`](configuration.md#tags_override) setting's pairs go
through the same steps, decoded when the setting is set (step 1), except
step 3: there is no extractor, so no `keys` list. For step 4 they use the
`rename` lists of the comment `sqlcommenter` extractors, in configuration
order (the first rule matching the key applies); other extractors'
`rename` lists are ignored, and with no `sqlcommenter` extractor nothing is
renamed.

The stored tag set is sorted by key and is part of the entry's key: two
statements with the same tags in a different order share an entry, and
different tag sets are never merged.

## Allowlist, denylist and cardinality

Every distinct tag set creates a separate entry for each query fingerprint.
A tag whose value is unique per request (`traceparent`, `request_id`, a user
ID, an unnormalized URL like `/users/123`) makes every statement unique and
can fill the table within seconds, evicting the useful entries.

- **Keep the allowlist small.** Only keys in `tags` are stored; the default is
  `action, controller, job`. Tags that developers add later are ignored until
  you opt in. Prefer low-cardinality keys: controller, action, route template,
  job class, service, endpoint.
- **Avoid `tags = '*'`.** If you use it, the `exclude_tags` denylist
  (default `traceparent, tracestate, request_id`) removes the best-known
  high-cardinality keys, but any other per-request key will still explode the
  table.
- **Watch for high-cardinality values in allowed keys,** such as routes with
  IDs in them, or a buggy or malicious client sending random values. Truncation
  (`max_tag_value_len`) bounds the size, not the number, of values. Values with
  known variable parts can be rewritten with
  [`normalize`](configuration.md#normalize) rules, for example
  `route: '/\d+' => '/:id'` turns `/users/123/posts/4` into
  `/users/:id/posts/:id`. Watch
  `_info().evicted_entries` (live entries lost to a full table; see
  [Eviction](configuration.md#eviction)) and look for keys with many distinct
  values:

  ```sql
  SELECT t.key, count(DISTINCT t.value) AS distinct_values
    FROM pg_stat_statement_context_totals c, jsonb_each_text(c.tags) t
   GROUP BY t.key
   ORDER BY 2 DESC;
  ```

- **High-cardinality comments also hurt the client side.** Drivers that use
  prepared statements cache them by SQL text including the comment, so each
  distinct comment value creates its own server-side prepared statement and
  churns the driver's statement cache (and pgbouncer's
  `max_prepared_statements`). See
  [Prepared statements](limitations.md#prepared-statements-carry-the-comment-from-prepare-time).

- **Cap the number of values per key** as a last line of defense:
  [`cardinality_cap`](configuration.md#cardinality_cap) (and per-key
  [`cardinality_cap_overrides`](configuration.md#cardinality_cap_overrides))
  let each key keep its first N distinct values and record any other value
  as JSON `null`. The statements still count, in a single `null` entry per
  query and remaining tags, rather than in thousands of entries:

  ```sql
  SELECT tags, calls FROM pg_stat_statement_context_totals
   WHERE tags->'route' = 'null';
  ```

  `_info().capped_tags` counts the values collapsed this way.

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
