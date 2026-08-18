# Lazy cursor contract for IRecordBatchStream

## Context

`IRecordBatchStream::next()` is supposed to mean "one bounded unit of forward progress."
This contract holds for `mldp.time_series` (each `next()` = one MongoDB bucket) but is
broken for `mldp.time_series_table`: `executeStream()` falls back to full materialization
(all window slices fetched + entire result spilled to an Arrow IPC temp file) before the
first `next()` returns.

This blocks the calling thread for the entire query duration and prevents the sql-server
cooperative resubmission pattern from working for wide/pivot queries.

## Required change: lazy streaming for mldp.time_series_table

### `src/query/QueryExecutor.cpp` — extend `makeStreamingPlan`

Currently returns `nullptr` for `mldp.time_series_table`, falling back to full
materialization. Extend to return a lazy `IRecordBatchStream` where each `next()` = fetch
+ pivot one window slice.

The stream state machine:

```
State 0: resolve subqueries (pv_metadata IN, configuration_activation window)
         → one next() call per subquery, returns nullptr batch (progress-only tick)
State 1: for each window × each slice:
         → fetch slice from MongoDB via WideTableScan
         → pivot to wide RecordBatch
         → return batch
State 2: EOF → return nullptr
```

Subquery resolution in state 0 currently happens synchronously before the scan starts.
Each subquery evaluation is itself one `next()`-equivalent unit of work — expose it as
such so the worker pool gets to yield between subquery and main scan.

### New class: `LazyWideTableScanStream`

`src/query/executor/scan/LazyWideTableScanStream.cpp`  
`include/query/executor/scan/LazyWideTableScanStream.h`

Wraps `WideTableScan` internal slice loop as a pull stream:

```cpp
class LazyWideTableScanStream final : public IRecordBatchStream {
public:
    // construction is fast — no MongoDB I/O
    LazyWideTableScanStream(plan::PhysicalTableScan scan,
                            ExecutionContext context,
                            std::vector<std::pair<int64_t,int64_t>> windows);

    // each call: fetches+pivots one slice, returns one RecordBatch
    // returns nullptr when all slices exhausted
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    // internal cursor state
    std::size_t window_index_{0};
    int64_t     slice_begin_ns_{0};
    ...
};
```

### Subquery resolution as lazy ticks

Subquery results (PV list, window list) are currently resolved synchronously in
`resolvePushableInSubqueries()` and `extractNormalizedWindows()`. Wrap resolution in a
preamble stream that:
- On first `next()`: resolves all subqueries, returns a zero-row batch (progress tick)
- Subsequent `next()`: delegates to `LazyWideTableScanStream`

This keeps the uniform contract: every `next()` returns quickly, first one does subquery
work, rest do slice work.

## Impact

Once this lands:

- `mldp.time_series_table` queries yield after each slice in the sql-server worker pool
- Cooperative resubmission works uniformly for all query shapes
- No query shape ever monopolizes a worker thread for more than one slice duration (~0.8s
  from observed logs)
- Full async migration later requires only replacing the blocking `next()` body with
  `co_await` — the continuation structure is already in place

## Dependencies

- Prerequisite for `todo/sql-server.md` cooperative resubmission to work for all query shapes
- No changes to `IRecordBatchStream` interface itself — contract clarification only
- No changes to query planner, parser, or binder
