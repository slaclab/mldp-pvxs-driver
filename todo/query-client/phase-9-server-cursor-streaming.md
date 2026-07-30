# Phase 9 — Server-Cursor Streaming for MLDP Time Series

← [Back to main plan](query-client-impl.md)

## Goal

Replace the local `ts:<offset>` pagination used by the long-form
`mldp.time_series` query path with MLDP service-managed `queryDataBidiStream`
cursors. A large time-series query is divided into bounded, deterministic
time/PV shards, receives data incrementally, and avoids rebuilding a full unary
`queryTable` response for every local output page.

Use the same streamed long-form source for `mldp.time_series_table`. The wide
table remains semantically ordered by `time`, so it writes streamed long
batches to spill-backed Arrow IPC storage and performs a globally ordered pivot
before it emits wide batches.

## Scope and non-goals

- Make server-cursor bidi streaming the default execution path for both MLDP
  time-series virtual tables, with user-controlled time/PV shard bounds.
- Preserve all existing `window` forms, the required `pv` predicate, column
  schemas, cancellation behavior, and output-format compatibility. The optional
  `window` shard parameters extend rather than replace the current contract.
- Split each window into sequential, configurable time/PV shards. Do not run
  shards concurrently in this phase: concurrency is a separate, benchmarked
  policy because it can multiply backend/MongoDB work and complicate output
  ordering, cancellation, and resume tokens.
- Do not add user-facing `STREAM` or `PIVOT` SQL syntax. The `window` options
  below are the only user-facing shard controls in this phase.
- Do not promise immediate wide-table rows: strict globally ascending `time`
  output requires stream ingestion and pivot preparation to finish first.

## Window shard parameters and pagination

Extend the existing window input syntax with optional named shard parameters:

```sql
window IN (start, end; slice 1s, series_per_shard 1)
```

- `slice` is a positive duration defining the maximum timestamp span of one
  backend cursor. Its default is `1s` when omitted.
- `series_per_shard` is a positive integer defining the maximum number of PV names in
  one backend cursor. Its default is `1` when omitted.
- The existing forms remain valid and use both defaults:

  ```sql
  window IN (NOW - 1h, NOW)
  window IN (SELECT activation.time, activation.end_time FROM ...)
  ```

- For a window subquery, options follow the subquery result expression:

  ```sql
  window IN (
    SELECT activation.time, activation.end_time
    FROM mldp.configuration_activation activation
    WHERE activation.end_time IS NOT NULL;
    slice 5s, series_per_shard 4
  )
  ```

  The parser treats the semicolon inside the `window IN (...)` production as
  the boundary between the two timestamp outputs and the window options; it
  is not a SQL statement terminator. The implementation must add this as a
  dedicated grammar production rather than overload the top-level statement
  terminator.

- Reject duplicate option names, unknown options, zero/negative values, and
  options on non-MLDP time-series tables at bind/type-check time.

After existing window normalization, the executor partitions every closed
range into consecutive half-open cursor slices `[begin, slice_end)`, except the
final slice whose end equals the inclusive window end. It partitions requested
PVs, in predicate order, into consecutive groups of at most `series_per_shard` names.
It visits shards in this deterministic order: normalized window, time slice,
then PV group. A cursor receives its exact shard bounds and PV group.

```text
window [10:00:00, 10:00:03], slice 1s, series_per_shard 1
PVs [MAG:ONE, RF:ONE]

  [10:00:00, 10:00:01) MAG:ONE  ->  [10:00:00, 10:00:01) RF:ONE
  [10:00:01, 10:00:02) MAG:ONE  ->  [10:00:01, 10:00:02) RF:ONE
  [10:00:02, 10:00:03] MAG:ONE  ->  [10:00:02, 10:00:03] RF:ONE
```

Adjacent slices must not duplicate a boundary sample. The implementation uses
the backend's inclusive timestamp bounds and applies a local half-open filter
to every non-final slice; the final slice retains its inclusive end.

