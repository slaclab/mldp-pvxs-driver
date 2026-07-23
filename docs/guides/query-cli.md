# Query CLI Guide

The `query` subcommand runs one SQL statement end-to-end (parse → plan → execute → render) and prints results to stdout.

> **Related:** [Query Clients](../dev/query-client.md) | [Query Engine Architecture](../reference/query-engine-architecture.md) | [Configuration Reference](configuration.md#queryable-block)

---

## Command

```bash
mldp_pvxs_driver [global options] query [query options] "<SQL>"
```

Accepted options:

| Option | Default | Purpose |
|---|---:|---|
| `--file <path>` | none | Read SQL text from file instead of positional SQL argument. |
| `--format <table|json|csv|arrow>` | `table` | Output format. |
| `--no-stats` | off | Suppress query stats footer. |
| `--memory-mb <n>` | `256` | Memory budget for query execution context (MiB). |
| `--spill-dir <path>` | system temp + `/mldp-query-spill` | Directory for spill files when memory pressure triggers spill. |
| `--spill-partitions <n>` | `16` | Spill partition count used by join spill paths. |
| `--join-batch-size <n>` | `100` | Batch size hint for join execution paths. |

Global config input is still owned by the main CLI parser:

| Global option | Purpose |
|---|---|
| `-c`, `--config <source>` | Add config source (file path or dotted `PATH=VALUE`). Repeatable. Must appear before `query`. |

---

## Usage

Pass SQL as positional text:

```bash
mldp_pvxs_driver -c config.yaml query "SELECT pv FROM mldp.time_series WHERE pv = 'MY:PV' LIMIT 5"
```

Or read SQL from file:

```bash
mldp_pvxs_driver -c config.yaml query --file queries.sql
```

Schema introspection works without backend connection config:

```bash
mldp_pvxs_driver query "SHOW TABLES"
mldp_pvxs_driver query "DESCRIBE mldp.pv_metadata"
```

---

## Query-Only Configuration

`query` mode prepares only `queryable:` backends. It does not require readers/writers/routing/metrics blocks.

```yaml
queryable:
  mldp:
    mldp-pool:
      ingestion-url: grpc://ingest:50051
      query-url: grpc://query:50052
      min-conn: 1
      max-conn: 2
  mldp-pv-metadata:
    mldp-pv-metadata-pool:
      annotation-url: grpc://annotation:50053
      min-conn: 1
      max-conn: 2
```

---

## Tuning Notes

- Increase `--memory-mb` when joins spill too often and memory is available.
- Set `--spill-dir` to fast local storage when spill is expected.
- Adjust `--spill-partitions` and `--join-batch-size` for large join workloads.
- Query execution reports row counts; deeper engine internals are documented in [Query Engine Architecture](../reference/query-engine-architecture.md).
