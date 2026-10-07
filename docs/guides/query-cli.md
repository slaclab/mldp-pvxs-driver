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
  - [Expressions and operators](#expressions-and-operators)
- [Query-only configuration](#query-only-configuration)
  - [Config file](#config-file)
  - [Inline dotted assignments](#inline-dotted-assignments-no-config-file)
- [SQL syntax reference](#sql-syntax-reference)
  - [Statement types](#statement-types)
  - [SELECT grammar](#select-grammar)
  - [One row per key: `DISTINCT ON`](#one-row-per-key-distinct-on)
  - [Grouping and aggregates: `GROUP BY` / `HAVING`](#grouping-and-aggregates-group-by--having)
  - [Predicates](#predicates)
  - [Time literals](#time-literals)
  - [Native value predicates](#native-value-predicates)
  - [Joins](#joins)
  - [Pagination](#pagination)
- [Virtual table catalog](#virtual-table-catalog)
- [Output formats](#output-formats)
- [Tuning notes](#tuning-notes)
- [Tutorial: first queries with sample data](#tutorial-first-queries-with-sample-data)
  - [Persistent table across client sessions](#persistent-table-across-client-sessions)

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
| `--table-fit` | off | Wrap table headers and cells to an interactive terminal viewport (fixed column widths, rows grow taller); ignored when output is redirected or piped. |
| `--no-stats` | off | Suppress the final textual query-statistics line. |
| `--trace-shards` | off | Emit window-shard diagnostics to stderr. |
| `--trace-shards-file <path>` | — | Enable window-shard diagnostics and write them to a newly truncated file. |
| `--memory-mb <n>` | `256` | Memory budget for the execution context (MiB). |
| `--spill-dir <path>` | `<tmp>/mldp-query-spill` | Directory for spill files under memory pressure. |
| `--table-catalog-dir <path>` | `<tmp>/mldp-query-catalog` | Root directory for durable Arrow IPC snapshots; separate from `--spill-dir`. |
| `--spill-partitions <n>` | `16` | Spill partition count for join and `GROUP BY` spill paths. |
| `--join-batch-size <n>` | `100` | Batch size hint for join execution and pagination. |

### Interactive session

Run `query` without positional SQL or `--file` to start the REPL:

```bash
mldp_pvxs_driver -c query-config.yaml query
mldp> SHOW TABLES;
mldp> SHOW FUNCTIONS;
mldp> SHOW OPERATORS;
```

Terminate each statement with a semicolon. Statements can span lines; the prompt changes from `mldp> ` to `...> ` while a statement is buffered. A semicolon inside a quoted string does not terminate the statement. The session executes one statement at a time and remains open after parse, planning, or execution errors.

Table and expanded results use normal terminal scrollback, so SSH and container-attached terminals retain their native resize, copy/paste, and scroll behavior. A direct-output query in a real terminal temporarily shows a progress footer with `Ctrl-C cancel`; it is removed before the next prompt. Completed results then end with the normal textual `-- ...` statistics line unless `--no-stats` is set. Redirected/plain-stream REPL output and one-shot commands retain the same textual statistics line, and machine-readable formats remain free of terminal control sequences.

### Pager and terminal controls

The REPL uses `replxx` for all interactive terminal sessions. It clears the terminal immediately before each submitted SQL statement, exactly as `.clear` does. For direct table or expanded output, it temporarily reserves the final row for a reverse-video progress footer while the query runs, then restores the full terminal before printing the final statistics and returning to `mldp> `. Idle prompt editing is wholly owned by `replxx`; there is no pinned footer after completion, cancellation, or error. `Ctrl-C` cancels an active query or abandons the current editor line; `Ctrl-Q`, `.quit`, and `.exit` leave the REPL. Pager output does not activate the footer.

Use `.pager on` to send table or expanded output from a real terminal to a pager. Paging is off by default. The pager command comes from `$PAGER`, or defaults to `less -FRSX` when `$PAGER` is unset. `.pager` reports the current setting and `.pager off` restores direct scrollback output. JSON, CSV, Arrow, one-shot SQL, and redirected sessions always use direct formatter output.

### Query progress

The active-query footer and command listeners receive query progress while a query runs. Depending on the active step, it identifies the source table, operation, detail, result page, window-shard position, parallel-shard capacity, RPC progress, cursor progress, and backend row count. `shards <active>/<limit>` means active PV-group cursors over the configured per-slice concurrency limit, which MLDP derives from `max-conn`.

| State or step | Progress information |
|---|---|
| `parsing` | SQL statement parsing is in progress. |
| `planning` | The logical and physical query plan is being prepared. |
| `formatting` | The result stream is being prepared for the selected table, JSON, CSV, or Arrow output format. |
| `backend RPC` | A table scan or window shard has opened a server cursor. The footer can show `<completed>/<started> RPCs` and backend rows as batches arrive. |
| `executing` — MLDP bidi cursor | Shows `mldp.time_series`, `MLDP bidi cursor`, `cursor response` or `cursor next`, and `cursor <responses>, next <requests>`. A response is an Arrow batch received from MLDP; `next` is a request for the next cursor batch. |
| `executing` — windowed MLDP scan | Shows the active source plus `window <n>, slice <n>, series shard <n>`. Windows are visited in normalized-range, time-slice, then requested-series-group order. This corresponds to `window IN (...; slice ..., series_per_shard ...)`. |
| `executing` — wide pivot | Shows `mldp.time_series_table` and a stage such as `wide pivot`, `wide pivot ingestion`, spill finalization, or external sort/merge while the long-form cursor data is converted to the wide result. |
| `executing` — relational stage | Shows the local stage that is consuming batches, such as filter, projection, sort, limit, hash join, nested-loop join, or block nested-loop join. |
| `cancelling` | `Ctrl-C` has requested cancellation until the in-flight query releases its cursor or RPC. |

For a paginated `LIMIT` query, the status can show `result page <n>`. This is the client result-page number, separate from MLDP cursor responses. The continuation token remains available for `PAGE TOKEN` in the plain-stream session.

When a real-terminal query finishes, the footer is removed and the normal result/statistics output remains in scrollback. Query errors and `Query cancelled` are likewise written to normal scrollback before the next usable prompt.

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
`FROM` and `JOIN` positions. An alias is optional for a single unqualified
source (`SELECT pv FROM (SELECT pv FROM ...)`), but is required when SQL needs
to qualify its fields (`recent.pv`) or distinguish it from another source. Any
column that supports `IN` accepts literals or a single-column `SELECT`; the
child output is consumed by position, must be non-null, and must be
type-compatible with the target column. `mldp.time_series_table` retains
positional `window IN (SELECT start, end ...)` input. Scalar subqueries remain
unsupported.

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

### LIMIT on annotation tables

The annotation service paginates `queryPvMetadata`, `queryConfigurations` and
`queryConfigurationActivations`. `LIMIT` is pushed into those requests when the
plan between the limit and the scan preserves cardinality — that is, when the
query has no residual filter, join, sort, pivot or aggregate. `EXPLAIN` shows
the pushed value as `row_limit=` on the scan node.

Two cases deliberately keep the full fetch:

- Predicates on `tag`, `attributes.<key>` and on `mldp.configuration_activation`
  `start_time` / `end_time` are re-verified locally (the backend criteria are a
  candidate-set optimization only), so they leave a residual filter above the
  scan and suppress the pushdown.
- `SELECT *`, or any query that selects the whole `attributes` map, derives its
  `attributes.<key>` columns from the union of every returned row, so all pages
  must be read before the first batch can be emitted. An explicit select list
  that does not project `attributes` streams one gRPC page at a time instead.

`mldp.active_configurations` has no paginated RPC, so `LIMIT` there is always
applied locally.

| Command | Purpose |
|---|---|
| `.help` | Show statement, command, and editing usage. |
| `.clear` | Discard the buffered statement and clear/redraw the interactive terminal. With redirected input, print a confirmation instead. |
| `.history`, `history` | Print the command history. In an interactive terminal this includes saved history from earlier sessions. |
| `.format` | Print the current output style. |
| `.format <table\|json\|csv\|arrow>` | Set the output style for subsequent statements in this REPL session. |
| `.table-fit [on\|off]` | Show or change whether table output is wrapped to the interactive terminal width for this REPL session. |
| `.quit`, `.exit` | Exit the session. |

### Line editing and completion

When both standard input and output are interactive terminals, the REPL provides shell-style editing. Use Tab to complete SQL keywords, REPL commands, output styles, table names registered by the active `queryable:` configuration, active session tables, persistent Arrow IPC tables in `--table-catalog-dir`, and columns after a table alias. Completion is case-insensitive and prefix-based; it does not complete inside quoted literals. Stored-table columns come from their Arrow IPC schema. Table completion is available after `FROM`, `JOIN`, `DESCRIBE`/`DESC`, and `DROP TABLE`. When more than one candidate matches, the terminal displays the choices and keeps the current input.

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

The query engine accepts scalar functions in constant `WHERE` values and `SELECT` projections. Function names are case-insensitive and calls may be nested. The built-in `to_utc` converts a user-facing timestamp into the query engine's UTC epoch-second timestamp value.

```sql
SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'MY:PV'
  AND time >= to_utc('2026-07-23T09:00:00-07:00');

SELECT pv FROM mldp.time_series
WHERE time >= to_utc('2026-07-23 09:00:00', '-07:00');
```

The one-argument form requires `Z` or an explicit `+/-HH:MM` offset. The two-argument form currently accepts an explicit offset. Results are truncated to epoch-second precision.

`from_utc(timestamp, zone_or_offset)` is a `SELECT` projection function that renders a UTC timestamp as an ISO-8601 string in an IANA timezone or a fixed numeric offset. IANA zones apply the offset in effect for each instant, including daylight saving time; fixed offsets do not change.

```sql
SELECT config_name,
       from_utc(start_time, 'America/Los_Angeles') AS pacific_time,
       from_utc(end_time, '-07:00') AS fixed_offset_end_time
FROM mldp.configuration_activation
WHERE start_time >= NOW-120d;
```

Numeric offsets must be quoted and use `+/-HH:MM` form, for example `'-07:00'` or `'+05:30'`. A bare `-7:00` is not a timezone argument. Null timestamps return null strings; unknown IANA zones and malformed offsets fail the query.

### Callable discovery and type semantics

`SHOW FUNCTIONS` and `SHOW OPERATORS` list the executable scalar-language catalog used by the planner. They are useful for feature discovery and return normal Arrow query results, so all output formats work consistently. Function rows contain `name`, `arguments`, `returns`, `description`, and `example`; operator rows contain `symbol`, `arity`, `arguments`, `returns`, `description`, and `example`.

The catalog is sorted deterministically by function name or operator symbol and signature. Scalar call names are case-insensitive. Operator/function overloads exclude `native_value`; mismatched argument types fail during planning. `SHOW FUNCTIONS` currently lists `from_utc(timestamp, string)`, `to_utc(string)`, and `to_utc(string, string)`; use `SHOW OPERATORS` for the exact supported operator signatures.

### Expressions and operators

`SELECT` items and `ORDER BY` items accept column references, literals,
functions, parenthesized expressions, unary signs, arithmetic, comparisons,
and boolean operators. Operators bind with the following precedence, from
highest to lowest:

1. Unary `+`, `-`, `NOT`
2. `*`, `/`
3. `+`, `-`
4. `=`, `!=`, `<`, `<=`, `>`, `>=`
5. `AND`
6. `OR`

Duration literals are a non-negative integer immediately followed by a
case-insensitive unit suffix. The complete supported set is `s`/`S` for
seconds, `m`/`M` for minutes, `h`/`H` for hours, and `d`/`D` for fixed
24-hour (86,400-second) days. They retain nanosecond precision in expressions
after conversion. Weeks, milliseconds, and compound forms such as `1h30m` are
not duration literals; write their equivalent in one supported unit (for
example, `36h`, `2d`, `90m`, or `5s`). For example, `activation.start_time + 2s` produces a timestamp two seconds after
`activation.start_time`. Existing `NOW`, `NOW + 2s`, and `NOW - 10m` predicate
syntax remains available.

```sql
SELECT value + 1 AS next_value,
       activation.start_time + 2s AS interval_end
FROM mldp.configuration_activation activation;

SELECT value * 2, value + 1
FROM mldp.time_series
ORDER BY value + 1 DESC;
```

An unaliased computed projection receives a normalized name: lowercase the
rendered expression, replace punctuation and whitespace runs with `_`, then
trim outer `_`. For example, `activation.start_time + 2s` becomes
`activation_time_2s`; `AS name` always takes precedence. Plain column output
names are unchanged.

Expression evaluation propagates null inputs to a null result. A filter keeps
only `true`; `false` and null do not match. Invalid overloads, unsupported
runtime value types, divide-by-zero, and malformed function inputs report an
operator or callable-specific query error.

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

### Window shard trace

Pass `--trace-shards` to emit one stderr diagnostic line for every windowed MLDP series shard after the query completes or fails. Each line reports driver-observed first-batch order, window/slice/shard identity, PV group, requested time range, first-batch and terminal-observation timing, batch and row counts. These are client-side asynchronous-pull timestamps, not server-side Mongo execution timestamps. Query results remain on stdout in the selected format.

Pass `--trace-shards-file <path>` to enable the same trace and write it to a newly truncated file instead of stderr. The file is created before planning, so an invalid path fails the command before it issues a query.

```bash
mldp_pvxs_driver -c query-config.yaml query --trace-shards \
  "SELECT * FROM mldp.time_series_table WHERE pv IN (...) AND window IN (...; slice 5s, series_per_shard 2)"

mldp_pvxs_driver -c query-config.yaml query --trace-shards-file shard-trace.log \
  "SELECT * FROM mldp.time_series_table WHERE pv IN (...) AND window IN (...; slice 5s, series_per_shard 2)"
```

For a production-shaped wide-window investigation under GDB, use `scripts/show-arrow-table.sh`. It defaults to `build/bin/mldp_pvxs_driver`, `test-query.sql`, `host.docker.internal:50052`, a four-connection query pool, and writes `spear-user-wide-window-trace.log`. Override a default with environment variables such as `MLDP_QUERY_URL`, `MLDP_ANNOTATION_URL`, `MLDP_QUERY_MAX_CONN`, `MLDP_WIDE_WINDOW_QUERY_FILE`, or `MLDP_WIDE_WINDOW_TRACE_FILE`.

To inspect an Arrow IPC spill or catalog file with pandas, run `python3 scripts/show-arrow-table.py <file-name>`. Add `--all` to inspect every `.arrow` file below a directory. The helper converts dense-union columns such as the native time-series `value` field to their active Python values before rendering, so catalog snapshots can be inspected directly. It needs `pandas` and `pyarrow` in that Python environment.

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
DESC <table>                 -- shorthand for DESCRIBE
EXPLAIN <select>
SELECT ...
```

### Reading `DESCRIBE`

`DESCRIBE <table>` reports the query engine's logical schema for that virtual
table. Its fields have the following meanings:

`DESC <table>` is an exact shorthand for `DESCRIBE <table>`.

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
SELECT [DISTINCT | DISTINCT ON (expr [, expr ...])] { * | expr [AS alias] [, ...] }
FROM   <table> [AS <alias>]
       [JOIN <table> [AS <alias>] ON <col> = <col>] ...
[WHERE <predicate> [AND <predicate>] ...]
[GROUP BY expr [, expr ...]]
[HAVING <condition>]
[WINDOW <name> AS (<window spec>) [, ...]]
[ORDER BY expr [ASC|DESC] [, expr [ASC|DESC] ...]]
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

`SELECT DISTINCT` drops duplicate output rows, comparing every projected column (a `NULL` equals another `NULL`). It works on virtual tables, stored tables, derived tables and joins, and the first occurrence of each row is kept, so `ORDER BY` order is preserved. `LIMIT` counts distinct rows, so it is not pushed to the backend scan when `DISTINCT` is present. In the interactive pager, de-duplication spans every page of one query.

```sql
SELECT DISTINCT attributes.device_group FROM mldp.pv_metadata;
```

### One row per key: `DISTINCT ON`

`DISTINCT` compares *every* selected column, and `distinct(col)` is not a
function — the parentheses do not limit it to `col`. To keep one row per key
while showing other columns, use `DISTINCT ON (keys)`: it keeps the first row
for each distinct key combination and returns all selected columns. The keys
need not be selected. Combine it with `ORDER BY` to choose which row is kept.

```sql
-- One line per configuration, with its description
SELECT DISTINCT ON (config_name) config_name, description
FROM mldp.configuration_activation;

-- Most recent activation per configuration
SELECT DISTINCT ON (config_name) config_name, start_time, activation_id
FROM mldp.configuration_activation
ORDER BY start_time DESC;
```

### Grouping and aggregates: `GROUP BY` / `HAVING`

`GROUP BY` collapses rows with equal keys into one row and computes aggregates
over each group. Without `GROUP BY`, aggregates in the select list compute one
row over the whole result (and still return one row for empty input).

| Aggregate | Result | Notes |
|---|---|---|
| `COUNT(*)` | int | Rows in the group, including nulls. |
| `COUNT(x)` / `COUNT(DISTINCT x)` | int | Non-null values / distinct non-null values. |
| `SUM(x)` | int or double | Numeric only; double when any value is floating point. |
| `AVG(x)` | double | Numeric only. |
| `MIN(x)` / `MAX(x)` | type of `x` | Numbers, strings, timestamps; native `value` compares numerically. |
| `FIRST(x)` / `LAST(x)` | type of `x` | First / last non-null value in input order. |

Rules:

- Every selected column must be a `GROUP BY` key or appear inside an aggregate
  (`SELECT config_name, description ... GROUP BY config_name` is an error; use
  `FIRST(description)` or `DISTINCT ON`).
- `SELECT *` cannot be combined with `GROUP BY`.
- `HAVING` filters groups and may use aggregates; `WHERE` filters input rows
  before grouping and cannot.
- `ORDER BY` may reference a group key, an aggregate, a select alias, or a
  select position (`ORDER BY 2 DESC`).
- Output names: aggregates are named `count`, `max_time`, `count_distinct_pv`,
  … unless given an `AS` alias.
- Groups are returned in first-appearance order unless `ORDER BY` is given.

Grouping runs **locally on the downloaded rows**: MLDP has no server-side
aggregation, so `GROUP BY` over `mldp.time_series` still fetches every sample
in the selection. Narrow the query with `pv`, `time`/`window`, and `config_*`
predicates first. `LIMIT` applies to groups and is not pushed to the backend.

Execution is streaming: each backend page is folded into the groups and then
released, so memory follows the number of groups, not the number of samples.
When group state exceeds `--memory-mb` (256 MiB by default), new keys are
hash-partitioned to spill files in `--spill-dir` (`--spill-partitions` files)
and aggregated one partition at a time; results are identical, only group
order changes.
`EXPLAIN` shows the step as `PhysicalAggregate(keys=…, aggregates=…)`, and
`SHOW FUNCTIONS` lists the aggregates with `kind = aggregate`.

```sql
-- Activation count and span per configuration, busiest first
SELECT config_name, COUNT(*) AS n, MIN(start_time), MAX(start_time)
FROM mldp.configuration_activation
GROUP BY config_name
HAVING COUNT(*) > 1
ORDER BY n DESC
LIMIT 10;

-- Per-PV sample statistics over the last hour
SELECT pv, COUNT(*), AVG(value), MIN(value), MAX(value)
FROM mldp.time_series
WHERE pv PREFIX 'RF:' AND time >= NOW - 1h
GROUP BY pv;

-- Whole-table totals
SELECT COUNT(*), COUNT(DISTINCT config_name) FROM mldp.configuration_activation;
```

### Window functions: `OVER`

A window function computes one value **per row** from neighbouring rows — the
previous sample, a running total, a moving average, a rank — without
collapsing rows the way `GROUP BY` does.

```
func(args) OVER ( [<window name>] [PARTITION BY expr, ...] [ORDER BY expr [ASC|DESC], ...] [<frame>] )
func(args) OVER <window name>

<frame> := { ROWS | RANGE } BETWEEN <bound> AND <bound>
         | { ROWS | RANGE } <bound>                 -- shorthand for BETWEEN <bound> AND CURRENT ROW
<bound> := UNBOUNDED PRECEDING | <n> PRECEDING | CURRENT ROW | <n> FOLLOWING | UNBOUNDED FOLLOWING
```

`PARTITION BY` splits the rows into independent groups (typically `pv`);
`ORDER BY` orders rows inside each partition (typically `time`). A
`WINDOW w AS (...)` clause names a definition so several calls can share it;
`OVER (w ROWS 2 PRECEDING)` reuses `w` and adds an `ORDER BY` or frame (a
named window's `PARTITION BY` cannot be replaced).

| Function | Result | Frame |
|---|---|---|
| `ROW_NUMBER()` | 1, 2, 3, … within the partition | ignored |
| `RANK()` / `DENSE_RANK()` | rank by `ORDER BY`; ties share a rank (`RANK` leaves gaps) | ignored |
| `LAG(x [, n [, default]])` / `LEAD(x [, n [, default]])` | `x` from `n` rows before / after (default 1); `default` (else `NULL`) outside the partition | ignored |
| `FIRST_VALUE(x)` / `LAST_VALUE(x)` | `x` at the first / last row of the frame | used |
| `SUM`, `AVG`, `MIN`, `MAX`, `COUNT(*\|x)` | aggregate over the frame, nulls skipped | used |

Frames:

- `ROWS` counts physical rows: `ROWS BETWEEN 2 PRECEDING AND CURRENT ROW` is
  the current row and the two before it.
- `RANGE` compares `ORDER BY` values and needs exactly one `ORDER BY` key when
  an offset is given: an integer offset for integer keys, a duration for
  timestamps (`RANGE BETWEEN 5m PRECEDING AND CURRENT ROW` is the last five
  minutes up to each sample). Rows with an equal key (peers) share a frame.
- Default frame: with `ORDER BY`, from the start of the partition to the
  current row and its peers (a running aggregate); without `ORDER BY`, the
  whole partition.
- `NULL` order keys sort last in both directions and are peers of each other.

Rules:

- Window functions are allowed in the select list and `ORDER BY` only (by the
  call itself or its select alias). To filter on one, use a derived table:

  ```sql
  -- Latest sample per PV
  SELECT pv, time, value FROM (
      SELECT pv, time, value, ROW_NUMBER() OVER (PARTITION BY pv ORDER BY time DESC) AS rn
      FROM mldp.time_series WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h) x
  WHERE rn = 1;
  ```

- They run after `WHERE`, `GROUP BY` and `HAVING`, so in a grouped query they
  see one row per group and may use aggregates:
  `SELECT pv, SUM(value), RANK() OVER (ORDER BY SUM(value) DESC) FROM ... GROUP BY pv`.
- Not supported: `COUNT(DISTINCT x) OVER`, `FIRST`/`LAST` with `OVER`, `GROUPS`
  frames, `EXCLUDE`, `NTILE`, `PERCENT_RANK`, `CUME_DIST`, `NTH_VALUE`,
  `IGNORE NULLS`.
- `SUM` returns an integer for integer arguments and a double otherwise (the
  native `value` column always gives a double); `AVG` returns a double.
- Arithmetic on the native `value` column is not supported, so
  `value - LAG(value) OVER w` fails to plan; select `LAG(value)` next to
  `value` instead. Integer and timestamp columns work (`time - LAG(time) OVER w`).
- `window`, `range`, `rows`, `row`, `current`, `partition`, `preceding` and
  `following` stay usable as column names, so `window IN (...)` is unchanged.

Window functions run **locally**: every sample of the selection is fetched,
then sorted by partition and order keys (calls with the same `PARTITION BY` and
`ORDER BY` share one sort). Without an outer `ORDER BY`, rows come back grouped
by partition in window order. When the buffered rows exceed `--memory-mb`, they
are sorted in runs spilled to `--spill-dir` and merged, and each partition is
computed as it is completed; a single partition must still fit in memory. A
`LIMIT` above a window is not pushed to the backend. On `mldp.time_series`, a
window that is exactly `PARTITION BY pv ORDER BY time` reuses the per-PV time
order of the samples (shown as `sorted_input=true`) and falls back to a full
sort if the samples arrive out of order. Frames at the start of the time range
see only the rows inside it (no look-back fetch).
`EXPLAIN` shows the step as `PhysicalWindow(groups=…, calls=…, partition=…, order=…, sorted_input=…)`
and `SHOW FUNCTIONS` lists the window-only functions with `kind = window`.
`.help timeseries` in the REPL summarizes the syntax.

```sql
-- Previous sample and the gap since it, per PV
SELECT pv, time, value, LAG(value) OVER w AS previous, time - LAG(time) OVER w AS gap
FROM mldp.time_series
WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h
WINDOW w AS (PARTITION BY pv ORDER BY time);

-- Five-minute moving average and running sample count
SELECT pv, time, value,
       AVG(value) OVER (PARTITION BY pv ORDER BY time RANGE BETWEEN 5m PRECEDING AND CURRENT ROW) AS avg_5m,
       COUNT(*) OVER (PARTITION BY pv ORDER BY time) AS n
FROM mldp.time_series
WHERE pv = 'RF:AMP' AND time >= NOW - 1h;
```

#### How the pieces fit: a worked example

Take one PV with five samples (`t` in seconds, `v` the value; two samples share `t = 3`):

| t | v |
|---|---|
| 1 | 10 |
| 2 | 20 |
| 3 | 30 |
| 3 | 40 |
| 5 | 50 |

```sql
SELECT t, v,
       ROW_NUMBER() OVER w                                         AS rn,
       RANK()       OVER w                                         AS rk,
       DENSE_RANK() OVER w                                         AS drk,
       LAG(v)       OVER w                                         AS prev,
       LEAD(v)      OVER w                                         AS next,
       SUM(v)       OVER w                                         AS running,
       SUM(v)       OVER (w ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS last_two,
       COUNT(*)     OVER (w RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) AS within_1
FROM samples
WINDOW w AS (ORDER BY t);
```

| t | v | rn | rk | drk | prev | next | running | last_two | within_1 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 10 | 1 | 1 | 1 | NULL | 20 | 10 | 10 | 1 |
| 2 | 20 | 2 | 2 | 2 | 10 | 30 | 30 | 30 | 2 |
| 3 | 30 | 3 | 3 | 3 | 20 | 40 | 100 | 50 | 3 |
| 3 | 40 | 4 | 3 | 3 | 30 | 50 | 100 | 70 | 3 |
| 5 | 50 | 5 | 5 | 4 | 40 | NULL | 150 | 90 | 1 |

What to notice:

- `ROW_NUMBER` is always unique; `RANK` gives ties the same number and skips
  the next one (3, 3, 5); `DENSE_RANK` does not skip (3, 3, 4).
- `LAG`/`LEAD` return `NULL` at the partition edges. Use a third argument for
  a default: `LAG(v, 1, 0)`.
- `running` uses the **default frame**, which includes *ties* of the current
  row: both `t = 3` rows show 100, not 60 then 100. Use
  `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW` for a strict row-by-row total.
- `ROWS` counts rows (`last_two` = this row plus the one before it).
  `RANGE` compares values of the `ORDER BY` key (`within_1` = rows with
  `t` between `t - 1` and `t`).

#### Common recipes

**Latest N samples per PV** (top-N per group, filter through a derived table):

```sql
SELECT pv, time, value FROM (
    SELECT pv, time, value,
           ROW_NUMBER() OVER (PARTITION BY pv ORDER BY time DESC) AS rn
    FROM mldp.time_series
    WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h) x
WHERE rn <= 3;
```

**Sampling gaps**: time elapsed since the previous sample of the same PV:

```sql
SELECT pv, time, time - LAG(time) OVER (PARTITION BY pv ORDER BY time) AS gap
FROM mldp.time_series
WHERE pv = 'RF:AMP' AND time >= NOW - 10m;
```

**Previous and next value side by side** (to compare a sample with its neighbours):

```sql
SELECT pv, time,
       LAG(value)  OVER w AS before,
       value,
       LEAD(value) OVER w AS after
FROM mldp.time_series
WHERE pv IN ('BPM:X', 'BPM:Y') AND time >= NOW - 5m
WINDOW w AS (PARTITION BY pv ORDER BY time);
```

**Moving statistics**: by row count and by time span:

```sql
SELECT pv, time, value,
       AVG(value) OVER (w ROWS BETWEEN 9 PRECEDING AND CURRENT ROW)   AS avg_last_10,
       MIN(value) OVER (w RANGE BETWEEN 1m PRECEDING AND CURRENT ROW) AS min_1m,
       MAX(value) OVER (w RANGE BETWEEN 1m PRECEDING AND CURRENT ROW) AS max_1m
FROM mldp.time_series
WHERE pv = 'RF:AMP' AND time >= NOW - 1h
WINDOW w AS (PARTITION BY pv ORDER BY time);
```

**Centered window**: frames can look ahead too:

```sql
SELECT pv, time, value,
       AVG(value) OVER (PARTITION BY pv ORDER BY time
                        RANGE BETWEEN 30s PRECEDING AND 30s FOLLOWING) AS avg_centered_1m
FROM mldp.time_series
WHERE pv = 'RF:AMP' AND time >= NOW - 1h;
```

**First and last value of each PV on every row** (whole-partition frame):

```sql
SELECT pv, time, value,
       FIRST_VALUE(value) OVER w AS first_in_range,
       LAST_VALUE(value)  OVER w AS last_in_range,
       COUNT(*)           OVER w AS samples_in_range
FROM mldp.time_series
WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h
WINDOW w AS (PARTITION BY pv ORDER BY time
             ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING);
```

`LAST_VALUE` with only `ORDER BY` (default frame) returns the *current* row's
value (the frame ends at the current row), so give it an explicit frame as above.

**Rank PVs after grouping** (window over `GROUP BY` output):

```sql
SELECT pv, COUNT(*) AS n,
       RANK() OVER (ORDER BY COUNT(*) DESC) AS busiest,
       SUM(COUNT(*)) OVER ()                AS total_samples
FROM mldp.time_series
WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h
GROUP BY pv
ORDER BY busiest;
```

**Number and order configuration activations**:

```sql
SELECT config_name, start_time,
       ROW_NUMBER() OVER (PARTITION BY config_name ORDER BY start_time) AS nth_activation,
       start_time - LAG(start_time) OVER (PARTITION BY config_name ORDER BY start_time) AS since_previous
FROM mldp.configuration_activation
ORDER BY config_name, start_time;
```

#### Common mistakes

| Query | Problem | Use instead |
|---|---|---|
| `... WHERE ROW_NUMBER() OVER (...) = 1` | Window functions are not allowed in `WHERE` | Wrap in a derived table and filter `rn` outside |
| `value - LAG(value) OVER w` | No arithmetic on the native `value` column | Select `value` and `LAG(value) OVER w` side by side |
| `LAG(value) OVER (ORDER BY time)` on many PVs | Rows of different PVs mix | Add `PARTITION BY pv` |
| `RANGE BETWEEN 5m PRECEDING ...` with `ORDER BY pv, time` | `RANGE` offsets need exactly one `ORDER BY` key | Partition by `pv`, order by `time` only |
| `RANGE BETWEEN 5 PRECEDING ...` on `time` | Timestamp keys need a duration | `5s`, `5m`, `1h` |
| `OVER (w PARTITION BY ...)` | A named window's partition cannot be replaced | Define a second `WINDOW` entry |

### Compact and expanded table output

Table output keeps each result on one physical line. Lists and maps show their first two values followed by `+N` when values remain; map keys are sorted for a predictable display. Use the REPL controls below to inspect every value in a record:

When enabled with `--table-fit` or `.table-fit on`, table output fits to the current interactive terminal width. The table always spans the full terminal width, and a streamed result keeps the column widths chosen from its first rows so every batch lines up under one header. Short columns keep their natural width; the remaining width goes mostly to the wider columns, and their headers and cells wrap onto extra lines (at spaces or punctuation when possible) so the full content stays visible. A cell longer than 1024 characters is cut and ends with `...`. When even 4 characters per column cannot fit, each row is printed as stacked `column: value` lines instead. This display-only setting never wraps or truncates JSON, CSV, Arrow, expanded output, or redirected/piped output.

```text
\expanded on     # persistently enable expanded records
\expanded off    # return to compact table output
\x               # toggle expanded records
SELECT * FROM mldp.pv_metadata \G
```

The `\G` query terminator expands that one result without changing the current display mode. JSON, CSV, and Arrow output retain their complete machine-readable collections.

### String matching: `PREFIX`, `CONTAINS`, `LIKE`

Three operators match part of a string:

| Operator | Case | Wildcards | Matches when | Example |
|---|---|---|---|---|
| `PREFIX 'x'` | sensitive | none | value starts with `x` | `pv PREFIX 'LTU:'` |
| `CONTAINS 'x'` | sensitive | none | value contains `x` anywhere | `pv CONTAINS ':BPM'` |
| `LIKE 'p'` | **insensitive** | `%`/`*`, `_` | whole value matches pattern `p` | `pv LIKE 'ltu:%:bpm_'` |

Choose `PREFIX` or `CONTAINS` for plain literal text (characters such as `%`
and `_` are taken literally), and `LIKE` when you need wildcards, anchoring at
both ends, or case-insensitive matching. `LIKE` must match the *whole* value:
`pv LIKE 'BPM'` matches only `BPM`; use `'%BPM%'` for "contains".

Where each operator runs:

- **`pv` on `mldp.time_series`, `mldp.time_series_table`, `mldp.pv_stats`** —
  pushed to MLDP as a PV-name regex (`PvSelector.pvNamePattern`), so no
  `pv =`/`pv IN` list is needed. Results are re-verified locally with the SQL
  semantics above. Only the first pattern is pushed; others are applied locally.
  With `pv_tag`/`pv_attributes.<key>`, `PREFIX`/`CONTAINS` join the backend
  metadata query instead, while `LIKE` is applied locally only.
- **`pv`, `name`, `attributes.<key>`, … on annotation tables
  (`mldp.pv_metadata`, `mldp.configuration`, …)** — see each table: `PREFIX`/
  `CONTAINS` may be backend-pushed, `LIKE` is evaluated locally.
- **Other string columns** — evaluated locally after fetching, so combine with a
  pushable predicate to limit data transferred.

```sql
-- Every BPM in the LTU, any case, without listing PVs
SELECT pv, start_time, end_time
FROM mldp.pv_stats
WHERE pv LIKE 'ltu:%:bpm%';

-- Last hour of all PVs starting with 'RF:'
SELECT pv, time, value
FROM mldp.time_series
WHERE pv PREFIX 'RF:' AND time >= NOW - 1h;

-- Literal underscore: PREFIX needs no escaping, LIKE needs \_
SELECT pv FROM mldp.pv_stats WHERE pv PREFIX 'PV_';
SELECT pv FROM mldp.pv_stats WHERE pv LIKE 'pv\_%';
```

#### `LIKE` pattern syntax

`LIKE` supports standard SQL patterns plus `*` as a convenient alternative to `%`:

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
WHERE time >= NOW -2H AND time <= NOW
WHERE time >= NOW -1d AND time <= NOW
```

Duration literals use the form `<non-negative-integer><unit>` with no space:

| Unit | Meaning | Examples |
|---|---|---|
| `s` or `S` | seconds | `5s`, `3600S` |
| `m` or `M` | minutes | `30m`, `90M` |
| `h` or `H` | hours | `1h`, `36H` |
| `d` or `D` | fixed 24-hour days | `1d`, `2D` |

Each day is exactly 86,400 seconds, rather than a timezone-aware calendar day.
The parser does not accept `w` for weeks, `ms` for milliseconds, or compound
durations such as `1h30m`. Express those intervals with a single supported
unit instead: `1d` for one day, `7d` for one week, and `90m` for one hour and
thirty minutes. For sub-second precision, use the explicit
`duration_ns(<signed-integer-nanoseconds>)` constructor where a duration value
is accepted.

### Native value predicates

The `value` column of materialized time-series results can be a native Arrow
dense union. Local filtering supports `=`, `!=`, `<`, `<=`, `>`, `>=`, `IN`,
and `BETWEEN` for comparable active members. These predicates are evaluated
after data is materialized; they are not sent as MLDP backend RPC predicates.

Boolean literals are case-insensitive: `TRUE` and `FALSE`. Use explicit
nanosecond constructors for native timestamp and duration members:

```sql
-- Boolean values
SELECT * FROM samples WHERE value = TRUE;
SELECT * FROM samples WHERE value IN (false, TRUE);

-- Native timestamps and durations; arguments are signed integer nanoseconds
SELECT * FROM samples WHERE value >= timestamp_ns(1720000000000000000);
SELECT * FROM samples WHERE value BETWEEN duration_ns(-500000000) AND duration_ns(500000000);
```

Numeric members (`int*`, `uint*`, `float`, and `double`) compare with numeric
literals using numeric promotion. Strings compare with string literals, and
booleans compare with boolean literals. Timestamps and durations compare only
with their matching constructor type; `timestamp_ns(...)` and
`duration_ns(...)` are not interchangeable with each other or with plain
integer literals.

An incompatible comparison never matches, including `!=`. An `IN` list matches
only when at least one compatible literal is equal. `BETWEEN` requires two
numeric bounds, two timestamp bounds, or two duration bounds of the matching
family. Binary, null, arrays, structures, images, and other unsupported native
members remain non-comparable; use `IS NOT NULL` when only nullness matters.

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
| Tags | Select `tags`; filter with `tag =` or `tag IN` | `mldp.time_series`, `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation` | Annotation-table criteria are sent to the annotation service and locally verified. That local verification suppresses `LIMIT` pushdown. Time-series tags come from returned bucket `dataColumn.metadata` and are filtered locally. |
| Attributes | Select `attributes`; select/filter `attributes.<key>` with `=` or `IN` | `mldp.time_series`, `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation` | Same execution path as tags. Selecting the whole `attributes` map forces annotation tables to read every page before emitting a batch. |
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

-- Next page — use the token printed after the result statistics
SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'MY:PV' AND time >= NOW -24h AND time <= NOW
LIMIT 500 PAGE TOKEN '<token-from-previous-result>'
```

The continuation token is printed after the result statistics for each paginated result.

---

## Virtual table catalog

### `mldp.time_series`

Time-series samples from the MLDP query service.

A valid selection that matches no samples returns an empty result. This also
applies when a requested PV is not returned for the selected time range.

| Column | Type | Required predicate | Pushable operators | Notes |
|---|---|---|---|---|
| `pv` | string | no | `=`, `IN`, `PREFIX`, `CONTAINS`, `LIKE` | PV name. `=`/`IN` select explicit PVs; `PREFIX`/`CONTAINS`/`LIKE` are sent as a backend PV-name regex and verified locally. Omitted = every PV. |
| `time` | timestamp | no | `>=`, `<=` | UTC epoch seconds. |
| `window` | timestamp | no | `IN (start, end)`, `IN (SELECT start, end ...)` | Closed interval input; each normalized range becomes a time-series request. |
| `value` | union | no | — | Typed sample value (see below). |
| `column_type` | string | no | — | Native MLDP value kind: `string`, `bool`, integer, float, `double`, `binary`, `timestamp`, `array`, `structure`, or `image`. Filter locally with `=` or `IN`. |
| `tags` | list&lt;string&gt; | no | — | Complete bucket column-metadata tag collection. Filter with `tag =` or `tag IN` locally. |
| `attributes` | map&lt;string,string&gt; | no | — | Complete bucket column-metadata attributes. Select/filter `attributes.&lt;key&gt;` locally. |
| `provenance` | map&lt;string,string&gt; | no | — | Complete bucket column-metadata provenance. Select/filter `provenance.&lt;key&gt;` locally. |
| `pv_tag` | string | no | `=`, `IN` | Predicate-only. Selects PVs by PV-metadata tag (backend `PvSelector.metadataQuery`). |
| `pv_attributes.<key>` | string | no | `=`, `IN` | Predicate-only. Selects PVs by PV-metadata attribute. |
| `config_name` | string | no | `=`, `IN` | Predicate-only. Restricts samples to activation intervals of the named configuration(s) (backend `configurationSelector`). |
| `config_activation_id` | string | no | `=`, `IN` | Predicate-only. Restricts samples to the given client activation id(s). |
| `config_category` | string | no | `=`, `IN` | Predicate-only. Restricts samples to activations of configurations in the category(ies). |
| `config_tag` | string | no | `=`, `IN` | Predicate-only. Restricts samples to activations of configurations carrying the tag(s). |
| `timeout` | duration | no | `=` | Query timeout in seconds. |
| `rpc_deadline` | duration | no | `=` | RPC deadline in seconds. |

`value` is a dense-union Arrow column that carries the native MLDP data type: `string`, `bool`, `uint32`, `uint64`, `int32`, `int64`, `float`, `double`, `binary`, `timestamp`, `array`, `structure`, or `image`.

Table and expanded output display the active union member directly (for example, a double sample renders as `10`, not Arrow's `union{double: ...}` diagnostic). JSON, CSV, and Arrow output retain the underlying union representation for machine-readable consumers.

No predicate is required. Without `pv =`/`pv IN`, PVs are chosen by the
backend: by `pv_tag`/`pv_attributes.<key>` when present, else by the PV-name
pattern, else every PV (`.*`). Different `config_*` columns are ANDed; values
within one are ORed. Series sharding applies only to explicit PV lists.

`config_*` and `status_*` predicates are exact per sample, so a query that uses
them runs on MLDP's sample-oriented `querySamples` instead of `queryBuckets`
(bucket queries would return every bucket overlapping a matching activation
whole). Results keep the normal long-form columns.

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

-- Backend-selected PVs restricted to a configuration's activation intervals
SELECT pv, time, value
FROM mldp.time_series
WHERE pv_tag = 'magnet'
  AND pv PREFIX 'LTU:'
  AND config_name = 'BSY SAT Shift 1'
```

Use `window` when the requested range is a literal interval or is produced by
another query. The interval is inclusive; literal endpoints may be reversed
and are normalized. A window subquery must return exactly two non-null
timestamp columns: start first and end second. Its output names and aliases do
not matter. Overlapping or adjacent subquery intervals are coalesced, and the
long-form table opens serial MLDP server cursors for each resulting range.

### Window shard parameters

Both MLDP time-series tables accept optional shard parameters after a semicolon
inside a `window IN (...)` input:

```sql
window IN (start, end; slice <duration>, series_per_shard <positive-integer>)
```

The parameters are optional, may appear in either order, and each may appear
at most once.

| Parameter | Meaning | Default |
|---|---|---|
| `slice` | Maximum timestamp span for one backend cursor. It is a positive duration. | `1s` |
| `series_per_shard` | Maximum number of requested PVs in one backend cursor. It is a positive integer. | `1` |

The driver first coalesces normalized window ranges, then visits shards in this
deterministic order: normalized range, time slice, and requested-PV group.
Within one time slice, the streaming long-form `mldp.time_series` path can use
concurrent independent requested-PV groups up to the configured query pool
`max-conn`; result batches still emit in requested-PV-group order. The current
wide-table materialization path reports one active cursor (`shards 1/1`) before
its pivot, then returns to `0/1` after each shard completes. Each cursor receives exactly one bounded time range
and PV group. Backend time
bounds are inclusive, so the driver locally makes every non-final slice
half-open; a sample at a slice boundary appears once. `slice` and `series_per_shard`
are supported only on `window` input for `mldp.time_series` and
`mldp.time_series_table`. Duplicate names, unknown names, zero or negative
values, and shard parameters on another table or predicate are rejected.

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE')
  AND window IN (NOW - 10m, NOW; slice 5s, series_per_shard 2)
```

The same options can follow a window subquery:

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE')
  AND window IN (
    SELECT activation.start_time, activation.end_time
    FROM mldp.configuration_activation activation
    WHERE activation.end_time IS NOT NULL;
    series_per_shard 2, slice 5s
  )
```

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv = 'MY:PV:CURRENT'
  AND window IN (NOW - 10m, NOW)
```

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN (
  SELECT pv
  FROM mldp.pv_metadata
  WHERE tag = 'magnet'
)
AND window IN (
  SELECT activation.start_time, activation.end_time
  FROM mldp.configuration_activation activation
  WHERE activation.end_time IS NOT NULL
)
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
`pv =` or `pv IN (...)` determines the requested PV columns; the PV-name
pattern, `pv_tag`, `pv_attributes.<key>`, and `config_*` predicates of
`mldp.time_series` work here too. The result contains one shared `time` column
followed by returned PV columns in the requested-PV order (sorted by name when
the backend selects the PVs).

Sample-status filtering (`status_*`, predicate-only, also accepted by
`mldp.time_series`) uses the sample-oriented MLDP query. With `status_*` or
`config_*` predicates this table runs that query natively instead of pivoting
bucket cursors:

| Column | Pushable operators | Notes |
|---|---|---|
| `status_domain` | `=` | Required to enable status filtering. |
| `status_layer` | `=`, `IN` | Omitted = every layer in the domain. |
| `status_code` | `=`, `IN` | Omitted = any code. |
| `status_mode` | `=` | `'include'` (default) keeps only samples with a matching status; `'exclude'` drops them. |

Filtered-out samples become nulls; timestamps where every PV is filtered out
are omitted. Each PV column keeps its native Arrow type; shorter
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
window input on either MLDP time-series table accepts either one literal inclusive interval, `window IN (start,
end)`, or a subquery that returns exactly two non-null timestamp outputs. The
subquery outputs are positional: the first is the start and the second is the
end, regardless of their names or aliases. Literal endpoints must be
timestamp-compatible expressions; `NOW` and `NOW +/- duration` are supported,
and reversed endpoints are automatically normalized. A query cannot supply
both forms. Subquery ranges must be closed; overlapping or adjacent ranges are
coalesced before the driver issues the corresponding time-series requests. As with ordinary SQL
filtering, a valid `pv` or `window` subquery that finds no rows returns an empty
result; malformed subquery output remains an error.

The window shard parameters described above apply to both MLDP time-series
tables. Long-form `mldp.time_series` emits server-cursor batches directly.
`mldp.time_series_table` consumes the same bounded long-form cursors into
temporary Arrow spill storage and emits a globally time-ordered pivot only
after preparation finishes. Missing `(time, pv)` cells are null; duplicate
cells are an execution error.

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
  AND window IN (NOW - 10h, NOW; slice 30s, series_per_shard 2)
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
  SELECT activation.start_time, activation.start_time + 2s
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
| `pv` | string | no | `=`, `IN`, `PREFIX`, `CONTAINS`, `LIKE` | PV name. Patterns are sent as a backend regex; omitted = every PV. |
| `start_time` | timestamp | no | — | Earliest recorded sample. |
| `end_time` | timestamp | no | — | Most recent recorded sample. |
| `num_buckets` | int | no | — | Number of storage buckets. |

```sql
SELECT pv, start_time, end_time, num_buckets
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

`LIKE` is available on every string column and runs as a local filter; see [String matching](#string-matching-prefix-contains-like) for its wildcard and escaping rules.

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
| `start_time` | timestamp | `=`, `!=`, `<`, `<=`, `>`, `>=` | Activation start time. The annotation-service candidate set is locally verified. |
| `end_time` | timestamp | `=`, `!=`, `<`, `<=`, `>`, `>=`, `IS NULL`, `IS NOT NULL` (local) | Activation end time; null means the activation is open. |
| `config_name` | string | `=`, `IN`, `LIKE` (local) | Configuration name. |
| `activation_id` | string | `=`, `IN`, `LIKE` (local) | Client-assigned activation identifier. |
| `description` | string | `LIKE` (local) | Free-text description. |
| `tags` | list&lt;string&gt; | — | Complete tag collection. Filter with `tag =` or `tag IN`; criteria are backend-pushed and locally verified. |
| `attributes` | map&lt;string,string&gt; | — | Complete dynamic attribute collection. Select/filter `attributes.&lt;key&gt;`; criteria are backend-pushed and locally verified. |
| `tag` | string | `=`, `IN` | Predicate-only tag membership shorthand. |
| `created_time` | timestamp | — | Record creation time. |
| `updated_time` | timestamp | — | Last modification time. |

No predicate is required; an unfiltered query returns every activation. Timestamp predicates are evaluated locally after fetching the annotation-service candidate set.

```sql
-- Activations for a specific configuration
SELECT start_time, config_name, activation_id, description
FROM mldp.configuration_activation
WHERE config_name = 'injector_tuning'

-- Activations in a time window
SELECT start_time, config_name, activation_id
FROM mldp.configuration_activation
WHERE start_time >= NOW -2h AND end_time <= NOW
```

---

### `mldp.active_configurations`

Configurations that were active at a given point in time. **Requires exactly one `at = <epoch>` predicate.**

| Column | Type | Required predicate | Notes |
|---|---|---|---|
| `at` | timestamp | **yes** (`=` only) | Point in time to query. Must be constrained. |
| `name` | string | — | Active configuration name. |
| `activation_id` | string | — | Activation identifier. |
| `start_time` | timestamp | — | Activation start time. |

```sql
SELECT name, activation_id, start_time
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
table_name                         type                  location
---------------------------------------------------------------------------------------------------------------
mldp.active_configurations         virtual
mldp.configuration                 virtual
mldp.configuration_activation      virtual
mldp.pv_metadata                   virtual
mldp.pv_stats                      virtual
mldp.time_series                   virtual
mldp.time_series_table             virtual
magnet_samples                     persistent Arrow IPC  /var/lib/mldp/query-catalog/.mldp-query-tables/6d61676e65745f73616d706c6573.arrow
-- 8 rows (8 from backend, 0 filtered) in 0ms | 0 RPC | 0 bytes spilled | 0 bytes materialized in 0 file(s) | 0 MB peak
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

To run the complete tutorial non-interactively against the live services, use
the dedicated runner from the project root. It creates a unique namespace,
feeds the SQL below to real REPL client processes using the inline queryable
configuration, exercises the persistent-table walkthrough, and removes its
generated annotation records afterward:

```bash
python3 scripts/run_query_cli_tutorial.py
```

Pass `--verify` to make the data generator read its annotation records back
before the REPL starts. This check is optional; without it the runner still
creates all tutorial time-series, metadata, configurations, and activations.

### Step 1 — Populate sample data

Run the sample-data generator from the project root:

```bash
python3 scripts/generate_mldp_sample_data.py
```

This creates:
- **20 PVs** cycling through four device families: `mldp_sample:MAGNET:01:VALUE`, `mldp_sample:RF:02:VALUE`, `mldp_sample:VACUUM:03:VALUE`, `mldp_sample:DIAGNOSTIC:04:VALUE`, `mldp_sample:MAGNET:05:VALUE`, …, `mldp_sample:DIAGNOSTIC:20:VALUE`
- **3600 time-series samples** per PV (deterministic sine waves) at 1-second intervals ending roughly at "now", written as **10 contiguous buckets of 360 samples** each (**200 archive buckets** across all 20 PVs)
- PV metadata with tags (`sample`, `mldp_sample`, device family) and attributes (`namespace`, `device_group`, `ordinal`, `units`, `sample_period_seconds`)
- **4 configurations:** two `beam_mode` (`mldp_sample_injector_tuning`, `mldp_sample_user_delivery`), one `rf` (`mldp_sample_rf_station_a`), one `vacuum` (`mldp_sample_vacuum_ready`)
- **4 activation windows:** beam-mode activations are closed (the two most recent hours, adjacent); RF and vacuum activations are open (started within the last 30 and 15 minutes)

The script prints the exact PV names, time range, and an example query when it finishes:

```
Generated MLDP sample namespace: mldp_sample
Provider: mldp_sample-sample-provider (<id>)
Time series: 20 PVs x 3600 samples at 1-second intervals (10 buckets per PV x 360 samples)
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

Start one REPL session for the remaining interactive examples:

```bash
mldp_pvxs_driver -c query-config.yaml query
```

Paste each semicolon-terminated statement into the REPL:

```sql
SHOW TABLES;
DESC mldp.time_series;
DESCRIBE mldp.pv_metadata;
```

### Step 4 — Fetch time-series samples

```sql
-- Last 10 minutes of samples for one MAGNET PV
SELECT pv, time, value
FROM mldp.time_series
WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
  AND time >= NOW -10m
  AND time <= NOW;

-- Multiple PVs from different device families
SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
             'mldp_sample:RF:02:VALUE',
             'mldp_sample:VACUUM:03:VALUE',
             'mldp_sample:DIAGNOSTIC:04:VALUE')
  AND time >= NOW -1h
  AND time <= NOW;
```

The REPL writes results directly to its terminal. To export CSV or Arrow
output, use one-shot mode with `--format` and shell redirection.

### Step 5 — Check PV availability

```sql
-- A fresh generator run creates 10 buckets for each requested PV.
SELECT pv, start_time, end_time, num_buckets
FROM mldp.pv_stats
WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
             'mldp_sample:RF:02:VALUE',
             'mldp_sample:VACUUM:03:VALUE',
             'mldp_sample:DIAGNOSTIC:04:VALUE');
```

### Step 6 — Explore metadata

```sql
# All sample-namespace PVs
SELECT pv, description, attributes.device_group, attributes.units, modified_by
FROM mldp.pv_metadata
WHERE pv PREFIX 'mldp_sample';

# PVs tagged 'magnet' (MAGNET family PVs: 01, 05, 09, 13, 17)
SELECT pv, alias, description, attributes.device_group, attributes.ordinal, tags
FROM mldp.pv_metadata
WHERE tag = 'magnet';

# PVs filtered by namespace attribute
SELECT pv, attributes.device_group, attributes.ordinal
FROM mldp.pv_metadata
WHERE attributes.namespace = 'mldp_sample'
ORDER BY attributes.ordinal;
```

### Step 7 — Query configurations

```sql
# All sample configurations
SELECT name, category, description
FROM mldp.configuration
WHERE name PREFIX 'mldp_sample';

# Only beam-mode configurations
SELECT name, category, description
FROM mldp.configuration
WHERE category = 'beam_mode';

# Closed activation windows for beam-mode configurations
SELECT start_time, end_time, config_name, activation_id
FROM mldp.configuration_activation
WHERE config_name IN ('mldp_sample_injector_tuning', 'mldp_sample_user_delivery')
  AND end_time IS NOT NULL;

# Active configurations 30 minutes ago (RF and vacuum should be active)
SELECT name, activation_id, start_time
FROM mldp.active_configurations
WHERE at = NOW -30m;
```

### Step 8 — Join time-series with metadata

This query obtains the PV list from `mldp.pv_metadata` before querying samples.

```sql
SELECT ts.pv, ts.time, ts.value, m.description, m.attributes.units
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv IN (
  SELECT pv
  FROM mldp.pv_metadata
  WHERE pv PREFIX 'mldp_sample:MAGNET'
)
  AND ts.time >= NOW -10m
  AND ts.time <= NOW;
```

### Step 9 — Use EXPLAIN to inspect the query plan

```sql
EXPLAIN SELECT pv, time, value FROM mldp.time_series
WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
  AND time >= NOW -1h AND time <= NOW;
```

### Step 10 — Enter a longer multi-line query

The REPL keeps buffering until the terminating semicolon, so longer queries do
not require a temporary SQL file:

```sql
SELECT ts.pv,
       ts.time,
       ts.value,
       m.description
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv IN ('mldp_sample:MAGNET:01:VALUE',
                'mldp_sample:RF:02:VALUE',
                'mldp_sample:VACUUM:03:VALUE',
                'mldp_sample:DIAGNOSTIC:04:VALUE')
  AND ts.time >= NOW -1h
  AND ts.time <= NOW
  AND m.pv PREFIX 'mldp_sample'
LIMIT 200;
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
  SELECT activation.start_time, activation.end_time
  FROM mldp.configuration_activation activation
  JOIN mldp.configuration configuration
    ON activation.config_name = configuration.name
  WHERE activation.attributes.namespace = 'mldp_sample'
    AND configuration.attributes.namespace = 'mldp_sample'
    AND configuration.category = 'beam_mode'
    AND activation.end_time IS NOT NULL
)
```

The PV subquery output must be a single non-null string field named `pv`. The
window subquery must return exactly two non-null timestamp fields: its first
output is the interval start and its second output is the interval end. Field
names and aliases are ignored, so expressions such as `activation.start_time + 2s`
are valid endpoints. Open or inverted activation ranges are rejected. Each
returned batch is `time` followed by the requested native PV columns in
metadata-query order.

### Step 12 — Materialize a temporary table in this session

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

### Persistent table across client sessions

Use `CREATE TABLE` without `TEMP` for an immutable snapshot that survives the
REPL session. Every client must point at the same stable, writable catalog
directory. The following walkthrough creates the table in one REPL, reads it
in a second REPL, and removes it in a third.

**Client session 1 — create the snapshot:**

```bash
mldp_pvxs_driver -c query-config.yaml query \
  --table-catalog-dir /var/lib/mldp/query-catalog
```

```sql
CREATE TABLE magnet_samples AS
SELECT pv, time, value
FROM mldp.time_series
WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
  AND time >= NOW -10m
  AND time <= NOW;
```

**Client session 2 — discover and read the same snapshot:**

```bash
mldp_pvxs_driver -c query-config.yaml query \
  --table-catalog-dir /var/lib/mldp/query-catalog
```

```sql
SHOW TABLES;
DESC magnet_samples;
SELECT * FROM magnet_samples;
```

**Client session 3 — explicitly remove the snapshot:**

```bash
mldp_pvxs_driver -c query-config.yaml query \
  --table-catalog-dir /var/lib/mldp/query-catalog
```

```sql
DROP TABLE magnet_samples;
SHOW TABLES;
```

Set the catalog location with `query --table-catalog-dir <path>`. If it is not
specified, the CLI uses `<tmp>/mldp-query-catalog`; choose a stable directory
such as `/var/lib/mldp/query-catalog` when tables must survive multiple client
runs. The catalog stores managed files only in `<path>/.mldp-query-tables/` and
does not clean unrelated files. `CREATE TABLE` fails if the name already
exists, so use `DROP TABLE magnet_samples` before recreating it. Use a shared
mounted catalog directory when multiple processes or hosts need access to the
same snapshots.
