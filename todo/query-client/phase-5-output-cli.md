# Phase 5 — Output + CLI Wiring

← [Back to main plan](query-client-impl.md)

## Tasks

- [ ] `src/cli/query/QueryFormatter.h/.cpp` — renders `arrow::RecordBatch` stream to:
  - `table` (default): columnar aligned ASCII via `arrow::PrettyPrint`
  - `json`: `arrow::json::WriteJSON` → JSON Lines to stdout
  - `csv`: `arrow::csv::WriteCSV`
  - `arrow` (binary): `arrow::ipc::MakeStreamWriter` → raw Arrow IPC to stdout (pipe-friendly)
- [ ] Print `QueryStats` footer unless `--no-stats`
- [ ] `src/cli/QuerySubcommand.h/.cpp` — top-level arg parse (`-c`, `--file`, `--format`, `--memory-mb`, `--spill-dir`, `--spill-partitions`, `--join-batch-size`) + pipeline: parse → plan → execute → format; build `ExecutionContext`; call `SpillManager::cleanup()` in destructor

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

- `arrow` binary format enables zero-copy pipe to downstream tools
- `QueryStats` footer format: `-- N rows (M from backend, K filtered) in Xms | Y RPC | Z bytes spilled | W MB peak`
- `SpillManager::cleanup()` called unconditionally in destructor — no leaking spill files on error paths