The binder stores resolved values, not merely whether an option was written:
an omitted `slice` becomes `1s` and an omitted `series_per_shard` becomes `1` in the
logical and physical window specification. This makes defaults visible to the
executor and testable without CLI-specific behavior.

### Interactive page versus full materialization

The current grammar already accepts `LIMIT n PAGE TOKEN 'token'`, and carries
the token through binding. It does not yet affect physical planning or
execution: the current executor materializes the full result before applying
`LIMIT`, while `MLDPQueryClient` uses its internal `ts:<offset>` token only to
fetch all unary backend pages. Phase 9 gives `PAGE TOKEN` defined, user-visible
semantics for a streaming interactive query.

For an interactive `SELECT ... LIMIT n`, the executor stops after `n` emitted
rows and returns an opaque token such as `p9:<random-id>`. The token is a key,
not a serialized gRPC cursor or a timestamp range. In the same live REPL/query
session, a session-owned continuation registry maps it to the complete
continuation state:

```text
p9:<random-id>
        |
        v
session continuation registry
  - canonical query fingerprint (including projection, predicates, window,
    slice, series_per_shard, ordering, and limit-compatible output shape)
  - active bidi stream and its pooled gRPC handle / ClientContext
  - normalized-window index, current slice bounds, and PV-group index
  - unconsumed Arrow batch and in-batch row offset
  - cancellation state, last-use time, and expiry deadline
```

The user continues with the same query shape and the token:

```sql
SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('MAG:ONE', 'RF:ONE')
  AND window IN (NOW - 1m, NOW; slice 1s, series_per_shard 1)
LIMIT 100 PAGE TOKEN 'p9:<random-id>'
```

The executor validates the query fingerprint, first emits any rows remaining
in the retained Arrow batch, then reads the active cursor. It sends
`CURSOR_OP_NEXT` only when those buffered rows are exhausted and another server
response is required. At shard EOF it opens the next PV group, then the next
time slice, in deterministic order. Completed shards are never reopened.

```text
page 1: open shard C -> emit 100 rows -> retain row offset + active cursor
page 2: validate token -> emit retained rows -> CURSOR_OP_NEXT if needed
        -> shard C EOF -> open the next deterministic shard
```

Only one active continuation may consume a token at a time. A successful
continuation rotates it to a new opaque token so stale copies fail. The registry
must cancel and release the retained RPC when the user cancels, the query
reaches EOF, the REPL exits, or the configurable idle timeout expires. Invalid,
expired, already-consumed, or query-mismatched tokens fail clearly and must not
fall back to a fresh query.

A server bidi cursor is normally stateful only while its stream remains open.
A token passed to a new one-shot CLI process therefore cannot promise
no-refetch resumption unless the MLDP protocol gains server-issued resumable
cursor IDs. Phase 9 supports live-session continuation only. A later,
explicitly designed stateless mode may restart the current deterministic shard
from a row watermark and deduplicate locally, accepting refetch; it is not
part of this phase.

`CREATE [TEMP] TABLE name AS SELECT ...` is a full-materialization operation:
it never stops at the interactive page boundary. It drains every cursor in
every PV group and time slice, applies all SQL operators, and commits the Arrow
IPC catalog table only after complete successful execution. Cancellation or an
error leaves no newly published table.

## Examples and data flow

### Current unary long-form pagination

The current path receives a complete unary `queryTable` response, expands it
to long-form rows in the driver, and then paginates that local row vector. A
continuation token such as `ts:438` is a driver offset, not a server cursor.
Each continuation repeats the full-range backend request.

```text
User SQL
  SELECT pv, time, value FROM mldp.time_series
  WHERE pv IN ('MAG:ONE', 'RF:ONE')
    AND window IN (10:00, 11:00)
        |
        v
Driver page 1
  QueryTable [10:00, 11:00] --------------------------------------+
        |                                                         |
        v                                                         |
dp-query creates complete column table                               |
        |                                                         |
        v                                                         |
Driver expands/sorts all MAG:ONE + RF:ONE rows                    |
        |                                                         |
        +--> emit rows 0..437 to user                             |
        +--> local token ts:438                                   |
                                                                  |
User asks for next page                                            |
        |                                                         |
        v                                                         |
Driver page 2                                                      |
  QueryTable [10:00, 11:00]  <------------------------------------+
        |
        v
Driver rebuilds all rows, skips 0..437, emits 438..875
```

