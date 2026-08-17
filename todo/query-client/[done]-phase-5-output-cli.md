# Phase 5 — Output + CLI Wiring

← [Back to main plan](query-client-impl.md)

## Goal

Wire the `query` subcommand end-to-end (parse → plan → execute → render) and expose output formats plus runtime controls without requiring reader/writer controller config.

## CLI Behavior Contract

- Query mode accepts SQL as positional text or `--file`.
- Query mode uses merged config source loading but consumes only `queryable:` subtree.
- Missing `reader`, `writer`, `routing`, or `metrics` config is valid in query mode.
- `SHOW TABLES` / `DESCRIBE` require no backend connection config.

## Tasks

- [ ] `src/cli/query/QueryFormatter.h/.cpp` — renders `arrow::RecordBatch` stream to:
  - `table` (default): columnar aligned ASCII via `arrow::PrettyPrint`
  - `json`: `arrow::json::WriteJSON` → JSON Lines to stdout
  - `csv`: `arrow::csv::WriteCSV`
  - `arrow` (binary): `arrow::ipc::MakeStreamWriter` → raw Arrow IPC to stdout (pipe-friendly)
- [ ] Print `QueryStats` footer unless `--no-stats`
- [ ] `src/cli/QueryCommand.h/.cpp` — top-level arg parse (`-c`, `--file`, `--format`, `--memory-mb`, `--spill-dir`, `--spill-partitions`, `--join-batch-size`) + pipeline: parse → plan → execute → format; build `ExecutionContext`; call `SpillManager::cleanup()` in destructor

## Required CLI Flags

- `-c, --config` (file path or inline override key=value source)
- `--file` for SQL file input
- `--format` in `{table,json,csv,arrow}`
- `--no-stats`
- `--memory-mb`
- `--spill-dir`
- `--spill-partitions`
- `--join-batch-size`

## Output Semantics

- `table`: human-readable console rendering.
- `json`: JSON Lines suitable for machine processing.
- `csv`: flat CSV with stable column order from final projection.
- `arrow`: Arrow IPC stream to stdout for pipe/zero-copy workflows.

Stats footer format (unless `--no-stats`):
`-- N rows (M from backend, K filtered) in Xms | Y RPC | Z bytes spilled | W MB peak`

## Query-Only Config Example

```yaml
queryable:
  mldp:
    - ingestion-url: "localhost:50051"
      query-url: "localhost:50052"
      min-conn: 1
      max-conn: 2
  mldp-annotation:
    - annotation-url: "localhost:50053"
      min-conn: 1
      max-conn: 2
```

Inline overrides are also valid via repeated `-c key=value`.

## CLI Examples

```bash
mldp_pvxs_driver query -c config.yaml "SELECT * FROM mldp.time_series WHERE pv IN ('MY:PV')"
mldp_pvxs_driver query -c config.yaml "..." --format json
mldp_pvxs_driver query -c config.yaml "..." --format csv
mldp_pvxs_driver query -c config.yaml --file query.sql
mldp_pvxs_driver query "SHOW TABLES"
mldp_pvxs_driver query "DESCRIBE mldp.pv_metadata"
```

## Notes

- `SpillManager::cleanup()` runs unconditionally on command teardown, including error paths.
- `arrow_flight` server mode stays Phase 7 and is not part of this CLI phase.
