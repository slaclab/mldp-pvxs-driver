# Phase 10 — Final Server-Cursor Streaming Acceptance

← [Back to main plan](query-client-impl.md)

## Goal

Close the remaining Phase 9 acceptance gaps with protocol-level evidence and a
reusable physical wide-pivot boundary. Phase 9 already establishes pull-based
long-form streaming, deterministic window sharding, live REPL continuations,
subquery window options, IPC spill, and externally merged wide output.

Phase 10 proves cursor lifecycle behavior against a controllable gRPC service,
proves cancellation cleanup through every long/wide stage, and extracts the
wide pivot from the table scan into an explicit physical execution state.

## Scope

- Add a gRPC test service for `queryDataBidiStream` that records every client
  message and can control response release, terminal status, and cancellation.
- Add a physical pivot node and `PivotExecutionState` for long-form
  `pv,time,value` streams. It owns spill-run creation, timestamp merge,
  duplicate-cell validation, and bounded wide-batch output. The
  `mldp.time_series_table` scan supplies the long stream; it does not contain
  pivot mechanics.
- Preserve the Phase 9 SQL surface, default shard values, and output formats.
  Wide output remains available only after pivot preparation; do not add a
  user-facing `STREAM` or `PIVOT` clause.

## Required behavior

### Bidi cursor lifecycle

- Each `(normalized range, time slice, PV group)` opens exactly one bidi RPC
  and sends one initial `QuerySpec` containing its exact inclusive bounds and
  PV names in predicate order.
- The client sends `CURSOR_OP_NEXT` only after the consumer pulls beyond the
  prior response-derived Arrow batch. A shard reaches terminal `Finish()` once
  and propagates non-OK status even when the final read appeared clean.
- Cancellation calls `ClientContext::TryCancel`; destroying an incomplete
  stream completes/cancels the RPC before its pooled handle is released.
- A live REPL continuation consumes retained rows before requesting the next
  server response, then advances only to the next deterministic shard. Invalid,
  expired, rotated, and cross-process tokens remain errors rather than fresh
  query restarts.

### Physical wide pivot

- Add `PhysicalPivot` with a long-form input, requested PV order, and bounded
  output batch size. `PivotExecutionState` creates Arrow IPC input spill and
  sorted runs, k-way merges by timestamp, and emits wide batches without
  materializing all long-form input batches.
- Emit `time` first, followed by PV columns in predicate order. Missing cells
  are null; duplicate `(time,pv)` cells and mixed active value types for one PV
  are execution errors.
- Check cancellation while ingesting, sorting, merging, and emitting. Spill
  files are released on success, error, cancellation, and abandoned state.
- Report shard open/response/next/completion plus spill, sort, merge, and pivot
  progress through `QueryProgressTracker`; count backend response rows
  independently from wide output rows.

## Tests and acceptance

- gRPC mock tests assert initial `QuerySpec`, ordered `CURSOR_OP_NEXT`, no
  repeated full-range request, terminal `Finish()` error propagation, and
  `TryCancel` after cancellation or stream destruction.
- Formatter/backpressure test proves that the first JSON/CSV/Arrow IPC batch is
  written before the mock releases the next cursor response.
- Long-form tests cover literal and subquery windows, multiple normalized
  ranges, adjacent-boundary de-duplication, retained batch offsets, token
  rotation, cancellation, REPL cleanup, and idle expiry.
- Pivot tests cover aligned and disjoint timestamps, null cells, requested PV
  order, duplicate-cell and mixed-type failures, multiple sorted spill runs,
  cancellation during each pivot stage, and globally ascending timestamps.
- Run:

  ```sh
  cmake --build /workspace/build --target mldp_pvxs_driver_test --parallel
  ctest --test-dir /workspace/build -R "(MLDPQueryClientTest|QueryParserTest|QueryPlannerExecutorTest|QueryRunnerTest|QueryContinuationRegistryTest|QueryableMldpIntegrationTest|QueryFormatterTest)" --output-on-failure
  git diff --check
  ```

## Completion criteria

Phase 10 is complete only when the protocol mock and deterministic unit tests
cover all lifecycle/cancellation cases above, the pivot is represented by a
physical execution node, the focused suite passes in the devcontainer, and the
existing MLDP integration tests still pass.