### Proposed streamed long-form response

For `mldp.time_series`, the driver opens one bidirectional RPC for each
deterministic `(normalized window, time slice, PV group)` shard. The backend
retains cursor state for that bounded request. With `series_per_shard 1`, each cursor
contains exactly one PV; with a larger group, the cursor contains only that
group's PV names. The driver completes a shard before it opens the next shard.
It converts a server response and makes its Arrow batch available while the
consumer is processing the preceding output. It sends `CURSOR_OP_NEXT` only
after the consumer pulls again, which bounds work to consumer demand.

```text
Time ---------------------------------------------------------------------->

User / Arrow IPC consumer | next()        process batch 1        next()
                          |---|----------------------|-------------|
                          |   ^                      |             ^
                          |   |                      |             |
Driver stream             |   | Arrow batch 1        |             | Arrow batch 2
                          |   | (MAG + RF rows)      |             |
                          |   +----------------------+-------------+
                          |      convert response 1       convert response 2
                          |   QuerySpec                     CURSOR_OP_NEXT
                          |------->|                            |------->|
                          |        |                            |        |
dp-query bidi cursor      |  response 1:                  response 2:
  shard [10:00,10:00:01], |  MAG:ONE bucket(s)            MAG:ONE bucket(s)
  PV group [MAG:ONE]      |                              (same shard)
                          |<-------|                            |<-------|

After the cursor reaches EOF, the driver opens the next PV group, then the
next time slice. For the defaults, `MAG:ONE` and `RF:ONE` use separate cursors
for each one-second slice. There is no driver-side join or synchronization
barrier between these requests; wide-table alignment is deferred to the
spill-backed pivot.
```

The backend chooses response boundaries within one bounded shard and retains
cursor position until that shard reaches EOF. There is no `ts:<offset>` token
and no repeated full-range `QueryTable` RPC.

### Client display and file persistence policy

The driver should not wait for a full long-form table before it gives data to
the client. The output sink is the consumer of `IRecordBatchStream`; calling
`next()` is the backpressure boundary. A slow terminal, file system, or Arrow
consumer naturally delays `CURSOR_OP_NEXT`, so the server does not run ahead
without demand.

```text
Time ---------------------------------------------------------------------->

dp-query cursor        response 1             waits for next             response 2
                              |                     |                        |
                              v                     |                        v
Driver stream           Arrow batch 1 --------> output sink ---------> Arrow batch 2
                              |                     ^                        |
                              |                     |                        |
Client / file           write + flush batch 1  |                 write + flush batch 2
  Arrow IPC             IPC message            |                 IPC message
  JSON Lines            complete rows + '\n'    |                 complete rows + '\n'
  CSV                   header once + rows     |                 more rows
                              |                     |
                              +--> durable/visible <--+
                                    before next() requests more
```

Use incremental output for long-form results:

- **Arrow IPC:** open one `RecordBatchStreamWriter`, write and flush each
  batch, then close the writer after clean EOF. Keep diagnostics and final
  statistics off the binary data stream.
- **JSON Lines:** write only complete JSON objects followed by a newline; flush
  after each batch. A partial query leaves a valid prefix of complete records.
- **CSV:** write the header once, then complete rows per batch; flush after
  each batch. A partial query leaves a readable CSV prefix.
- **Table and expanded terminal output:** render each completed batch as it
  arrives. Print the header once and do not attempt retroactive global column
  width alignment; `--table-fit` uses the current terminal width and each
  rendered batch's values. The status footer remains asynchronous and reports
  stream progress while output scrolls.

