# Query CLI Guide

The `query` subcommand runs the embedded query engine in **line-oriented stdin mode**. It prepares configured query backends, then executes one query per input line.

> **Related:** [Query Clients](../dev/query-client.md) | [Query Engine Architecture](../reference/query-engine-architecture.md) | [Configuration Reference](configuration.md#queryable-block)

---

## Command

```bash
mldp_pvxs_driver query [options]
```

Accepted options:

| Option | Default | Purpose |
|---|---:|---|
| `-c`, `--config <source>` | none | Add config input (file path or dotted `PATH=VALUE` override). Repeatable. |
| `--memory-mb <n>` | `256` | Memory budget for query execution context (MiB). |
| `--spill-dir <path>` | system temp + `/mldp-query-spill` | Directory for spill files when memory pressure triggers spill. |
| `--spill-partitions <n>` | `16` | Spill partition count used by join spill paths. |
| `--join-batch-size <n>` | `100` | Batch size hint for join execution paths. |

Unknown options are rejected. The current implementation does not expose a dedicated `query --help`; use this guide for supported parameters.

---

## Interactive Usage

Start the subcommand, then type one statement per line. End with `exit` or `quit`.

```bash
mldp_pvxs_driver query -c config.yaml
SHOW TABLES
DESCRIBE mldp.pv_metadata
SELECT pv FROM mldp.time_series WHERE pv = 'MY:PV' LIMIT 5
exit
```

Supported statements depend on parser/planner support and currently include `SELECT`, `EXPLAIN`, `SHOW TABLES`, and `DESCRIBE`.

---

## Running from a File or Pipe

Because query input is read from `stdin`, you can run batch queries with redirection:

```bash
mldp_pvxs_driver query -c config.yaml < queries.sql
```

or:

```bash
cat queries.sql | mldp_pvxs_driver query -c config.yaml
```

---

## Query-Only Configuration

`query` mode prepares only `queryable:` backends. It does not require readers/writers/routing blocks.

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
