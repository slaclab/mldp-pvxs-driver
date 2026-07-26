# Phase 8 — Streamable `IQueryable` + Incremental Arrow IPC

← [Back to main plan](query-client-impl.md)

## Goal

Make `IQueryable` expose a lazy Arrow `RecordBatch` source so the query CLI
and REPL can consume data while a backend is still producing it.  In Arrow
mode the CLI writes an Arrow IPC stream directly to stdout as batches arrive.

For multi-PV time-series exports, make the streamable wide result
(`time | PV1 | PV2 | ...`) by materializing the naturally streamed long data
(`pv | time | value`) into a temporary spill-backed Arrow table and pivoting it
inside the query engine.  Do not depend on a backend ordering guarantee to
align independent PV buckets in memory.

## Contract

- Add `IRecordBatchStream::next()`, returning one batch at a time and `nullptr`
  on clean EOF.  A stream owns backend resources and reports protocol/terminal
  RPC errors from `next()`.
- Add `IQueryable::executeStream(...)`; retain `execute(...)` as a compatibility
  adapter while all queryables and test doubles migrate.
- The default adapter follows the existing continuation-token protocol, so all
  current queryables become streamable without changing their backend RPCs.

## MLDP Time-Series Stream

- Implement `mldp.time_series` with dp-grpc 1.14
  `DpQueryService.queryDataBidiStream`.
- Send one `QuerySpec` containing the time range and all requested PV names.
  Convert every returned `DataBucket` into long-form Arrow rows (`pv`, `time`,
  `value`, `column_type`, and requested metadata), then request
  `CURSOR_OP_NEXT` only after the caller consumes the response.
- Validate timestamps/value lengths and exceptional results; call `Finish()`
  and propagate its terminal status.  Cancellation/destruction must release
  unfinished gRPC activity.

## Streamable Wide Time-Series Table

- Implement `mldp.time_series_table` as a query-engine pivot over the streamed
  long-form source instead of attempting to align per-PV gRPC buckets directly.
- Stream every long-form batch into an Arrow IPC temporary table through
  `SpillManager`.  The temporary table has the stable internal columns `pv`,
  `time`, and `value`; retain source metadata only when the requested output or
  local predicates need it.
- Add a physical `Pivot` node and `PivotExecutionState` with fixed semantics:
  - pivot key: `time`;
  - output-column key: `pv`;
  - cell value: `value`;
  - output schema: `time` followed by requested PV names in `pv IN (...)` / PV
    predicate order;
  - absent PV/timestamp combinations become Arrow nulls;
  - duplicate `(time, pv)` rows are an error, never silently overwritten.
- The pivot reads the temporary long table in spill-bounded partitions, groups
  rows by timestamp, and emits bounded wide `RecordBatch` chunks.  It must not
  require the complete raw source in memory.
- A pivot is a blocking operation: it may not emit a timestamp until its
  materialized partition has been completely read.  This preserves correct
  wide-table semantics when dp-query interleaves PVs or returns buckets out of
  time order.
- Preserve the current unary `queryTable` implementation as an optional small
  result fast path.  The planner selects it only when an explicit configured
  threshold is met; otherwise `mldp.time_series_table` uses the streaming
  long-table pivot path.  The streaming/pivot path is the correctness baseline.

## Executor and Output

- Convert scan, filter, project, and limit states to pull one batch and yield
  one batch.  Blocking operations (sort, joins, aggregations, and table
  creation) drain into the existing spill/materialization mechanism before
  yielding their complete SQL-correct result.
- Route a query selecting `mldp.time_series_table` through the long-form MLDP
  scan plus `PivotExecutionState`; do not expose an ad-hoc external PIVOT SQL
  syntax in this phase.  A future general `PIVOT` operator may reuse this
  execution state once parser/planner syntax is designed.
- Have `--format arrow` and REPL Arrow mode write a single
  `arrow::ipc::RecordBatchStreamWriter` directly to stdout, flushing each
  batch.  Keep diagnostics and final statistics off binary stdout.
- Keep a materialization adapter for table, JSON, CSV, expanded display, and
  compatibility callers until those consumers are made incremental.

## Tests

- Mock `queryDataBidiStream` with multi-PV, multi-bucket, multi-response data;
  assert the initial request includes all PVs and `CURSOR_OP_NEXT` follows
  consumption.
- Verify an Arrow IPC reader receives the first record batch before the mock
  releases the next server response; cover terminal errors, cancellation,
  malformed buckets, timestamps, nulls, and metadata.
- Add pivot tests for multiple PVs, disjoint and aligned timestamps, null
  cells, stable requested-PV column ordering, duplicate `(time,pv)` detection,
  spill partition boundaries, and bounded batch output.
- Cover lazy filter/project/limit and spill-backed blocking operators, while
  retaining unary wide-table fast-path regression tests and REPL/CLI output
  coverage.