For a requested durable Arrow data file, write to a temporary sibling path and
atomically rename it only after clean EOF and successful writer close. The
temporary file may be retained for diagnostics or removed on cancellation or
failure; the final destination is never presented as a complete dataset before
the stream succeeds.

### Proposed streamed wide-table response

`mldp.time_series_table` uses the same server cursor but cannot safely expose
a wide timestamp row immediately: a later response may contain another PV for
an earlier timestamp. The driver streams data into temporary Arrow IPC storage,
then externally orders and pivots it before producing globally ordered wide
batches.

```text
User SQL
  SELECT * FROM mldp.time_series_table
  WHERE pv IN ('MAG:ONE', 'RF:ONE')
    AND window IN (10:00, 11:00)
        |
        v
Driver opens bounded queryDataBidiStream cursors sequentially
  [10:00,10:00:01], [MAG:ONE]
  [10:00,10:00:01], [RF:ONE]
  then later time slices
        |
        v
dp-query response stream
  response 1: MAG:ONE @ 10:05, 10:07
  response 2: RF:ONE  @ 10:04, 10:07    (arrival order is not a table order)
        |
        v
Driver converts to long Arrow batches
  pv       | time  | value
  MAG:ONE  | 10:05 | ...
  RF:ONE   | 10:04 | ...
        |
        v
Spill-backed Arrow IPC temporary data
        |
        v
External sort by time + pivot on pv
        |
        v
Wide Arrow batches, globally ordered
  time  | MAG:ONE | RF:ONE
  10:04 | null    | ...
  10:05 | ...     | null
  10:07 | ...     | ...
        |
        v
User receives ordered wide table batches
```

The wide table streams backend ingestion and avoids rebuilding unary tables,
but its first user-visible wide batch follows pivot finalization. This is the
trade-off required to preserve correct null placement, requested PV order, and
global timestamp ordering.

For strict wide-table output, the client waits for pivot finalization before it
receives the first wide batch. It should still receive asynchronous progress
updates (`stream response`, `spill`, `external sort`, and `pivot`) and may
cancel at any point. The temporary spill data is an internal implementation
artifact, not a user-visible completed table or output file.

## Stream contracts

### Queryable and executor

- Introduce `IRecordBatchStream` with `next()`: it returns one Arrow
  `RecordBatch`, returns `nullptr` at clean EOF, and reports backend/protocol
  failures from `next()`.
- Add `IQueryable::executeStream(...)` without removing `execute(...)`. The
  default implementation adapts the legacy continuation-token API so every
  existing queryable remains compatible.
- Add a pull-based executor entry point. Scan, filter, project, and limit pull
  and yield batches lazily. Sort, joins, table creation, and the wide pivot are
  explicitly materializing operators and use the existing spill facilities.
- Carry `PAGE TOKEN` from the bound statement into the interactive physical
  limit/stream execution state. Add a REPL-session continuation registry that
  owns active streams, pending Arrow rows, token rotation, fingerprint
  validation, cancellation, and idle expiry. Do not create this registry for
  one-shot CLI invocations or `CREATE [TEMP] TABLE AS SELECT`.
- Make the long-form formatter consume the pull stream directly: Arrow IPC,
  JSON Lines, CSV, table, and expanded output write completed batches as they
  arrive. Keep a materialization adapter only for compatibility callers and
  blocking physical operators.

### MLDP bidi cursor

- For every deterministic `(normalized window, time slice, PV group)` shard,
  open one `DpQueryService.queryDataBidiStream` and send one initial `QuerySpec`
  with the shard's exact interval and PV names in predicate order. Execute
  shards serially in phase 9.
- Convert each `QueryDataResponse` into one or more long-form Arrow batches:
  `pv`, `time`, `value`, `column_type`, and requested metadata fields.
- Send `CURSOR_OP_NEXT` only after the caller pulls the prior response-derived
  batch. The MLDP service owns cursor position and response chunking.
