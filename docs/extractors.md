# Extractors: from SQL comments to tags
This page describes these topics:

- how the extension finds comments in a statement,
- how the `pg_stat_statement_context.extractors` setting (the *extractor DSL*) changes the comments into tags, and
- how to control the number of different tag sets.

The [`appname`](#appname) extractor gets tags from `application_name`, not from comments. Use it for clients that cannot add comments.

To see what the current configuration extracts from a statement, use `pg_stat_statement_context_extract()`. By default, only superusers and the owner of the extension can call this function. See [the SQL interface](sql-interface.md#pg_stat_statement_context_extract).

## Where comments are found
`pg_stat_statement_context` does not parse SQL a second time. It does a lexical scan of the statement text. The scan uses the lexical rules of PostgreSQL, and it recognizes these items:

- string literals, including `E''`, `U&''`, and the escapes of `standard_conforming_strings = off`,
- quoted identifiers,
- dollar quotes,
- nested `/* /* */ */` comments, and
- `--` line comments.

Thus, text inside a string is not a comment, even if it looks like one.

### Statement range
A query string can contain many statements, for example `SELECT 1 /*a*/; SELECT 2 /*b*/`. Each statement sees only the comments in its own range. A comment after the last `;` belongs to no statement. There is one fallback: if the range of a statement gives no tags, the statement can use that trailing comment. This is possible only if the statement is the last one, which means that only `;`, whitespace, and comments come after it. For example, in `SELECT 1; SELECT 2; /*controller:x*/`, only `SELECT 2` gets the tag.

### Position
Each extractor examines the comments in one location. Its `position` parameter sets the location:

| `position` | Comments examined |
|---|---|
| `append` | The trailing run. This is the last comment before the optional trailing `;` and whitespace. It also includes all block comments immediately before that comment, if only whitespace separates them. The last comment must be a `/* */` block comment. A trailing `--` comment gives no tags. |
| `prepend` | The leading run. These are the comments before the first token, if only whitespace separates them. They can be block comments or `--` line comments. |
| `any` | All comments in the statement, in all locations. **This can be expensive. Use it carefully.** |

The extractor examines a run, not a single comment. Thus, it finds a context comment that comes before a free-text annotation, as marginalia writes them:

```sql
SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,action:show*/ /*with_annotation free text*/') -> 'tags';
-- {"action": "show", "controller": "users"}
```

### Long statements and `scan_window`
If a statement is not longer than `scan_window` bytes (default 2 kB), the extension always scans it exactly. A statement can be longer, for example if it has an `IN` list with 10,000 elements. For a longer statement:

- `prepend` examines only the first `scan_window` bytes. The result is still exact.
- `append` examines only the last `scan_window` bytes. It cannot know the lexical state at the start of that window, so this path is **heuristic**. It goes backward from the closing `*/` to the matching `/*`.
  - A comment that crosses the start of the window gives no tags.
  - Rarely, text that is not a comment can look like a comment. This can occur with a string literal that ends in `*/`, or with a `--` line comment that started before the window.
  - `pg_stat_statement_context_info().heuristic_scans` counts each use of this path.
- `any` always scans the full statement from the start. Thus, its cost increases with the length of the statement.
- If the statement has no trailing `;`, PostgreSQL gives no statement length. Thus, `append` first runs `strlen` on the full text to find its end.

For the measured costs, see [Long statements: scan costs and tuning](benchmarks.md#long-statements-scan-costs-and-tuning).

For each statement, the extension examines a maximum of 16 comments and a maximum of `scan_window` bytes of comment text.

## The extractor DSL
`pg_stat_statement_context.extractors` is a comma-separated list of extractors. Each extractor can have parameters:

```
extractor := name [ '(' param { ',' param } ')' ]
name      := sqlcommenter | marginalia | regex | appname
param     := key '=' value
```

The default is `'sqlcommenter, marginalia'`.

### Chain semantics
The extractors run in the order of the list.

- The first extractor that *produces* wins, and the extension skips the extractors after it.
- An extractor after the winner still runs if it has `merge=on`. The extension adds its tags.
- If more than one source gives the same key, the first value wins. The order is: first by extractor order, then by comment order, then by pair order.

[`appname`](#appname) extractors make a **separate chain** for `application_name`. The same rules apply in this chain: the first extractor that produces wins, and the extractors after it run only with `merge=on`. The two chains are independent:

- If a comment extractor produces, `appname` extractors still run. The opposite is also true.
- The extension then combines the tags of the two chains. If both chains give the same key, **the value from the comment wins**. This is true in all positions of the extractors in the list.
- The pipeline can drop a comment pair, for example because of an invalid value, a failed normalization, or a filtered key. In that case, the `application_name` value for that key can still be used.

The extension adds the tags of the [`tags_override`](configuration.md#tags_override) setting of the session to the tags of the two chains. The override tags win all key conflicts: **override > comment > `application_name`**. The override tags are not part of a chain. They do not stop an extractor. If the pipeline drops an override pair, that pair blocks nothing.

### When an extractor produces
An extractor *produces* when at least one of its tags is still a candidate after steps 1–7 of the [pipeline](#the-tag-pipeline). These steps are validation, the allowlists and the denylist, `rename`, normalization, and truncation. The extension decides this **before** step 8 (the cardinality caps) and step 9 (the `max_tags` and `max_tagset_bytes` limits). Thus:

- A value that a cap changes to `null` still counts as produced.
- If the limits in step 9 drop the tags of the winner, the extension does not try the skipped extractors again. The statement can get fewer tags, or no tags.
- With `untagged = skip`, the extension does not record a statement that has no tags.

For example, `max_tag_value_len = 1024` and `max_tagset_bytes = 128`. The statement is `SELECT 1 /*controller='xxx…'*/ /*action:show*/`, with a value of 200 bytes. This statement gets no tags (`dropped_tags` = 1). The reason is:

1. `sqlcommenter` wins with the `controller` tag, which is too large.
2. Step 9 drops that tag.
3. The extension never uses the `marginalia` comment `action:show`.

To prevent this, set `max_tagset_bytes` much larger than the largest possible tag set. To calculate that size, add `length(key) + max_tag_value_len + 2` for each key in your allowlist. The defaults give 217 bytes for 3 keys, which is much less than 512. See [`max_tagset_bytes`](configuration.md#max_tagset_bytes).

### Syntax
- Whitespace can be around all tokens.
- Extractor names, parameter names, and keyword values (`append`, `on`, ...) are not case-sensitive.
- An unquoted value continues until whitespace, `,`, or `)`. It cannot contain a quote or `(`.
- A quoted value is `'...'`. Use `''` for one quote. The extension uses the value exactly as written, so it can contain spaces, commas, and parentheses. Regex patterns almost always need quotes.
- Empty values are not permitted.
- The full setting is also a quoted string in SQL and in `postgresql.conf`. Thus, each quote of a quoted value is doubled one more time there: `'regex(pattern=''x=(\w+)'', keys=x)'`.

### Common parameters

| Parameter | Values | Meaning |
|---|---|---|
| `keys` | `a\|b\|c` | For `sqlcommenter` and `marginalia`: an allowlist for this extractor. It matches the **original** key names, as written in the comment. The extension applies it **before** `rename`. For `regex`: the names of the capture groups (required). |
| `merge` | `on`, `off` (default) | Add the tags of this extractor to the tags of the extractors before it. Without `merge=on`, the extension skips this extractor if an extractor before it produced. |
| `position` | `append`, `prepend`, `any` | The location to examine (see above). The default is `append` for `sqlcommenter` and `marginalia`, and `any` for `regex`. `appname` does not accept this parameter. |
| `rename` | `old:new\|old2:new2` | Changes the names of keys. For example, use it to change `route` and `controller` from different formats to one key. |

### `sqlcommenter`

The [SQLCommenter] format: `/*key='value',key2='value2'*/`.

- Values must have single quotes. A `,` inside quotes does not split the pair.
- The extension unescapes `\'` and `\\`.
- With `url_decode=on` (the default), the extension URL-decodes keys and values:
  - It decodes `%XX`.
  - It changes a raw `+` to a space. Thus, emitters that use form encoding (Go and Java) and emitters that use `%20` (Python and Node) give the same value.
  - `%2B` is a literal `+`.
  - It keeps an invalid `%` escape as it is.
- `url_decode=off` disables decoding.

| Parameter | Default |
|---|---|
| `url_decode` | `on` |

```sql
SELECT pg_stat_statement_context_extract(
         $$SELECT 1 /*controller='users%2Fshow',action='it\'s+here'*/$$) -> 'tags';
-- {"action": "it's here", "controller": "users/show"}
```

### `marginalia`
The legacy format of [marginalia] and Rails `query_log_tags`: `/*application:Foo,controller:users,action:show*/`.

- First, the extension splits the comment into pairs at each `pair_sep`.
- Then, it splits each pair at the **first** `kv_sep` only. This is necessary because some values contain colons, for example `line:app/models/u.rb:12`.
- The extension does not decode the values.

| Parameter | Default | Notes |
|---|---|---|
| `kv_sep` | `:` | 1–8 bytes. It must not contain `pair_sep`. |
| `pair_sep` | `,` | 1–8 bytes. |

A separator can start with whitespace, for example `kv_sep=' :'`. Put quotes around such a separator.

### Rules for both `sqlcommenter` and `marginalia`
- The extension removes ASCII whitespace around the comment body, each pair, each key, and each value. It ignores empty segments.
- A segment is malformed if it has no separator, or if its key contains whitespace. The extension skips malformed segments. Thus, free-text annotations do not become tags.
- `pg_stat_statement_context_info().invalid_tags` counts a malformed segment only if the same comment also has a correct pair for that extractor. Thus, the extension does not count a comment that is in the format of a different extractor.

```sql
SELECT r -> 'tags' AS tags, r -> 'invalid_tags' AS invalid_tags
  FROM pg_stat_statement_context_extract(
         'SELECT 1 /*controller:users,free text,action:show*/') AS r;
-- tags: {"action": "show", "controller": "users"}, invalid_tags: 1
```

### `regex`
A custom format: `regex(pattern='...', keys='k1|k2', position=any)`.

- `pattern` (required) is a PostgreSQL advanced regular expression. This is the same engine as `~` and `regexp_matches`. The extension uses the C collation. It applies the pattern **only to comment text**, never to the other parts of the query.
- `keys` (required) gives names to the capture groups: capture group *n* becomes key *n*. The number of keys must be equal to the number of capture groups.
- The extension uses all non-overlapping matches in a comment, as `regexp_matches(..., 'g')` does.
  - `^` matches only at the start of the comment body.
  - An optional group that did not match gives no tag.
  - If a key has more than one value, the first value wins.
- Limits:
  - The pattern is a maximum of 1 kB.
  - It has a maximum of `max_tags` capture groups.
  - It cannot use back-references.
  - It must compile in a maximum of **100 ms**.

#### The compile time limit
Some short patterns can take the regex engine seconds or minutes to compile. An example is a bounded repetition of a group that can match the empty string: `((?:(?:$)|\Zda|(?<!1)|\S){0,255})`. The compile time limit prevents such a pattern from stopping queries:

- **`ALTER SYSTEM`.** When you set the value with `ALTER SYSTEM`, the extension does a test compile. If the compile takes longer than 100 ms, the extension rejects the pattern (`DETAIL: Compiling the pattern of extractor "regex" took longer than 100 ms.`). `ALTER SYSTEM` stops the compile at the limit, and it does not write the value.
- **The configuration file.** The extension never rejects a value from the configuration file only because of compile time. This applies at startup, on reload, and in `pg_file_settings`. Thus, all processes get the same configuration.
  - The postmaster compiles the pattern until the compile is complete. Thus, a reload can take as long as that compile. If the compile was too slow, the postmaster writes this log message: `compiling the pattern of extractor "regex" took N ms, longer than the 100 ms limit`.
  - On reload, each backend checks the value again. It stops its compile at the limit. Then it accepts the value without the checks that need the compiled pattern.
  - Then the run-time compile of each backend (see the next item) disables a pattern that is too slow.
  - Parallel workers do not compile the patterns. They use the value of the leader without changes, and they never extract tags.
- **Run time.** Each backend compiles the pattern again for its first tagged statement.
  - In a client backend, if a compile is still running after 100 ms, the backend stops it. This counts as a compile failure (see below). The statement is not cancelled, and it does not fail.
  - Other processes, for example background workers, have no limit at run time.
- **Load on the host.** In a backend, the limit is elapsed time.
  - A compile can reach the limit while it used less than half of the limit in CPU time. This shows that the backend was mostly waiting for the CPU, because the host has a high load. In that case, the backend tries the compile again, up to a total of 3 attempts. Thus, a statement waits a maximum of approximately 300 ms for a slow pattern.
  - In a VM, time that the host takes from the virtual CPU can count as CPU time. Many hypervisors (for example Docker Desktop on macOS) do not tell the guest about steal time, so the VM cannot find it.
  - Thus, on a host with a very high load, the limit can stop a normal pattern. `ALTER SYSTEM` can reject it (try again). Or, a backend can disable it until the next change to `extractors` or `normalize`.
- **Keep patterns well below the limit.** The limit is for each pattern. Compile times are different on different machines and with different loads. Thus, a pattern near the limit can be accepted when you set it, but still be stopped in some backends. Normal patterns compile in much less than 1 ms.

#### Compile failures at run time

A regex can fail to compile in a backend at run time, because of low memory or because of the compile time limit. Then:

- The backend disables that extractor until the next change to `extractors` or `normalize`.
- `pg_stat_statement_context_info().regex_compile_failures` counts the failure.
- The statement of the user does not fail because of the pattern.

Interrupts that arrive during the compile have their usual effect. These include a query cancel, `statement_timeout`, `transaction_timeout`, and a recovery conflict. But a cancel or a `statement_timeout` can wait until the compile ends or reaches the time limit. The extension does not hide other unexpected errors in the regex engine. Such an error causes the statement to fail as usual.

### `appname`

`appname(format=sqlcommenter|marginalia|regex, ...)` gets tags from the `application_name` of the session, not from comments. Use it for drivers and tools that can set `application_name` but cannot add comments to their queries. Examples of ways to set `application_name` are `PGAPPNAME` and the `application_name` connection parameter.

- **Format.** `format` (required) selects the parser. The extension parses the full `application_name` string as if it were a comment body. It uses the rules and the parameters of that format:
  - `url_decode` for `sqlcommenter`,
  - `kv_sep` and `pair_sep` for `marginalia`, and
  - `pattern` and `keys` (both required) for `regex`.

  The extension rejects a parameter of a different format. `keys`, `rename`, and `merge` work as for the other extractors. `position` is not accepted.
- **Pipeline.** The tags go through the same [pipeline](#the-tag-pipeline) as comment tags: allowlists, `rename`, `normalize`, truncation, and limits. The extension then combines them with the comment tags, as described in [chain semantics](#the-extractor-dsl). If both give the same key, the comment wins. A statement without a comment still gets the `application_name` tags. For `untagged = skip`, such a statement counts as tagged.
- **Malformed segments.** The extension drops malformed segments. `pg_stat_statement_context_info().invalid_tags` counts them with the usual rule: only if the same value also has a correct pair. Thus, a plain name such as `psql` adds nothing to the count. The extension always counts a value that decodes to a NUL byte or to invalid text.
- **Time of the value.** The extension uses the value of `application_name` **when the statement starts to execute**:
  - A prepared statement uses the value at the time of `EXECUTE` (or of the protocol Execute message), not at the time of `PREPARE`.
  - A `SET application_name` statement gets the tags of the previous value.
  - `SET LOCAL` stays in effect until the end of the transaction.
  - With [`nested_tags`](configuration.md#nested_tags) `= inherit`, a nested statement gets the tags of its top-level statement. This is true even if a function changed `application_name` before the nested statement. With `scan`, the nested statement reads the value that is current when it starts.
- **ASCII only.** PostgreSQL replaces non-ASCII characters in `application_name`. On PostgreSQL 14 and 15, it uses `?`. On PostgreSQL 16 and later, it uses `\xHH` escapes. Thus, tag values from `application_name` are ASCII.
- **Cache.** Each backend keeps the parsed result for the last `application_name` and configuration. Thus, a statement usually costs one string comparison. The extension adds the counters (`invalid_tags`, `normalized_tags`, ...) again for each statement, as if there were no cache.
- **Debug function.** `pg_stat_statement_context_extract()` uses the current `application_name` of the calling session. Its result does not show these tags in a separate field.
- **Alternative.** A client can also set tags for a session or a transaction with `SET` or `SET LOCAL`, without a change to `application_name`. For this, see [`tags_override`](configuration.md#tags_override). The override tags win over comments and over `application_name`.

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

This example uses a regex for a version string such as `billing/1.2.3`:

```ini
pg_stat_statement_context.extractors = 'sqlcommenter, appname(format=regex, pattern=''^(\\w+)/([0-9.]+)$'', keys=app|version)'
```

### Validation

The extension parses and validates the setting when you set it or reload it. If the value is malformed, the extension rejects it, and the previous value stays in effect. The extension rejects a value for these reasons:

- an unknown extractor or parameter, a parameter that occurs two times, or a parameter that the extractor does not accept (for example `kv_sep` on `sqlcommenter`),
- more than 16 extractors,
- a key (in `keys` or `rename`) that is empty, is longer than 63 bytes, or contains whitespace,
- a key that is renamed two times,
- a separator that is empty or longer than 8 bytes, or a `kv_sep` that contains `pair_sep`,
- for `regex`:
  - a missing `pattern` or `keys`,
  - an invalid pattern,
  - a pattern that is longer than 1 kB,
  - a pattern that takes longer than 100 ms to compile,
  - back-references,
  - more capture groups than `max_tags`, or
  - a number of `keys` that is different from the number of capture groups,
- for `appname`:
  - a missing or unknown `format`,
  - `position`, or
  - a parameter of a different format (for example `kv_sep` with `format=sqlcommenter`).

  The rules of the selected format also apply. For example, with `format=regex`, the rules for `regex` above apply.

### Examples

Each example changes the configuration with `ALTER SYSTEM`, and then checks it with `pg_stat_statement_context_extract()`. `pg_reload_conf()` only sends a signal to the server. Each session applies the new values before its next command. Thus, if a script runs the statements one after the other, a short pause is necessary first (see [Changing the configuration from SQL](configuration.md#changing-the-configuration-from-sql)).

A Rails application that uses marginalia or the legacy `query_log_tags` format, appended. It also keeps `application`:

```sql
ALTER SYSTEM SET pg_stat_statement_context.extractors = 'marginalia(position=append)';
ALTER SYSTEM SET pg_stat_statement_context.tags = 'application, controller, action, job';
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);  -- the reload is asynchronous

SELECT pg_stat_statement_context_extract(
         'SELECT 1 /*application:Shop,controller:users,action:show*/') -> 'tags';
-- {"action": "show", "controller": "users", "application": "Shop"}
```

A shop with many languages:

- Django and Flask use SQLCommenter, appended, with the key `route`.
- Rails uses marginalia, prepended, with the key `controller`.
- Both keys are changed to `endpoint`.

The extension applies the allowlist after `rename`. Thus, the allowlist contains `endpoint`:

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

A custom format of a company, `/* svc=billing op=charge */`:

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

The same setting in `postgresql.conf`. PostgreSQL processes backslash escapes in the values of this file. Thus, you must double the backslashes of the regex. (`ALTER SYSTEM` writes this escaping for you.)

```ini
pg_stat_statement_context.extractors = 'regex(pattern=''svc=(\\w+)\\s+op=(\\w+)'', keys=service|operation)'
pg_stat_statement_context.tags       = 'service,operation'
```

`keys` matches only original key names. In this example, the extension takes only `controller` from marginalia comments, and then changes its name:

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

To go back to the defaults:

```sql
ALTER SYSTEM RESET pg_stat_statement_context.extractors;
ALTER SYSTEM RESET pg_stat_statement_context.tags;
SELECT pg_reload_conf();
```

## The tag pipeline

Tag values are client input that is not trusted. The extension never interprets them. A bad tag never causes an error in the statement of the user. Each key/value pair that an extractor finds goes through these steps:

1. **Decode.** For `sqlcommenter`, this is URL decoding and `\'`.
2. **Validate.** Reject pairs whose key or value contains a NUL byte, or is not valid in the database encoding. `invalid_tags` counts them.
3. **Extractor allowlist.** Apply the `keys` allowlist of the extractor. It matches the **original** key name.
4. **Rename.** Apply `rename`.
5. **Global allowlist.** Apply the global `tags` allowlist. With `tags = '*'`, apply the `exclude_tags` denylist. Then drop keys that are longer than 63 bytes, and count them in `invalid_tags`. (Such a key can never match an allowlist entry. Thus, with an allowlist, the extension does not keep it.)
6. **Normalize.** Apply the [`normalize`](configuration.md#normalize) rules for the (final) key of the value, in order. If a normalization fails, drop the pair. The `normalize_failures` field of the debug function counts these pairs.
7. **Truncate.** Truncate the value to `max_tag_value_len` bytes, at a character boundary.
8. **Cardinality cap.** If the key has a [cardinality cap](configuration.md#cardinality_cap), apply it.
   - The extension counts the different values of the key after the steps above. It counts them for each [`cardinality_cap_scope`](configuration.md#cardinality_cap_scope). By default, it counts separately for each role and database, for all queries.
   - When the key has its cap of different values, the extension stores all other values as JSON `null`. `capped_tags` counts these values.
   - In `max_tagset_bytes`, a `null` value counts as 2 bytes. A string counts as its length plus 1.
   - The extension admits only tags that step 9 keeps. Thus, a dropped tag never uses part of a cap.
9. **Sort and store** within `max_tags` and `max_tagset_bytes`.
   - The extension takes the tags in priority order. This is the order of the `tags` list, or the sorted key order with `tags = '*'`.
   - If a tag still fits, the extension keeps it. If it does not fit, the extension drops it (counted in `dropped_tags`) and tries the next tag. Thus, a tag that is too large never pushes out smaller tags of lower priority.
   - This step runs after the extractor chain selects its winner (see [chain semantics](#the-extractor-dsl)). Thus, tags from a skipped extractor do not replace a tag that this step drops.
   - The priority depends only on the key. It does not depend on the source of the tag (a comment or `application_name`).

The pairs of the [`tags_override`](configuration.md#tags_override) setting go through the same steps, with these differences:

- The extension decodes them (step 1) when you set the setting.
- They skip step 3, because there is no extractor, and thus no `keys` list.
- For step 4, they use the `rename` lists of the `sqlcommenter` comment extractors, in configuration order. The first rule that matches the key applies. The extension ignores the `rename` lists of other extractors. If there is no `sqlcommenter` extractor, the extension does not rename the keys.

The extension sorts the stored tag set by key. The tag set is part of the key of the entry. Thus, two statements with the same tags in a different order use the same entry. The extension never merges different tag sets.

## Allowlist, denylist, and cardinality

Each different tag set makes a separate entry for each query fingerprint. Some tags have a different value for each request, for example `traceparent`, `request_id`, a user ID, or a URL that is not normalized, such as `/users/123`. Such a tag makes each statement unique. It can fill the table in seconds and evict the useful entries.

- **Keep the allowlist small.** The extension stores only the keys in `tags`. The default is `action, controller, job`. If developers add tags later, the extension ignores them until you add them to the list. Use keys with low cardinality, for example controller, action, route template, job class, service, and endpoint.
- **Do not use `tags = '*'`.** If you use it, the `exclude_tags` denylist removes the best-known keys with high cardinality. The default denylist is `traceparent, tracestate, request_id`. But all other keys with a different value for each request will still make the table too large.
- **Look for values with high cardinality in allowed keys.** Examples are routes that contain IDs, or a client with a bug, or a malicious client, that sends random values.
  - Truncation (`max_tag_value_len`) limits the size of the values, not their number.
  - You can use [`normalize`](configuration.md#normalize) rules to change values with known variable parts. For example, `route: '/\d+' => '/:id'` changes `/users/123/posts/4` to `/users/:id/posts/:id`.
  - Monitor `pg_stat_statement_context_info().evicted_entries`. This counts live entries that the extension removed because the table was full (see [Eviction](configuration.md#eviction)).
  - Look for keys with many different values:

  ```sql
  SELECT t.key, count(DISTINCT t.value) AS distinct_values
    FROM pg_stat_statement_context_totals c, jsonb_each_text(c.tags) t
   GROUP BY t.key
   ORDER BY 2 DESC;
  ```

- **Comments with high cardinality also cause problems on the client side.** Drivers that use prepared statements keep them in a cache by SQL text, and the SQL text includes the comment. Thus, each different comment value makes its own prepared statement on the server. This also causes churn in the statement cache of the driver, and in `max_prepared_statements` of pgbouncer. See [Prepared statements](limitations.md#prepared-statements-carry-the-comment-from-prepare-time).

- **Cap the number of values for each key** as a last protection. With [`cardinality_cap`](configuration.md#cardinality_cap) and [`cardinality_cap_overrides`](configuration.md#cardinality_cap_overrides) (for each key), each key keeps its first N different values. The extension records all other values as JSON `null`. The extension still counts the statements, but in one `null` entry for each query and set of other tags, not in thousands of entries:

  ```sql
  SELECT tags, calls FROM pg_stat_statement_context_totals
   WHERE tags->'route' = 'null';
  ```

  `pg_stat_statement_context_info().capped_tags` counts the values that the extension changed to `null`. A cap limits each key separately. It does not limit the number of entries. See [below](#caps-bound-values-not-combinations).

### Caps bound values, not combinations

An entry is one {role, database, `queryid`, `toplevel`, tag} set. A cap limits the values of **one key**. It does not limit the tag sets that the keys make together.

- With k kept keys, and a cap of N values for each key, one query of one role in one database can make N^k entries. This occurs when each statement has all k keys.
- When values change to `null`, the limit becomes (N + 1)^k, because `null` is one more value of each key.
- A statement can also be without a key. This occurs if the statement does not have the key, or if `max_tags` or `max_tagset_bytes` drops it. A missing key is different from all strings and from `null`. Thus, when keys are optional (which is usual), each key has N + 2 states. The limit is (N + 2)^k − 1 tag sets that are not empty. With [`untagged = record`](configuration.md#untagged), there is also one entry for the empty set.
- All of this can occur without a cap event, while each value stays within its cap.

For example, `cardinality_cap = 5`, `max_entries = 100`, and the default keys are `action, controller, job`. A client sends each combination of 5 values of each key to one query. This makes 5^3 = 125 entries.

- This is more than the table can hold. 5 eviction passes evict 25 live entries, while `capped_tags` and `cap_table_full` stay at 0.
- With 7 values of each key, and sufficient space in the table, the same query has 6^3 = 216 entries (5 strings and `null` for each key).
- If each key can also be missing, the query has 7^3 − 1 = 342 entries.

`test/t/035_cap_combinations.pl` checks these numbers.

These limits are correct only if the caps had their current values since the last [`pg_stat_statement_context_reset()`](sql-interface.md#pg_stat_statement_context_reset), or since a restart that restored no saved statistics. If not, use a different number for N for each key. This number is the number of admitted values of the key (in its [cap scope](configuration.md#cardinality_cap_scope)), plus the other values in live entries. It can be larger than N:

- **A lower cap.** The extension keeps the values that it already admitted. An admitted value stays a string, for all values of the cap. The extension forgets admitted values only after `pg_stat_statement_context_reset()` or a restart. It does not forget them when their entries are evicted or expire. For example, a key has 5 admitted values with `cardinality_cap = 5`, and then a reload changes the cap to 2. The result is still 6^3 = 216 tag sets, not 3^3 = 27. New statements continue to make these tag sets.
- **A cap set to `0` and then set again.** The result is the same as for a lower cap.
  - While a key has no cap, the extension does not check or admit its values. But the earlier admitted values stay.
  - When you set a cap again, these values still count against it.
  - The entries made while the key had no cap keep their values, as in the next case.
  - Thus, 5 → 0 → 2 without `pg_stat_statement_context_reset()` keeps all 5 values, and admits no new value.
- **A cap enabled on a key with no admitted values.** This occurs when `cardinality_cap`, or the override of the key, changes from `0`. The extension admits the first N values that it sees after the change. The entries that the extension collected while the key had no cap keep their values. They keep them until their history expires (`bucket_count × bucket_interval`), or until the extension evicts them. For example, 125 entries (5^3) are collected without a cap, and then the cap is set to 2. The result is 125 + 19 = 144 tag sets.
- **A restart with [`save`](configuration.md#save) on** (the default). The restart forgets all admitted values. But it loads again the entries that the extension saved at the clean shutdown, with their tags, and it does not check the caps. This is the same as the previous case: the restored values count until their entries expire or are evicted. A restart makes the limit correct again only if the extension loads no statistics. This occurs with `save` off, or if the extension does not use a saved file.

The same test checks these four cases.

Real keys are related. For example, an action belongs to one controller, and a job sets no action. Thus, the product is a maximum, and the number of tag sets that you see is usually much smaller. Size the table for the tag sets that you see, not for the caps:

- **Size `max_entries` for the combinations that you see** in one history window (`bucket_count × bucket_interval`), with spare space. See [Sizing `max_entries`](configuration.md#sizing-max_entries). To find the queries that make many combinations, count the tag sets of each query:

  ```sql
  SELECT userid, dbid, queryid, count(*) AS tag_sets
    FROM pg_stat_statement_context_totals
   GROUP BY 1, 2, 3
   ORDER BY 4 DESC
   LIMIT 20;
  ```

- **Use caps to limit the worst case**, not to size the table.
  - For each role, database, query, and `toplevel`, the maximum number of tag sets is the product of (cap + 2) for all kept keys. The 2 is for `null` and for a missing key.
  - If each statement has each key, the maximum is the product of (cap + 1).
  - Keys without a cap have no limit.
  - In some cases, use the admitted values and the live values of each key, not its cap ([see above](#caps-bound-values-not-combinations)). These cases are: after a cap was made lower, after a cap was disabled and enabled again, after a cap was enabled on collected data, and after a restart that restored saved statistics.
  - Keep that product, multiplied by the number of queries that you expect, near `max_entries`. Or, accept that the extension evicts the oldest combinations.
- **A lower cap stops growth, but it frees nothing.** After you make a cap lower or remove it with a reload, the admitted values stay admitted. They also keep their slots in the table of the caps. Only a restart, or [`pg_stat_statement_context_reset()`](sql-interface.md#pg_stat_statement_context_reset), frees them. `pg_stat_statement_context_reset()` also deletes **all** collected statistics.
- **Keep few independent keys.** Each added key multiplies the limit by its number of values. Some keys get their value from another key, for example an action name that is unique for each controller. Such a key adds no combinations. Two keys that are not related multiply.

### Watching for cardinality pressure

[`pg_stat_statement_context_counters()`](sql-interface.md#pg_stat_statement_context_counters) and [`pg_stat_statement_context_info()`](sql-interface.md#pg_stat_statement_context_info) show these counters. `pg_stat_statement_context_counters()` costs little, so you can call it on each scrape. The [exporter recipes](integrations/README.md) export the counters. This table shows what each counter means when it increases, and what to do:

| Counter | What it counts | When it increases |
|---------|----------------|---------------|
| `evicted_entries` | Live entries that an [eviction pass](configuration.md#eviction) removed, because the reclaim of dead entries did not free 5% of the table. The history of these entries is lost. | There are too many combinations for `max_entries`. Find the queries with the most tag sets (see above), and the keys with the most values ([query](#allowlist-denylist-and-cardinality)). Then normalize or cap those keys, or stop keeping them. Or, increase `max_entries` (this needs a restart). |
| `dealloc` | Eviction passes. A pass runs when a new combination finds the table full. Each pass scans the full table while it holds an exclusive lock. | If only this counter increases (`reclaimed_entries` also increases, but `evicted_entries` does not), this is housekeeping on a table whose combinations come and go. [`reclaim_worker`](configuration.md#reclaim_worker) can free dead entries in the background instead. If `evicted_entries` also increases, do the same as for `evicted_entries`. |
| `dropped_records` | Calls that the extension did not record. The table was full of live entries, and a pass could free nothing, because it could not allocate its working memory. Or, the shared hash table refused the insert. | The table is much too small, or memory is low. Do the same as for `evicted_entries`, and check the memory of the server. |
| `capped_tags` | Values that a [cardinality cap](configuration.md#cardinality_cap) changed to `null`, or that became `null` because the table of the caps was full. | This is expected if you want a cap on a key. If it increases continuously, a key continues to get new values. Find the key (`tags->'key' = 'null'`). Then normalize it, increase its [override](configuration.md#cardinality_cap_overrides), or stop keeping it. You cannot attribute its `null` rows. |
| `cap_table_full` | Values that became `null` because the shared table of the caps had no space, for all caps of their keys. `capped_tags` also counts these values. | Increase [`cardinality_cap_slots`](configuration.md#cardinality_cap_slots) (this needs a restart). Lower caps only stop more growth. Only `pg_stat_statement_context_reset()` or a restart frees the slots of values that the extension already admitted. `pg_stat_statement_context_reset()` also deletes all collected statistics. Until then, new values of all roles become `null`. |

It is normal for `entries` to stay at `max_entries` (see [Eviction](configuration.md#eviction)). Use the counters above to tell the difference between pressure and housekeeping.

### Cap scope and trust

The extension counts caps for each [`cardinality_cap_scope`](configuration.md#cardinality_cap_scope). The default is `role`. With `role`, each role and database has its own caps and admitted values. With `database` or `server`, roles share the caps. Then, a role that can read its own rows can do these things:

- **Find which values other roles sent.** When a key is at its cap, a value that another role already sent stays a string. A value that no role sent becomes `null`. Thus, a role can send a value and see if a different role sent it before.
- **Use all of the cap.** A role can fill the cap of a key with its own values. Then, all new values of other roles become `null` until a `pg_stat_statement_context_reset()`.

Use `role` (the default) on multi-tenant services and managed services. Use `database` or `server` only if all roles that share a scope trust each other.

With all scopes, all roles share the table of the caps (`cardinality_cap_slots`) and the entries (`max_entries`). Thus, the combinations of one role can evict the entries of other roles. A full table of caps changes the new values of all roles to `null`. Size the two tables for all tenants together.

[marginalia]: https://github.com/basecamp/marginalia
[SQLCommenter]: https://google.github.io/sqlcommenter/
