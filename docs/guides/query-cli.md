# Query CLI Guide

The `query` subcommand runs SQL statements — parse → plan → execute → render — and prints results to stdout. Supply a statement or file for one-shot execution, or omit both to open an interactive session.

> **Related:** [Query Engine Architecture](../reference/query-engine-architecture.md) | [Configuration Reference](configuration.md#queryable-block) | [Tutorial: first queries with sample data](#tutorial-first-queries-with-sample-data)

## Table of contents

- [Command](#command)
  - [Options](#options)
  - [Interactive session](#interactive-session)
  - [Stored query tables](#stored-query-tables)
  - [Line editing and completion](#line-editing-and-completion)
- [Quick-start examples](#quick-start-examples)
  - [Scalar timestamp functions](#scalar-timestamp-functions)
- [Query-only configuration](#query-only-configuration)
  - [Config file](#config-file)
  - [Inline dotted assignments](#inline-dotted-assignments-no-config-file)
- [SQL syntax reference](#sql-syntax-reference)
  - [Statement types](#statement-types)
  - [SELECT grammar](#select-grammar)
  - [Predicates](#predicates)
  - [Time literals](#time-literals)
  - [Joins](#joins)
  - [Pagination](#pagination)
- [Virtual table catalog](#virtual-table-catalog)
- [Output formats](#output-formats)
- [Tuning notes](#tuning-notes)
- [Tutorial: first queries with sample data](#tutorial-first-queries-with-sample-data)

---

## Command

```bash
mldp_pvxs_driver [global options] query [query options] "<SQL>"
# Or start an interactive SQL session
mldp_pvxs_driver [global options] query [query options]
```

Global config must appear **before** `query`:

```bash
mldp_pvxs_driver -c config.yaml query "<SQL>"
```

### Options

| Option | Default | Purpose |
|---|---:|---|
| `--file <path>` | — | Read SQL text from a file instead of the positional argument. |
| `--format <fmt>` | `table` | Output format: `table`, `json`, `csv`, `arrow`. |
| `--table-fit` | off | Fit table output to an interactive terminal viewport by truncating long headers and values with `...`; ignored when output is redirected or piped. |
| `--no-stats` | off | Suppress the query-stats footer. |
| `--memory-mb <n>` | `256` | Memory budget for the execution context (MiB). |
| `--spill-dir <path>` | `<tmp>/mldp-query-spill` | Directory for spill files under memory pressure. |
| `--table-catalog-dir <path>` | `<tmp>/mldp-query-catalog` | Root directory for durable Arrow IPC snapshots; separate from `--spill-dir`. |
| `--spill-partitions <n>` | `16` | Spill partition count for join spill paths. |
| `--join-batch-size <n>` | `100` | Batch size hint for join execution and pagination. |

### Interactive session

Run `query` without positional SQL or `--file` to start the REPL:

```bash
mldp_pvxs_driver -c query-config.yaml query
mldp> SHOW TABLES;
```

Terminate each statement with a semicolon. Statements can span lines; the prompt changes from `mldp> ` to `...> ` while a statement is buffered. A semicolon inside a quoted string does not terminate the statement. The session executes one statement at a time and remains open after parse, planning, or execution errors.

### Stored query tables

`CREATE TEMP TABLE name AS SELECT ...` materializes a read-only Arrow IPC snapshot for the current REPL/client session. It can be queried and joined by later statements, then is removed when the runner exits or when `DROP TABLE name` is issued. `CREATE TABLE name AS SELECT ...` writes an immutable persistent Arrow IPC snapshot below `--table-catalog-dir`; later clients using the same directory can select it, describe it, and discover it through `SHOW TABLES`.

```sql
CREATE TEMP TABLE recent_samples AS
SELECT pv, time, value FROM mldp.time_series WHERE pv = 'BPMS:IN20:221:TMIT';

CREATE TABLE production_samples AS
SELECT pv, time, value FROM mldp.time_series WHERE pv = 'BPMS:IN20:221:TMIT';
SELECT pv, value FROM production_samples;
DROP TABLE production_samples;
```

`CREATE TABLE` fails if the name exists; explicitly `DROP TABLE` before recreating it. Persistent tables are immutable snapshots, not live gRPC views, and are never automatically refreshed. The catalog manages only its own `.mldp-query-tables` namespace inside the configured root and does not clean unrelated files. Use a shared mounted directory when multiple processes or hosts must share persistent tables.

Parenthesized `SELECT` statements are valid statement-scoped derived sources in
`FROM` and `JOIN` positions and require an alias. `IN (SELECT ...)` is reserved
for the `pv` and `window` inputs of `mldp.time_series_table`; scalar subqueries
remain unsupported.

```sql
SELECT recent.pv, recent.value
FROM (
  SELECT pv, value FROM mldp.time_series WHERE pv = 'BPMS:IN20:221:TMIT'
) AS recent
WHERE recent.value > 0;
```

For example:

```text
mldp> SELECT name, category
...> FROM mldp.configuration
...> WHERE category = 'beam';
```

`mldp.pv_metadata` and `mldp.configuration` support unfiltered list operations:

```sql
SELECT * FROM mldp.pv_metadata;
SELECT * FROM mldp.configuration;
```

| Command | Purpose |
|---|---|
| `.help` | Show statement, command, and editing usage. |
| `.clear` | Discard the buffered statement and clear/redraw the interactive terminal. With redirected input, print a confirmation instead. |
| `.history`, `history` | Print the command history. In an interactive terminal this includes saved history from earlier sessions. |
| `.format` | Print the current output style. |
| `.format <table\|json\|csv\|arrow>` | Set the output style for subsequent statements in this REPL session. |
| `.table-fit [on\|off]` | Show or change whether table output is fitted to the interactive terminal width for this REPL session. |
| `.quit`, `.exit` | Exit the session. |

### Line editing and completion

When both standard input and output are interactive terminals, the REPL provides shell-style editing. Use Tab to complete SQL keywords, REPL commands, output styles, table names registered by the active `queryable:` configuration, and columns after a table alias. Completion is case-insensitive and prefix-based; it does not complete inside quoted literals. When more than one candidate matches, the terminal displays the choices and keeps the current input.

| Keys | Behavior |
|---|---|
| Left/Right, Ctrl-B/Ctrl-F | Move one character backward/forward. |
| Home/End, Ctrl-A/Ctrl-E | Move to the beginning/end of the editable line. |
| Up/Down | Navigate prior completed statements and commands. |
| Backspace/Delete, Ctrl-D | Delete before/at the cursor. Ctrl-D at an empty primary prompt exits. |
| Ctrl-W / Alt-D | Delete the preceding / following word. |
| Ctrl-U / Ctrl-K | Erase text before / after the cursor. |
| Ctrl-L | Clear and redraw the terminal. |
| Ctrl-C | Cancel the editable line, discard any buffered multi-line statement, and return to `mldp> `. |

The REPL saves completed SQL statements and dot commands (but never result output or errors) across interactive sessions. Use `.history` (or `history`) to print it. It uses `$XDG_STATE_HOME/mldp-pvxs-driver/query-history`; if `XDG_STATE_HOME` is unset, it uses `$HOME/.local/state/mldp-pvxs-driver/query-history`. On startup it removes prompt and result-output entries left by older versions. Delete that file to clear saved history.

When input is redirected or supplied by a script, the REPL retains plain line-based input: it shows the prompts, but disables terminal editing, completion, and persistent history. One-shot positional SQL and `--file` mode are unaffected.

---

## Quick-start examples

### Scalar timestamp functions

Scalar functions may be used in `WHERE` values. Function names are case-insensitive and calls may be nested. The built-in `to_utc` converts a user-facing timestamp into the query engine's UTC epoch-second timestamp value.

```sql
SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'MY:PV'
  AND time >= to_utc('2026-07-23T09:00:00-07:00');

SELECT pv FROM mldp.time_series
WHERE time >= to_utc('2026-07-23 09:00:00', '-07:00');
```

The one-argument form requires `Z` or an explicit `+/-HH:MM` offset. The two-argument form currently accepts an explicit offset. Results are truncated to epoch-second precision.

```bash
# Schema introspection — queryable config required
mldp_pvxs_driver -c query-config.yaml query "SHOW TABLES"
mldp_pvxs_driver -c query-config.yaml query "DESCRIBE mldp.time_series"

# Fetch last hour of samples for one PV
mldp_pvxs_driver -c config.yaml query \
  "SELECT pv, time, value FROM mldp.time_series
   WHERE pv = 'MY:PV:NAME' AND time >= NOW -1h AND time <= NOW"

# Read SQL from a file
mldp_pvxs_driver -c config.yaml query --file queries/export.sql

# CSV output, suppress stats
mldp_pvxs_driver -c config.yaml query --format csv --no-stats \
  "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'MY:PV' LIMIT 1000"
```

---

## Query-only configuration

`query` mode activates only `queryable:` backends. It does not require `reader`, `writer`, `routing`, or `metrics` blocks.

The client settings are nested in their named pool (`mldp-pool` for `mldp`, and `mldp-annotation-pool` or the `mldp-pv-metadata-pool` alias for annotation tables). The CLI also accepts the older flat form with the endpoint and connection settings directly under the type key.

### Config file

Service hostnames match the Docker Compose service names defined in `docker-compose.yml` (`dp-ingestion`, `dp-query`, `dp-annotation`):

```yaml
# query-config.yaml
queryable:
  mldp:
    mldp-pool:
      query-url: dp-query:50052
      min-conn: 1
      max-conn: 2
  mldp-pv-metadata:
    mldp-pv-metadata-pool:
      annotation-url: dp-annotation:50053
      min-conn: 1
      max-conn: 2
```

### Inline dotted assignments (no config file)

Pass URLs directly with `-c` dotted assignments instead of writing a file:

```bash
mldp_pvxs_driver \
  -c queryable.mldp.mldp-pool.query-url=dp-query:50052 \
  -c queryable.mldp.mldp-pool.min-conn=1 \
  -c queryable.mldp.mldp-pool.max-conn=2 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=dp-annotation:50053 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=2 \
  query "SHOW TABLES"
```

Override just the query URL when running against a different host:

```bash
mldp_pvxs_driver \
  -c query-config.yaml \
  -c queryable.mldp.mldp-pool.query-url=my-host:50052 \
  query "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'MY:PV' LIMIT 10"
```

Two queryable types are available:

| `type` key | Tables exposed | Backend |
|---|---|---|
| `mldp` | `mldp.time_series`, `mldp.time_series_table`, `mldp.pv_stats` | MLDP query gRPC service |
| `mldp-annotation` / `mldp-pv-metadata` | `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation`, `mldp.active_configurations` | MLDP annotation gRPC service |

---

## SQL syntax reference

The engine supports a subset of SQL designed for time-series and annotation queries.

### Statement types

```sql
SHOW TABLES
DESCRIBE <table>
EXPLAIN <select>
SELECT ...
```

### Reading `DESCRIBE`

`DESCRIBE <table>` reports the query engine's logical schema for that virtual
table. Its fields have the following meanings:

| Field | Meaning |
|---|---|
| `name` | Column or predicate-shorthand name. |
| `type` | Logical scalar type: `string`, `timestamp`, `duration_seconds`, `int`, or `bool`. The time-series `value` column carries its native Arrow union type at runtime. |
| `required` | Whether a constraining predicate is required before the table can be scanned. |
| `is_output` | Whether the field can be selected, including through `SELECT *`. `tag` is `false` because it is a predicate-only membership shorthand. |
| `pushable_ops` | Operators the client can send to its backend request. For example, `=,IN,PREFIX,CONTAINS`. |
| `filterable_ops` | Operators evaluated locally after records are fetched. |
| `notes` | Field-specific behavior, metadata source, and any pushdown/fallback details. |

An empty operator cell means the field is output-only, or that filtering is
provided through a related predicate-only field such as `tag`. A backend-pushed
criterion is an optimization: the client retains the equivalent local check
where the response contract does not guarantee identical filtering semantics.

### SELECT grammar

```
SELECT { * | column [, column ...] }
FROM   <table> [AS <alias>]
       [JOIN <table> [AS <alias>] ON <col> = <col>] ...
[WHERE <predicate> [AND <predicate>] ...]
[ORDER BY column [ASC|DESC] [, column [ASC|DESC] ...]]
[LIMIT <n>]
[PAGE TOKEN '<token>']
```

### Predicates

| Predicate | Example |
|---|---|
| Equality | `pv = 'MY:PV'` |
| Not-equal | `pv != 'MY:PV'` |
| In list | `pv IN ('PV:A', 'PV:B')` |
| Range | `time BETWEEN 1700000000 AND 1700003600` |
| Comparison | `time >= 1700000000 AND time <= 1700003600` |
| Prefix match | `pv PREFIX 'MY:MAGNET'` |
| Contains | `pv CONTAINS 'MAGNET'` |
| SQL LIKE | `description LIKE '%vacuum%'` or `name LIKE 'beam*'` |

Multiple predicates are combined with `AND`.

`ORDER BY` sorts scalar fields before projection and `LIMIT`. `ASC` is the default and `NULL` values sort last. Collection columns (`tags`, `attributes`, and `provenance`) cannot be order keys, but dynamic scalar metadata keys can:

```sql
SELECT pv, alias, attributes.device_group, attributes.ordinal, tags
FROM mldp.pv_metadata
ORDER BY attributes.device_group, attributes.ordinal;
```

`GROUP BY`, aggregates, and `HAVING` are not currently supported.

### Compact and expanded table output

Table output keeps each result on one physical line. Lists and maps show their first two values followed by `+N` when values remain; map keys are sorted for a predictable display. Use the REPL controls below to inspect every value in a record:

When enabled with `--table-fit` or `.table-fit on`, table output fits to the current interactive terminal width. Long headers and cell lines preserve their beginning and end with `...` between them. This display-only setting never truncates JSON, CSV, Arrow, expanded output, or redirected/piped output.

```text
\expanded on     # persistently enable expanded records
\expanded off    # return to compact table output
\x               # toggle expanded records
SELECT * FROM mldp.pv_metadata \G
```

The `\G` query terminator expands that one result without changing the current display mode. JSON, CSV, and Arrow output retain their complete machine-readable collections.

### Pattern matching with `LIKE`

`LIKE` matches string values case-insensitively. It supports standard SQL patterns plus `*` as a convenient alternative to `%`:

| Pattern | Meaning | Example |
|---|---|---|
| `%` or `*` | Zero or more characters | `name LIKE 'beam*'` |
| `_` | Exactly one character | `name LIKE 'sector_1'` |
| `\%`, `\*`, `\_`, `\\` | Literal `%`, `*`, `_`, or backslash | `description LIKE 'rate\\%'` |

`LIKE` is available for every string column. It is evaluated locally after records are fetched, so a broad pattern can retrieve more data than a pushable predicate. Combine it with a pushable predicate when practical:

```sql
SELECT pv, description
FROM mldp.pv_metadata
WHERE tag = 'vacuum' AND description LIKE '%interlock%'
```

`CONTAINS` remains a case-sensitive literal substring operator: wildcard characters have no special meaning with `CONTAINS`.

### Time literals

Timestamps in predicates are **Unix epoch seconds** (integer literals) or the `NOW` expression:

```sql
-- absolute epoch seconds
WHERE time >= 1700000000 AND time <= 1700003600

-- relative to current time
WHERE time >= NOW -1h AND time <= NOW
WHERE time >= NOW -30m AND time <= NOW +5m
WHERE time >= NOW -3600s AND time <= NOW
```

Duration suffixes: `s` = seconds, `m` = minutes, `h` = hours.

### Joins

```sql
-- INNER JOIN
SELECT ts.pv, ts.time, ts.value, m.description
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv = 'MY:PV' AND ts.time >= NOW -1h AND ts.time <= NOW
  AND m.pv = 'MY:PV'

-- LEFT OUTER JOIN
SELECT ts.pv, ts.time, ts.value, m.description
FROM mldp.time_series ts
LEFT JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv = 'MY:PV' AND ts.time >= NOW -1h AND ts.time <= NOW
  AND m.pv = 'MY:PV'
```

The `ON` clause must be a single equi-join condition (`left_col = right_col`).

### Dynamic metadata columns

Dynamic metadata stays attached to its base record or sample: querying tags or
attributes never creates one result row per tag or key. `tags` returns the full
tag collection, while `attributes.<key>` and `provenance.<key>` return nullable
string scalars. `tag` is predicate-only membership shorthand.

| Field family | Access | Available on | Source and filtering |
|---|---|---|---|
| Tags | Select `tags`; filter with `tag =` or `tag IN` | `mldp.time_series`, `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation` | Annotation-table criteria are sent to the annotation service and locally verified. Time-series tags come from returned bucket `dataColumn.metadata` and are filtered locally. |
| Attributes | Select `attributes`; select/filter `attributes.<key>` with `=` or `IN` | `mldp.time_series`, `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation` | Same execution path as tags. |
| Provenance | Select `provenance`; select/filter `provenance.<key>` with `=` or `IN` | `mldp.time_series` only | Returned bucket `dataColumn.metadata`; filtered locally. |

Every selected dynamic key projects as a nullable string column, whether or not
the MLDP response contains that key. Rows without the key project as `NULL` and
do not match predicates. The
time-series bucket metadata is authoritative for time-series display and
filtering; it is not replaced with current `mldp.pv_metadata` annotations,
which can differ from the metadata stored with historical samples.

`mldp.pv_stats` and `mldp.active_configurations` do not advertise dynamic
metadata fields because their gRPC responses do not provide them.

```sql
SELECT pv, attributes.units, tags
FROM mldp.pv_metadata
WHERE attributes.namespace = 'mldp_sample'
  AND tag IN ('sample', 'magnet')
```

### Pagination

For large result sets use `LIMIT` plus cursor-based pagination:

```sql
-- First page
SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'MY:PV' AND time >= NOW -24h AND time <= NOW
LIMIT 500

-- Next page — use the token from the stats footer
SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'MY:PV' AND time >= NOW -24h AND time <= NOW
LIMIT 500 PAGE TOKEN '<token-from-previous-result>'
```

The continuation token is printed in the stats footer after each paginated result.

---

## Virtual table catalog

### `mldp.time_series`

Time-series samples from the MLDP query service.

A valid selection that matches no samples returns an empty result. This also
applies when a requested PV is not returned for the selected time range.

| Column | Type | Required predicate | Pushable operators | Notes |
|---|---|---|---|---|
| `pv` | string | **yes** | `=`, `IN` | PV name. Must be constrained. |
| `time` | timestamp | no | `>=`, `<=` | UTC epoch seconds. |
| `value` | union | no | — | Typed sample value (see below). |
| `column_type` | string | no | — | Native MLDP value kind: `string`, `bool`, integer, float, `double`, `binary`, `timestamp`, `array`, `structure`, or `image`. Filter locally with `=` or `IN`. |
| `tags` | list&lt;string&gt; | no | — | Complete bucket column-metadata tag collection. Filter with `tag =` or `tag IN` locally. |
| `attributes` | map&lt;string,string&gt; | no | — | Complete bucket column-metadata attributes. Select/filter `attributes.&lt;key&gt;` locally. |
| `provenance` | map&lt;string,string&gt; | no | — | Complete bucket column-metadata provenance. Select/filter `provenance.&lt;key&gt;` locally. |
| `timeout` | duration | no | `=` | Query timeout in seconds. |
| `rpc_deadline` | duration | no | `=` | RPC deadline in seconds. |

`value` is a dense-union Arrow column that carries the native MLDP data type: `string`, `bool`, `uint32`, `uint64`, `int32`, `int64`, `float`, `double`, `binary`, `timestamp`, `array`, `structure`, or `image`.

Table and expanded output display the active union member directly (for example, a double sample renders as `10`, not Arrow's `union{double: ...}` diagnostic). JSON, CSV, and Arrow output retain the underlying union representation for machine-readable consumers.

**Required:** `pv` must be constrained with `=` or `IN`.

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv = 'MY:PV:CURRENT'
  AND time >= NOW -1h
  AND time <= NOW

SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('PV:A', 'PV:B', 'PV:C')
  AND time >= 1700000000
  AND time <= 1700003600
```

```sql
SELECT pv, time, attributes.ordinal, provenance.process, tags
FROM mldp.time_series
WHERE pv = 'MY:PV:CURRENT'
  AND attributes.namespace = 'mldp_sample'
  AND provenance.source = 'sample-generator/mldp_sample'
```

---

### `mldp.time_series_table`

Native wide time-series tables from one MLDP `TABLE_FORMAT_COLUMN` response.
`pv =` or `pv IN (...)` is required and determines the requested PV columns.
The result contains one shared `time` column followed by returned PV columns in
the requested-PV order. Each PV column keeps its native Arrow type; shorter
returned vectors are padded with trailing nulls. Each generated PV Arrow field
carries its archived column metadata as key/value entries (`tags`,
`attributes.<key>`, `provenance.source`, and `provenance.process`). This is a special runtime-shaped
table: it supports `SELECT *` only, does not support `ORDER BY` or joins, and
does not accept projection or predicates on generated PV columns. Use `time`,
`column_type`, `tag`, `attributes.<key>`, and `provenance.<key>` in `WHERE` to
select whole PV columns.

A requested PV that has no data for the selection is omitted. If no requested
PV has matching data, the query returns an empty result.

`pv` and `window` are `WHERE` predicates; they are not table arguments. A
window input accepts either one literal inclusive interval, `window IN (start,
end)`, or a subquery that returns non-null timestamp columns named `time` and
`end_time`. Literal endpoints must be timestamp-compatible expressions; `NOW`
and `NOW +/- duration` are supported, and reversed endpoints are automatically
normalized. A query cannot supply both forms. Subquery ranges must be closed;
overlapping or adjacent ranges are coalesced before the driver requests the
wide table. As with ordinary SQL filtering, a valid `pv` or `window` subquery
that finds no rows returns an empty result; malformed subquery output remains
an error.

```sql
SELECT *
FROM mldp.time_series_table
WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE')
  AND column_type = 'double'
  AND attributes.namespace = 'mldp_sample'
  AND time >= NOW -1h
  AND time <= NOW
```

```sql
-- Start endpoint first; reversed endpoints are normalized automatically.
SELECT *
FROM mldp.time_series_table
WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE')
  AND window IN (NOW - 10h, NOW)
```

```sql
SELECT *
FROM mldp.time_series_table
WHERE pv IN (
  SELECT pv
  FROM mldp.pv_metadata
  WHERE attributes.namespace = 'mldp_sample'
    AND tag = 'magnet'
)
AND window IN (
  SELECT activation.time, activation.end_time
  FROM mldp.configuration_activation activation
  JOIN mldp.configuration configuration
    ON activation.config_name = configuration.name
  WHERE activation.attributes.namespace = 'mldp_sample'
    AND configuration.attributes.namespace = 'mldp_sample'
    AND configuration.category = 'beam_mode'
    AND activation.end_time IS NOT NULL
)
```

---

### `mldp.pv_stats`

Per-PV bucket statistics (first/last timestamp, bucket count).

| Column | Type | Required predicate | Pushable operators | Notes |
|---|---|---|---|---|
| `pv` | string | **yes** | `=`, `IN` | PV name. Must be constrained. |
| `first_timestamp` | timestamp | no | — | Earliest recorded sample. |
| `last_timestamp` | timestamp | no | — | Most recent recorded sample. |
| `num_buckets` | int | no | — | Number of storage buckets. |

```sql
SELECT pv, first_timestamp, last_timestamp, num_buckets
FROM mldp.pv_stats
WHERE pv IN ('PV:A', 'PV:B')
```

`mldp.pv_stats` does not expose tags, attributes, or provenance because the
query-service statistics response contains aggregate bucket data only.

---

### `mldp.pv_metadata`

PV metadata and annotation records from the MLDP annotation service.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `pv` | string | `=`, `IN`, `PREFIX`, `CONTAINS`, `LIKE` (local) | PV name or alias. |
| `alias` | string | `=`, `IN`, `PREFIX`, `CONTAINS`, `LIKE` (local) | Alternate name. |
| `tag` | string | `=`, `IN` | Predicate-only tag membership shorthand. |
| `tags` | list&lt;string&gt; | — | Complete tag collection. Filter with `tag =` or `tag IN`; criteria are backend-pushed when supported and locally verified. |
| `attributes` | map&lt;string,string&gt; | — | Complete dynamic attribute collection. Select/filter `attributes.&lt;key&gt;`; criteria are backend-pushed when supported and locally verified. |
| `description` | string | `LIKE` (local) | Free-text description. |
| `created_time` | timestamp | — | Record creation time. |
| `updated_time` | timestamp | — | Last modification time. |
| `modified_by` | string | `LIKE` (local) | Last modifier identity. |

An unfiltered query lists all PV metadata records. Predicates narrow the list on the annotation service.

`LIKE` is available on every string column and runs as a local filter; see [Pattern matching with `LIKE`](#pattern-matching-with-like) for its wildcard and escaping rules.

Dynamic attributes are accessible as `attributes.<key>` and support `=` and `IN`. Missing keys project as `NULL` and do not match filters.

```sql
-- Find PVs by prefix
SELECT pv, description, attributes.units, tags
FROM mldp.pv_metadata
WHERE pv PREFIX 'MY:MAGNET'

-- Find by tag
SELECT pv, alias, description
FROM mldp.pv_metadata
WHERE tag = 'production'

-- Case-insensitive description search
SELECT pv, description
FROM mldp.pv_metadata
WHERE description LIKE '%vacuum%'
```

---

### `mldp.configuration`

Machine/beam configuration records.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `name` | string | `=`, `IN`, `PREFIX`, `CONTAINS`, `LIKE` (local) | Configuration name. |
| `category` | string | `=`, `IN`, `LIKE` (local) | Configuration category. |
| `parent` | string | `=`, `IN`, `LIKE` (local) | Parent configuration name. |
| `tags` | list&lt;string&gt; | — | Complete tag collection. Filter with `tag =` or `tag IN`; criteria are backend-pushed and locally verified. |
| `attributes` | map&lt;string,string&gt; | — | Complete dynamic attribute collection. Select/filter `attributes.&lt;key&gt;`; criteria are backend-pushed and locally verified. |
| `tag` | string | `=`, `IN` | Predicate-only tag membership shorthand. |
| `description` | string | `LIKE` (local) | Free-text description. |
| `created_time` | timestamp | — | Creation time. |
| `updated_time` | timestamp | — | Last modification time. |
| `modified_by` | string | `LIKE` (local) | Last modifier identity. |

An unfiltered query lists all configurations. Predicates narrow the list on the annotation service.

```sql
SELECT name, category, description
FROM mldp.configuration
WHERE category = 'beam_mode'

-- `%` and `*` are equivalent any-length wildcards
SELECT name, description
FROM mldp.configuration
WHERE name LIKE 'beam*'
```

---

### `mldp.configuration_activation`

Time-windowed activation records for configurations.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `time` | timestamp | `=`, `>=`, `<=` | Activation window start time. |
| `end_time` | timestamp | `IS NOT NULL` (local) | Activation end time; null means the activation is open. |
| `config_name` | string | `=`, `IN`, `LIKE` (local) | Configuration name. |
| `activation_id` | string | `=`, `IN`, `LIKE` (local) | Client-assigned activation identifier. |
| `description` | string | `LIKE` (local) | Free-text description. |
| `tags` | list&lt;string&gt; | — | Complete tag collection. Filter with `tag =` or `tag IN`; criteria are backend-pushed and locally verified. |
| `attributes` | map&lt;string,string&gt; | — | Complete dynamic attribute collection. Select/filter `attributes.&lt;key&gt;`; criteria are backend-pushed and locally verified. |
| `tag` | string | `=`, `IN` | Predicate-only tag membership shorthand. |
| `created_time` | timestamp | — | Record creation time. |
| `updated_time` | timestamp | — | Last modification time. |

At least one pushable predicate is required.

```sql
-- Activations for a specific configuration
SELECT time, config_name, activation_id, description
FROM mldp.configuration_activation
WHERE config_name = 'injector_tuning'

-- Activations in a time window
SELECT time, config_name, activation_id
FROM mldp.configuration_activation
WHERE time >= NOW -2h AND time <= NOW
```

---

### `mldp.active_configurations`

Configurations that were active at a given point in time. **Requires exactly one `at = <epoch>` predicate.**

| Column | Type | Required predicate | Notes |
|---|---|---|---|
| `at` | timestamp | **yes** (`=` only) | Point in time to query. Must be constrained. |
| `name` | string | — | Active configuration name. |
| `activation_id` | string | — | Activation identifier. |
| `time` | timestamp | — | Activation start time. |

```sql
SELECT name, activation_id, time
FROM mldp.active_configurations
WHERE at = 1700000000

-- Using NOW
SELECT name, activation_id
FROM mldp.active_configurations
WHERE at = NOW -30m
```

`mldp.active_configurations` does not expose tags, attributes, or provenance:
the annotation response provides only the active configuration identity and
activation time.

---

## Output formats

| `--format` | Description |
|---|---|
| `table` | psql-style ASCII table with column headers and separator (default). |
| `json` | One JSON object per row, newline-delimited. |
| `csv` | RFC 4180 CSV with header row. |
| `arrow` | Apache Arrow IPC stream (binary). Suitable for programmatic consumption. |

Collection metadata stays native in every format: table output shows all
collection entries in one cell (one entry per line), JSON emits arrays and
objects, CSV stores canonical JSON in a quoted cell, and Arrow IPC preserves
the list/map types.

Example `table` output for `SHOW TABLES`:

```
table_name
-------------------------------
mldp.active_configurations
mldp.configuration
mldp.configuration_activation
mldp.pv_metadata
mldp.pv_stats
mldp.time_series
(6 rows)
-- 6 rows (6 from backend, 0 filtered) in 0ms | 0 RPC | 0 bytes spilled | 0 MB peak
```

---

## Tuning notes

- Increase `--memory-mb` when joins spill and memory is available.
- Set `--spill-dir` to fast local storage (NVMe) when spill is expected on large joins.
- Reduce `--join-batch-size` to lower peak memory per batch; increase it to reduce RPC round-trips.
- For unbounded table joins the planner emits a warning: _"joining two unbounded sides; spill is expected under memory pressure"_ — add time or PV predicates to bound at least one side.
- Deep engine internals are documented in [Query Engine Architecture](../reference/query-engine-architecture.md).

---

## Tutorial: first queries with sample data

This tutorial walks from zero to running SQL queries against a live MLDP stack using the sample-data generator.

### Prerequisites

- MLDP stack running inside the devcontainer (ingestion, query, and annotation services).
- Python gRPC dependencies:

  ```bash
  python3 -m pip install grpcio grpcio-tools protobuf
  ```

- A built `mldp_pvxs_driver` binary (or the dev container with the binary on `$PATH`).

### Step 1 — Populate sample data

Run the sample-data generator from the project root:

```bash
python3 scripts/generate_mldp_sample_data.py
```

This creates:
- **20 PVs** cycling through four device families: `mldp_sample:MAGNET:01:VALUE`, `mldp_sample:RF:02:VALUE`, `mldp_sample:VACUUM:03:VALUE`, `mldp_sample:DIAGNOSTIC:04:VALUE`, `mldp_sample:MAGNET:05:VALUE`, …, `mldp_sample:DIAGNOSTIC:20:VALUE`
- **3600 time-series samples** per PV (deterministic sine waves) at 1-second intervals ending roughly at "now"
- PV metadata with tags (`sample`, `mldp_sample`, device family) and attributes (`namespace`, `device_group`, `ordinal`, `units`, `sample_period_seconds`)
- **4 configurations:** two `beam_mode` (`mldp_sample_injector_tuning`, `mldp_sample_user_delivery`), one `rf` (`mldp_sample_rf_station_a`), one `vacuum` (`mldp_sample_vacuum_ready`)
- **4 activation windows:** beam-mode activations are closed (the two most recent hours, adjacent); RF and vacuum activations are open (started within the last 30 and 15 minutes)

The script prints the exact PV names, time range, and an example query when it finishes:

```
Generated MLDP sample namespace: mldp_sample
Provider: mldp_sample-sample-provider (<id>)
Time series: 20 PVs x 3600 samples at 1-second intervals
Time range: [<start>, <start+3599>] UTC epoch seconds
Configurations: mldp_sample_injector_tuning, mldp_sample_user_delivery, mldp_sample_rf_station_a, mldp_sample_vacuum_ready
Activation IDs: mldp_sample_injector_tuning_activation, mldp_sample_user_delivery_activation, mldp_sample_rf_station_a_activation, mldp_sample_vacuum_ready_activation
Example query: SELECT pv, time, value FROM mldp.time_series WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
```

To use a different namespace prefix:

```bash
python3 scripts/generate_mldp_sample_data.py --namespace accelerator_demo
```

To verify the metadata was written correctly:

```bash
python3 scripts/generate_mldp_sample_data.py --verify
```

To clean up annotation records when done:

```bash
python3 scripts/generate_mldp_sample_data.py --drop-namespace
```

> **Note:** `--drop-namespace` deletes only PV metadata, configurations, and activations. Ingested time-series samples remain in MLDP storage because the gRPC API has no bucket-deletion operation.

### Step 2 — Create a query config

Save the following as `query-config.yaml` (adjust URLs if your MLDP stack uses different hostnames):

```yaml
queryable:
  mldp:
    mldp-pool:
      query-url: dp-query:50052
      min-conn: 1
      max-conn: 2
  mldp-pv-metadata:
    mldp-pv-metadata-pool:
      annotation-url: dp-annotation:50053
      min-conn: 1
      max-conn: 2
```

### Step 3 — Explore the schema

```bash
# List all available virtual tables
mldp_pvxs_driver -c query-config.yaml query "SHOW TABLES"

# Inspect time-series table columns
mldp_pvxs_driver -c query-config.yaml query "DESCRIBE mldp.time_series"

# Inspect metadata table columns
mldp_pvxs_driver -c query-config.yaml query "DESCRIBE mldp.pv_metadata"
```

### Step 4 — Fetch time-series samples

```bash
# Last 10 minutes of samples for one MAGNET PV
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, time, value
   FROM mldp.time_series
   WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
     AND time >= NOW -10m
     AND time <= NOW"
```

```bash
# Multiple PVs from different device families, CSV output
mldp_pvxs_driver -c query-config.yaml query --format csv \
  "SELECT pv, time, value
   FROM mldp.time_series
   WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
                'mldp_sample:RF:02:VALUE',
                'mldp_sample:VACUUM:03:VALUE',
                'mldp_sample:DIAGNOSTIC:04:VALUE')
     AND time >= NOW -1h
     AND time <= NOW" \
  > samples.csv
```

```bash
# Include provenance metadata attached to time-series samples
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, time, value, provenance.source, provenance.process, tags
   FROM mldp.time_series
   WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
     AND time >= NOW -5m
     AND time <= NOW"
```

### Step 5 — Check PV availability

```bash
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, first_timestamp, last_timestamp, num_buckets
   FROM mldp.pv_stats
   WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
                'mldp_sample:RF:02:VALUE',
                'mldp_sample:VACUUM:03:VALUE',
                'mldp_sample:DIAGNOSTIC:04:VALUE')"
```

### Step 6 — Explore metadata

```bash
# All sample-namespace PVs
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, description, attributes.device_group, attributes.units, modified_by
   FROM mldp.pv_metadata
   WHERE pv PREFIX 'mldp_sample'"

# PVs tagged 'magnet' (MAGNET family PVs: 01, 05, 09, 13, 17)
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, alias, description, attributes.device_group, attributes.ordinal, tags
   FROM mldp.pv_metadata
   WHERE tag = 'magnet'"

# PVs filtered by namespace attribute
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, attributes.device_group, attributes.ordinal
   FROM mldp.pv_metadata
   WHERE attributes.namespace = 'mldp_sample'
   ORDER BY attributes.ordinal"
```

### Step 7 — Query configurations

```bash
# All sample configurations
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT name, category, description
   FROM mldp.configuration
   WHERE name PREFIX 'mldp_sample'"

# Only beam-mode configurations
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT name, category, description
   FROM mldp.configuration
   WHERE category = 'beam_mode'"

# Closed activation windows for beam-mode configurations
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT time, end_time, config_name, activation_id
   FROM mldp.configuration_activation
   WHERE config_name IN ('mldp_sample_injector_tuning', 'mldp_sample_user_delivery')
     AND end_time IS NOT NULL"

# Active configurations 30 minutes ago (RF and vacuum should be active)
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT name, activation_id, time
   FROM mldp.active_configurations
   WHERE at = NOW -30m"
```

### Step 8 — Join time-series with metadata

```bash
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT ts.pv, ts.time, ts.value, m.description, m.attributes.units
   FROM mldp.time_series ts
   JOIN mldp.pv_metadata m ON ts.pv = m.pv
   WHERE ts.pv PREFIX 'mldp_sample:MAGNET'
     AND ts.time >= NOW -10m
     AND ts.time <= NOW
     AND m.pv PREFIX 'mldp_sample:MAGNET'"
```

### Step 9 — Use EXPLAIN to inspect the query plan

```bash
mldp_pvxs_driver -c query-config.yaml query \
  "EXPLAIN SELECT pv, time, value FROM mldp.time_series
   WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
     AND time >= NOW -1h AND time <= NOW"
```

### Step 10 — Save SQL to a file

For longer queries, write the SQL to a file and use `--file`:

```bash
cat > my_query.sql <<'EOF'
SELECT ts.pv,
       ts.time,
       ts.value,
       m.description
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv PREFIX 'mldp_sample'
  AND ts.time >= NOW -1h
  AND ts.time <= NOW
  AND m.pv PREFIX 'mldp_sample'
LIMIT 200
EOF

mldp_pvxs_driver -c query-config.yaml query --file my_query.sql
```

### Step 11 — Discover PVs and closed beam-mode windows in one query

`mldp.time_series_table` also accepts its required `pv` input and an optional
`window` input. Use `window IN (start, end)` for one literal inclusive range,
or a subquery for activation ranges. Literal endpoints are normalized into
ascending order. The metadata subquery is evaluated first to produce the
ordered PV list, then the activation subquery produces closed time ranges. MLDP
receives one wide-table request for a literal interval or each normalized
subquery range; overlap and directly adjacent subquery ranges are coalesced, so
no batch crosses a gap.

```sql
SELECT *
FROM mldp.time_series_table
WHERE pv IN (
  SELECT pv
  FROM mldp.pv_metadata
  WHERE attributes.namespace = 'mldp_sample'
    AND tag = 'magnet'
)
AND window IN (
  SELECT activation.time, activation.end_time
  FROM mldp.configuration_activation activation
  JOIN mldp.configuration configuration
    ON activation.config_name = configuration.name
  WHERE activation.attributes.namespace = 'mldp_sample'
    AND configuration.attributes.namespace = 'mldp_sample'
    AND configuration.category = 'beam_mode'
    AND activation.end_time IS NOT NULL
)
```

The subquery output must be a single non-null string field named `pv`, and two
non-null timestamp fields named `time` and `end_time` (qualified names such as
`activation.time` are accepted). Open or inverted activation ranges are
rejected. Each returned batch is `time` followed by the requested native PV
columns in metadata-query order.

### Step 12 — Materialize a table for this session or future clients

Use `CREATE TEMP TABLE` when a materialized query result is needed only within
the current interactive client session. The table is an immutable Arrow IPC
snapshot: later statements in that session can select or join it, but the
catalog removes it when the client exits. It is also removed by `DROP TABLE`.

```sql
CREATE TEMP TABLE magnet_samples AS
SELECT pv, time, value
FROM mldp.time_series
WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
  AND time >= NOW -10m
  AND time <= NOW;

SELECT * FROM magnet_samples;
DROP TABLE magnet_samples;
```

Use `CREATE TABLE` without `TEMP` for a persistent immutable snapshot. It is
visible to later CLI runs that use the same catalog directory:

```bash
mldp_pvxs_driver -c query-config.yaml query \
  --table-catalog-dir /var/lib/mldp/query-catalog \
  "CREATE TABLE magnet_samples AS
   SELECT pv, time, value
   FROM mldp.time_series
   WHERE pv = 'mldp_sample:MAGNET:01:VALUE'"

mldp_pvxs_driver -c query-config.yaml query \
  --table-catalog-dir /var/lib/mldp/query-catalog \
  "SELECT * FROM magnet_samples"
```

Set the catalog location with `query --table-catalog-dir <path>`. If it is not
specified, the CLI uses `<tmp>/mldp-query-catalog`; choose a stable, writable
directory such as `/var/lib/mldp/query-catalog` for tables that must survive
multiple client runs. The catalog stores its managed files only in
`<path>/.mldp-query-tables/`; it does not clean unrelated files. `CREATE TABLE`
fails if the name already exists, so run `DROP TABLE magnet_samples` before
recreating a persistent snapshot. Use a shared mounted catalog directory when
multiple processes or hosts need to access the same snapshots.