- Reject exceptional responses, malformed buckets, incompatible timestamp/value
  cardinalities, and unsupported serialized payloads. Check the final
  `Finish()` status even after the last clean response.
- Retain one pooled gRPC handle and one `ClientContext` for each active cursor.
  Cancellation calls `TryCancel`; stream destruction completes or cancels
  unfinished RPC activity before returning the pool handle.
- Process normalized windows, their time slices, and their PV groups serially.
  Each shard has an independent server cursor; ranges are not merged beyond
  the current window-normalization rules.

## Wide-table pivot

- Route `mldp.time_series_table` through the streamed long-form MLDP source;
  do not attempt to align independently streamed PV buckets in memory.
- Materialize long batches into temporary Arrow IPC files through
  `SpillManager`, bounded by the existing memory limit, spill directory, and
  spill-partition configuration.
- Add a physical pivot node and `PivotExecutionState` with fixed semantics:
  - row key: `time`;
  - output-column key: `pv`;
  - cell value: `value`;
  - output fields: `time`, then requested PV names in PV-predicate order;
  - missing `(time, pv)` combinations: Arrow null;
  - duplicate `(time, pv)` combinations: execution error.
- Externally sort/partition the temporary long data by timestamp before the
  pivot so emitted wide batches are globally ascending by `time` regardless of
  backend bucket order. Emit bounded wide batches after pivot finalization.

## Progress, configuration, and compatibility

- Replace the misleading local `window continuation page` status for bidi
  scans with progress details for `shard open`, `stream response`,
  `cursor next`, shard completion, and `spill/pivot finalization`. Include the
  current normalized-window, time-slice, and PV-group positions when totals
  are known.
- Count each received server response and report backend rows independently
  from locally emitted Arrow batches.
- Keep `--join-batch-size` scoped to existing joins/materialization. It is not
  a time-window size or server-stream chunk-size control.
- Retain the unary `queryTable` implementation during migration for regression
  comparison. Do not introduce undocumented threshold heuristics; switch the
  default only after the bidi and pivot paths meet parity tests.

## Tests and acceptance criteria

- Add a gRPC mock implementing `queryDataBidiStream` with multi-PV,
  multi-bucket, multi-response results. Assert one initial `QuerySpec` per
  deterministic shard, exact shard timestamps and PV names, ordered
  `CURSOR_OP_NEXT` messages within a shard, no repeated full-range request,
  terminal status propagation, and cancellation via `TryCancel`.
- Cover omitted window options (`slice = 1s`, `series_per_shard = 1`), explicit values,
  invalid/duplicate options, literal windows, and subquery windows. Verify
  boundary samples appear once across adjacent time slices and that shard order
  is normalized window, time slice, then PV group.
- Test live-session `PAGE TOKEN` continuation, including an in-batch offset and
  active cursor retention; assert that a resume consumes buffered rows before
  sending `CURSOR_OP_NEXT`, then advances to the correct next shard at EOF.
  Test query-fingerprint mismatch, token rotation/single-consumer protection,
  cancellation, REPL shutdown, and idle-expiry cleanup. Reject or clearly
  diagnose use of that token in a different one-shot CLI process; do not
  silently claim a no-refetch resume.
- Verify that an Arrow IPC reader receives the first long-form batch before the
  mock releases its next response. Cover literal and subquery windows plus
  multiple normalized ranges.
- Test pivoted wide output for aligned and disjoint timestamps, stable requested
  PV order, null cells, duplicate `(time, pv)` detection, spill partitions,
  cancellation, and global timestamp ordering.
- Preserve existing unary query regressions until the streaming path replaces
  them. Add MLDP integration coverage against the devcontainer services for
  long-form streaming and wide-table parity.
- Required verification:

  ```sh
  cmake --build /workspace/build --target mldp_pvxs_driver_test --parallel
  ctest --test-dir /workspace/build -R "(MLDPQueryClientTest|QueryPlannerExecutorTest|QueryableMldpIntegrationTest|QueryFormatterTest)" --output-on-failure
  ```
