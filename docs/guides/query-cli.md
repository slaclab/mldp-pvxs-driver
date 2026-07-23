# Query CLI Guide

The `query` subcommand runs one SQL statement — parse → plan → execute → render — and prints results to stdout.

> **Related:** [Query Engine Architecture](../reference/query-engine-architecture.md) | [Configuration Reference](configuration.md#queryable-block) | [Tutorial: first queries with sample data](#tutorial-first-queries-with-sample-data)

---

## Command

```bash
mldp_pvxs_driver [global options] query [query options] "<SQL>"
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
| `--no-stats` | off | Suppress the query-stats footer. |
| `--memory-mb <n>` | `256` | Memory budget for the execution context (MiB). |
| `--spill-dir <path>` | `<tmp>/mldp-query-spill` | Directory for spill files under memory pressure. |
| `--spill-partitions <n>` | `16` | Spill partition count for join spill paths. |
| `--join-batch-size <n>` | `100` | Batch size hint for join execution and pagination. |

---

## Quick-start examples

```bash
# Schema introspection — no backend connection needed
mldp_pvxs_driver query "SHOW TABLES"
mldp_pvxs_driver query "DESCRIBE mldp.time_series"

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

### Config file

Service hostnames match the Docker Compose service names defined in `docker-compose.yml` (`dp-ingestion`, `dp-query`, `dp-annotation`):

```yaml
# query-config.yaml
queryable:
  mldp:
    mldp-pool:
      query-url: grpc://dp-query:50052
      min-conn: 1
      max-conn: 2
  mldp-pv-metadata:
    mldp-pv-metadata-pool:
      annotation-url: grpc://dp-annotation:50053
      min-conn: 1
      max-conn: 2
```

### Inline dotted assignments (no config file)

Pass URLs directly with `-c` dotted assignments instead of writing a file:

```bash
mldp_pvxs_driver \
  -c queryable.mldp.mldp-pool.query-url=grpc://dp-query:50052 \
  -c queryable.mldp.mldp-pool.min-conn=1 \
  -c queryable.mldp.mldp-pool.max-conn=2 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=grpc://dp-annotation:50053 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1 \
  -c queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=2 \
  query "SHOW TABLES"
```

Override just the query URL when running against a different host:

```bash
mldp_pvxs_driver \
  -c query-config.yaml \
  -c queryable.mldp.mldp-pool.query-url=grpc://my-host:50052 \
  query "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'MY:PV' LIMIT 10"
```

Two queryable types are available:

| `type` key | Tables exposed | Backend |
|---|---|---|
| `mldp` | `mldp.time_series`, `mldp.pv_stats` | MLDP query gRPC service |
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

### SELECT grammar

```
SELECT { * | column [, column ...] }
FROM   <table> [AS <alias>]
       [JOIN <table> [AS <alias>] ON <col> = <col>] ...
[WHERE <predicate> [AND <predicate>] ...]
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
| SQL LIKE | `pv LIKE 'MY:MAGNET'` (treated as CONTAINS) |

Multiple predicates are combined with `AND`.

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

### Attribute columns

Dynamic key-value metadata stored in MLDP can be accessed with the `attr.<key>` notation:

```sql
SELECT pv, attr.units, attr.device_group
FROM mldp.pv_metadata
WHERE pv PREFIX 'MY:MAGNET'
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

| Column | Type | Required predicate | Pushable operators | Notes |
|---|---|---|---|---|
| `pv` | string | **yes** | `=`, `IN` | PV name. Must be constrained. |
| `time` | timestamp | no | `>=`, `<=` | UTC epoch seconds. |
| `value` | union | no | — | Typed sample value (see below). |
| `timeout` | duration | no | `=` | Query timeout in seconds. |
| `rpc_deadline` | duration | no | `=` | RPC deadline in seconds. |

`value` is a dense-union Arrow column that carries the native MLDP data type: `string`, `bool`, `uint32`, `uint64`, `int32`, `int64`, `float`, `double`, `binary`, `timestamp`, `array`, `structure`, or `image`.

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

---

### `mldp.pv_metadata`

PV metadata and annotation records from the MLDP annotation service.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `pv` | string | `=`, `IN`, `PREFIX`, `CONTAINS` | PV name or alias. |
| `alias` | string | `=`, `IN`, `PREFIX`, `CONTAINS` | Alternate name. |
| `tag` | string | `=`, `IN` | Metadata tag. |
| `description` | string | — | Free-text description. |
| `created_time` | timestamp | — | Record creation time. |
| `updated_time` | timestamp | — | Last modification time. |
| `modified_by` | string | — | Last modifier identity. |

At least one pushable predicate is required (`pv`, `alias`, or `tag`).

Dynamic attributes are accessible as `attr.<key>` and support `=`, `!=`, `IN`, `PREFIX`, `CONTAINS`.

```sql
-- Find PVs by prefix
SELECT pv, description, attr.units
FROM mldp.pv_metadata
WHERE pv PREFIX 'MY:MAGNET'

-- Find by tag
SELECT pv, alias, description
FROM mldp.pv_metadata
WHERE tag = 'production'
```

---

### `mldp.configuration`

Machine/beam configuration records.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `name` | string | `=`, `IN`, `PREFIX`, `CONTAINS` | Configuration name. |
| `category` | string | `=`, `IN` | Configuration category. |
| `parent` | string | `=`, `IN` | Parent configuration name. |
| `description` | string | — | Free-text description. |
| `created_time` | timestamp | — | Creation time. |
| `updated_time` | timestamp | — | Last modification time. |
| `modified_by` | string | — | Last modifier identity. |

At least one pushable predicate is required (`name`, `category`, or `parent`).

```sql
SELECT name, category, description
FROM mldp.configuration
WHERE category = 'beam_mode'
```

---

### `mldp.configuration_activation`

Time-windowed activation records for configurations.

| Column | Type | Pushable operators | Notes |
|---|---|---|---|
| `time` | timestamp | `=`, `>=`, `<=` | Activation window start time. |
| `config_name` | string | `=`, `IN` | Configuration name. |
| `activation_id` | string | `=`, `IN` | Client-assigned activation identifier. |
| `description` | string | — | Free-text description. |
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

---

## Output formats

| `--format` | Description |
|---|---|
| `table` | ASCII-bordered table (default). Best for interactive use. |
| `json` | One JSON object per row, newline-delimited. |
| `csv` | RFC 4180 CSV with header row. |
| `arrow` | Apache Arrow IPC stream (binary). Suitable for programmatic consumption. |

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
- **20 PVs** named `mldp_sample:MAGNET:01:VALUE` through `mldp_sample:VACUUM:20:VALUE`
- **3600 time-series samples** per PV at 1-second intervals ending roughly at "now"
- PV metadata and four beam-mode/RF/vacuum configurations with activation windows

The script prints the exact PV names, time range, and an example query when it finishes:

```
Generated MLDP sample namespace: mldp_sample
Provider: mldp_sample-sample-provider (<id>)
Time series: 20 PVs x 3600 samples at 1-second intervals
Time range: [1720000000, 1720003599] UTC epoch seconds
...
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
      query-url: grpc://dp-query:50052
      min-conn: 1
      max-conn: 2
  mldp-pv-metadata:
    mldp-pv-metadata-pool:
      annotation-url: grpc://dp-annotation:50053
      min-conn: 1
      max-conn: 2
```

### Step 3 — Explore the schema

```bash
# List all available virtual tables
mldp_pvxs_driver query "SHOW TABLES"

# Inspect time-series table columns
mldp_pvxs_driver query "DESCRIBE mldp.time_series"

# Inspect metadata table columns
mldp_pvxs_driver query "DESCRIBE mldp.pv_metadata"
```

### Step 4 — Fetch time-series samples

```bash
# Last 10 minutes of samples for one PV
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, time, value
   FROM mldp.time_series
   WHERE pv = 'mldp_sample:MAGNET:01:VALUE'
     AND time >= NOW -10m
     AND time <= NOW"
```

```bash
# Multiple PVs, CSV output
mldp_pvxs_driver -c query-config.yaml query --format csv \
  "SELECT pv, time, value
   FROM mldp.time_series
   WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
                'mldp_sample:RF:02:VALUE',
                'mldp_sample:VACUUM:03:VALUE')
     AND time >= NOW -1h
     AND time <= NOW" \
  > samples.csv
```

### Step 5 — Check PV availability

```bash
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, first_timestamp, last_timestamp, num_buckets
   FROM mldp.pv_stats
   WHERE pv IN ('mldp_sample:MAGNET:01:VALUE',
                'mldp_sample:RF:02:VALUE')"
```

### Step 6 — Explore metadata

```bash
# All sample-namespace PVs
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, description, modified_by
   FROM mldp.pv_metadata
   WHERE pv PREFIX 'mldp_sample'"

# PVs tagged 'magnet'
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT pv, alias, description, attr.device_group
   FROM mldp.pv_metadata
   WHERE tag = 'magnet'"
```

### Step 7 — Query configurations

```bash
# All sample configurations
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT name, category, description
   FROM mldp.configuration
   WHERE name PREFIX 'mldp_sample'"

# Active configurations 30 minutes ago
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT name, activation_id, time
   FROM mldp.active_configurations
   WHERE at = NOW -30m"
```

### Step 8 — Join time-series with metadata

```bash
mldp_pvxs_driver -c query-config.yaml query \
  "SELECT ts.pv, ts.time, ts.value, m.description, m.attr.units
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
