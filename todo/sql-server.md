# Arrow Flight SQL Server + Fully Async Query Engine

Expose the SQL query engine as an Arrow Flight SQL server so external tools
(Python ADBC, Java JDBC, ODBC, DBeaver, Grafana) can query MLDP data over
standard gRPC without needing a CLI session.

The entire query engine — from backend gRPC calls through joins, filters,
subqueries and pivots — is made fully async.  No thread ever blocks on I/O.
The physical planner annotates each node with an `ExecutionHint` that drives a
DAG scheduler; the scheduler submits only I/O-bound and blocking-aggregation
nodes to `BS_thread_pool`, folds cheap CPU operators inline, and chains
continuations automatically so the next node starts the moment its dependencies
complete.

## Client ecosystem

| Client | Protocol |
|--------|----------|
| Python `adbc-driver-flightsql` | Flight SQL native |
| Java `arrow-flight-sql-jdbc-driver` | JDBC over Flight SQL |
| Dremio ODBC driver | ODBC over Flight SQL |
| DBeaver | JDBC plugin |
| `pyarrow.flight` | Flight low-level |
| curl/grpcurl | raw gRPC |

---

## Part 1 — Node classification and planner annotation pass

### 1.1  `ExecutionHint` enum

New file: `include/query/plan/ExecutionHint.h`

```cpp
namespace mldp_pvxs_driver::query::plan {

enum class ExecutionHint {
    // Submit to BS_thread_pool immediately; issues async gRPC, no children.
    IO_LEAF,

    // Fold into parent task — pure in-memory transform (µs–ms, no I/O).
    // Filter, Project, Limit fall here.
    CPU_PIPELINE,

    // Submit to BS_thread_pool after all children complete.
    // HashJoin, BlockNestedLoopJoin, Sort, Pivot fall here.
    BLOCKING_AGGREGATE,

    // Never submitted independently.
    // Inner side of a correlated NestedLoopJoin — driven row-by-row by outer.
    CORRELATED_INNER,
};

} // namespace mldp_pvxs_driver::query::plan
```

Add field to `PhysicalNode` (`include/query/plan/PhysicalPlan.h`):

```cpp
struct PhysicalNode {
    PhysicalNodeVariant value;
    ExecutionHint       hint{ExecutionHint::CPU_PIPELINE}; // set by annotation pass
};
```

### 1.2  Annotation pass

New files: `include/query/planner/ExecutionAnnotator.h`,
`src/query/planner/ExecutionAnnotator.cpp`

```cpp
// Single O(N) tree walk — pure structural rules, no cost model.
plan::PhysicalNodePtr annotateExecutionHints(plan::PhysicalNodePtr root);
```

Rules (applied bottom-up):

| Node type | Hint |
|---|---|
| `PhysicalTableScan` | `IO_LEAF` |
| Synthetic subquery leaf (see §4.2) | `IO_LEAF` |
| `PhysicalFilter` | `CPU_PIPELINE` |
| `PhysicalProject` | `CPU_PIPELINE` |
| `PhysicalLimit` | `CPU_PIPELINE` |
| `PhysicalSort` | `BLOCKING_AGGREGATE` |
| `PhysicalPivot` | `BLOCKING_AGGREGATE` |
| `PhysicalHashJoin` | `BLOCKING_AGGREGATE` |
| `PhysicalBlockNestedLoopJoin` | `BLOCKING_AGGREGATE` |
| `PhysicalNestedLoopJoin` (`correlated_push=false`) | `BLOCKING_AGGREGATE` |
| `PhysicalNestedLoopJoin` inner when `correlated_push=true` | `CORRELATED_INNER` |
| DDL statements | `CPU_PIPELINE` (run inline, trivial) |

`CPU_PIPELINE` nodes carry no separate `PlanTask` — they execute inside their
nearest non-`CPU_PIPELINE` ancestor's task body.

### 1.3  Integration into `QueryPlanner::plan()`

`QueryPlanner::plan()` (`src/query/QueryPlanner.cpp`) appends
`annotateExecutionHints` as the final optimization pass after
`applyCorrelatedPushOptimizer`.  The CLI path and the server path both receive
annotated plans automatically.

---

## Part 2 — `IRecordBatchStream` async contract

### 2.1  `nextAsync()` on `IRecordBatchStream`

Add to `include/query/IQueryable.h`:

```cpp
// Default: wraps synchronous next() — zero-cost fallback for impls that
// stay sync (CLI path, tests).
virtual std::future<std::shared_ptr<arrow::RecordBatch>> nextAsync()
{
    return std::async(std::launch::deferred, [this]{ return next(); });
}
```

Async-capable streams override `nextAsync()` directly and never block.
The DAG scheduler always calls `nextAsync()`; the CLI path keeps calling
`next()` unchanged.

### 2.2  `BatchQueue` primitive

New files: `include/query/BatchQueue.h`, `src/query/BatchQueue.cpp`

```cpp
// Thread-safe bounded queue: gRPC callback thread pushes, pool worker pops.
class BatchQueue {
public:
    explicit BatchQueue(std::size_t capacity);

    // Called from gRPC callback thread.
    void push(std::shared_ptr<arrow::RecordBatch> batch); // nullptr = EOF
    void push_error(std::exception_ptr ex);

    // Returns future resolved when next item available.
    // If queue full, push blocks briefly (capacity small → sub-ms).
    std::future<std::shared_ptr<arrow::RecordBatch>> popAsync();
};
```

Implemented with `std::mutex` + `std::condition_variable` + `std::deque`.

---

## Part 3 — Async backend migrations (`IQueryable` implementations)

Each migration replaces a blocking gRPC call with the gRPC async callback API.
The calling thread returns before the RPC completes; results arrive via
`BatchQueue` and are exposed through `nextAsync()`.

### 3.1  `MLDPQueryClient` — `mldp.time_series` (bidi stream)

**File:** `src/query/impl/mldp/MLDPQueryClient.cpp` lines 366–389  
**Current:** `MldpBidiRecordBatchStream::next()` → `ClientReaderWriter::Read()`
blocks thread per bucket.

**New class:** `AsyncMldpBidiRecordBatchStream` using `grpc::ClientBidiReactor`
- constructor: starts RPC, issues first `StartRead()` — returns immediately
- `OnReadDone(ok)`: builds `RecordBatch`, pushes to `BatchQueue(capacity=4)`,
  issues next `StartRead()`
- `OnDone(status)`: pushes `nullptr` EOF sentinel
- `nextAsync()`: returns `queue_.popAsync()`

Old `MldpBidiRecordBatchStream` retained for CLI/test path.
`MLDPQueryClient::executeStream` selects via `context.async_backend` flag
(default `true` when server enabled).

### 3.2  `MLDPQueryClient` — `mldp.time_series_table` single-shard (unary)

**File:** `src/query/impl/mldp/MLDPQueryClient.cpp` lines 644–755  
**Current:** `stub->queryTable(ctx, req, &resp)` — synchronous unary.

**New class:** `AsyncWideTableStream`
- constructor: submits `stub->async()->queryTable(ctx, &req, &resp, callback)`
- callback: builds `RecordBatch`, pushes to `BatchQueue(capacity=1)`
- `nextAsync()`: `queue_.popAsync()`; EOF on second call

Thread free for full RPC duration.

### 3.3  `MLDPQueryClient` — `mldp.time_series_table` multi-shard

**File:** `src/query/impl/mldp/MLDPQueryClient.cpp` lines 495–641  
**Current:** `std::async(launch::async, run_shard)` × N — N OS threads blocked.

**New class:** `AsyncShardedWideTableStream`
- constructor: submits all N `stub->async()->queryTable(...)` simultaneously
- `atomic<int> pending` counts in-flight RPCs
- each callback pushes result to `BatchQueue(capacity=N)`
- `nextAsync()`: pops one shard batch; EOF when all N consumed
- any callback error: pushed as error sentinel, rethrown by `nextAsync()`

Replaces N OS threads with zero extra threads — all RPCs in flight on gRPC
completion threads.

### 3.4  `MLDPQueryClient` — `mldp.pv_stats` multi-shard

**File:** `src/query/impl/mldp/MLDPQueryClient.cpp` lines 395–487  
**Current:** same `std::async` pattern as §3.3.

**New class:** `AsyncShardedPvStatsStream` — identical pattern to §3.3 using
`stub->async()->queryPvStats(...)`.

### 3.5  `MLDPAnnotationQueryClient` — paginated unary RPCs

**File:** `src/query/impl/mldp/MLDPAnnotationQueryClient.cpp` line 243+  
**Tables:** `mldp.pv_metadata`, `mldp.configuration`, `mldp.configuration_activation`  
**Current:** `queryAllPages<T>(...)` loop — each page synchronous.

**New class:** `AsyncAnnotationPageStream<RequestT, RecordT>`
- constructor: submits first page RPC
- page N callback: builds batch, pushes to queue, submits page N+1 RPC
  (pipelined — next RPC in flight while caller processes current batch)
- EOF when callback receives empty `next_page_token`
- `nextAsync()`: `queue_.popAsync()`

### 3.6  `ParallelSeriesRecordBatchStream`

**File:** `src/query/impl/mldp/ParallelSeriesRecordBatchStream.cpp` line 103  
After §3.1: replace sync `client_.executeStream()` drain with
`AsyncMldpBidiRecordBatchStream` per shard; shards interleave through a shared
`BatchQueue` — no shard blocks another.

### 3.7  `WideTableScan` / `WindowBackendScanRecordBatchStream`

**Files:** `src/query/executor/scan/WideTableScan.cpp:418`,
`src/query/executor/WindowBackendScanRecordBatchStream.cpp:175`  
After §3.1–3.4: callers automatically receive async streams.  CLI path calls
`next()` (unchanged).  DAG scheduler path calls `nextAsync()` — these are
`IO_LEAF` tasks, driven by the scheduler (see Part 4).

---

## Part 4 — DAG Plan Scheduler

The physical plan tree IS the dependency graph.  The scheduler walks it once,
creates one `PlanTask` per non-`CORRELATED_INNER` node, submits all leaves
simultaneously, and chains continuations automatically.

### 4.1  `PlanTask`

New file: `include/query/executor/PlanTask.h`

```cpp
struct PlanTask {
    plan::PhysicalNodePtr                        node;
    plan::ExecutionHint                          hint;
    std::atomic<int>                             pending_children{0};
    PlanTask*                                    parent{nullptr};
    std::vector<PlanTask*>                       children;
    executor::RecordBatches                      result;
    std::exception_ptr                           error;
    // Non-null only on root task.
    std::shared_ptr<std::promise<executor::RecordBatches>> root_promise;
};
```

### 4.2  DAG build: subquery injection

`PhysicalTableScan` nodes carry implicit dependencies (`in_subqueries`,
`window_subquery`).  During DAG build these become **synthetic `IO_LEAF`
children** of the scan task:

```
PhysicalTableScan [IO_LEAF, pending=2]
  SyntheticSubqueryTask_IN_1  [IO_LEAF, pending=0] ← submit immediately
  SyntheticSubqueryTask_WINDOW [IO_LEAF, pending=0] ← submit immediately
```

When a synthetic subquery task completes it injects its result as a resolved
predicate into the parent scan's `PhysicalTableScan` node, then decrements
`parent->pending_children`.  When `pending_children` reaches 0 the scan itself
is submitted to the pool.

### 4.3  DAG build algorithm

```cpp
// include/query/executor/PlanScheduler.h
class PlanScheduler {
public:
    PlanScheduler(BS::thread_pool& pool, const ExecutionContext& context,
                  QueryStats& stats);

    // Builds task DAG from annotated plan, submits all leaves, returns future.
    std::future<executor::RecordBatches> schedule(plan::PhysicalNodePtr root);

private:
    PlanTask* buildTask(plan::PhysicalNodePtr node, PlanTask* parent);
    void      submit(PlanTask* task);
    void      onTaskComplete(PlanTask* task);

    BS::thread_pool&        pool_;
    ExecutionContext         context_;
    QueryStats&             stats_;
    // Owns all tasks for lifetime of the query.
    std::vector<std::unique_ptr<PlanTask>> tasks_;
};
```

Build steps:
1. Walk plan top-down, create `PlanTask` per node (skip `CORRELATED_INNER`).
2. For each `PhysicalTableScan` with subqueries: inject synthetic `IO_LEAF`
   children, wire parent pointer back to scan task.
3. `CPU_PIPELINE` nodes: do NOT create a separate task — mark for inline fold
   into nearest non-`CPU_PIPELINE` ancestor.
4. Collect all tasks where `pending_children == 0` → submit all to pool.

### 4.4  Task body dispatch

Each task runs on a pool worker.  Dispatch by `hint`:

**`IO_LEAF` (BackendTableScan, synthetic subquery):**
```cpp
auto stream = makeAsyncStream(node, context_);  // returns async IRecordBatchStream
while (true) {
    auto batch = stream->nextAsync().get();      // wait for gRPC callback
    if (!batch) break;
    task->result.push_back(batch);
}
// fold any CPU_PIPELINE children inline (filter, project, limit)
task->result = applyCpuPipeline(node, task->result, context_);
onTaskComplete(task);
```

**`BLOCKING_AGGREGATE` (HashJoin, BlockNestedLoopJoin, Sort, Pivot):**
```cpp
// Children already completed — results are in child->result.
// CPU work only: join, sort, pivot.
task->result = executeAggregate(node, childResults(task), context_, stats_);
// fold CPU_PIPELINE children inline
task->result = applyCpuPipeline(node, task->result, context_);
onTaskComplete(task);
```

**`BLOCKING_AGGREGATE` (correlated NestedLoopJoin, `correlated_push=true`):**
```cpp
// Outer side already in child[0]->result.
// Inner side is CORRELATED_INNER — driven here row-by-row, not a separate task.
for (const auto& outer_row : child[0]->result) {
    auto inner_stream = makeAsyncStreamWithPush(inner_node, outer_row, context_);
    RecordBatches inner;
    while (auto b = inner_stream->nextAsync().get()) inner.push_back(b);
    task->result += joinRow(outer_row, inner, node.condition, node.type);
}
onTaskComplete(task);
```

**`onTaskComplete`:**
```cpp
void PlanScheduler::onTaskComplete(PlanTask* task) {
    if (task->parent == nullptr) {
        task->root_promise->set_value(std::move(task->result));
        return;
    }
    if (--task->parent->pending_children == 0)
        submit(task->parent);
}
```

### 4.5  `IExecutionState` kept for CLI path

`IExecutionState::execute()` is unchanged.  `QueryExecutor::execute()` (CLI,
tests) still calls `makeExecutionState(root)` → synchronous recursive tree.
`QueryExecutor::executeStream()` now delegates to `PlanScheduler::schedule()`
when `context.async_backend = true` (default when server enabled).

---

## Part 5 — `QueryExecutor` integration

`QueryExecutor::executeStream()` (`src/query/QueryExecutor.cpp` line 220)
becomes:

```cpp
QueryStreamExecutionResult QueryExecutor::executeStream(
    const plan::PhysicalNodePtr& root, ExecutionContext context) const
{
    if (!context.async_backend)
        return legacyExecuteStream(root, std::move(context)); // old path

    auto stats  = std::make_shared<QueryStats>();
    auto sched  = std::make_shared<PlanScheduler>(*context.pool_threads, context, *stats);
    auto future = sched->schedule(root);

    // Wrap future as IRecordBatchStream so callers (CLI, Flight server) are uniform.
    auto stream = std::make_unique<FutureRecordBatchStream>(std::move(future));
    return QueryStreamExecutionResult{
        .stream = std::make_unique<FinalizingRecordBatchStream>(
                      std::move(stream), std::move(context), stats,
                      std::chrono::steady_clock::now()),
        .stats  = std::move(stats)};
}
```

`FutureRecordBatchStream` wraps `future<RecordBatches>`, yielding batches one
at a time from the resolved vector.  `nextAsync()` on it returns immediately if
the future is already resolved.

---

## Part 6 — Arrow Flight SQL server

### CMake option

```cmake
option(MLDP_QUERY_SERVER_FLIGHT "Build Arrow Flight SQL query server" OFF)
```

When ON: enables `ARROW_WITH_GRPC`, `ARROW_FLIGHT`, `ARROW_FLIGHT_SQL`.
Update condition at `CMakeLists.txt` line ~210:
```cmake
if(MLDP_QUERY_CLIENT_FLIGHT OR MLDP_QUERY_SERVER_FLIGHT)
```
Add `arrow_flight_static` + `arrow_flight_sql_static` to main executable link.

### New subcommand

```
mldp_pvxs_driver sql-server [--config <yaml>] [--port 47470] [--bind 0.0.0.0]
                             [--query-threads N]   # default = cpu_count
                             [--grpc-threads N]    # default = 4
```

Dispatch from `mldp_pvxs_driver_main.cpp` (same early-subcommand pattern as
`query`, lines 315–342).  Guard with `#ifdef MLDP_QUERY_SERVER_FLIGHT`.

### `include/cli/QueryFlightSqlServer.h`

```cpp
#ifdef MLDP_QUERY_SERVER_FLIGHT
class QueryFlightSqlServer {
public:
    struct Options {
        uint16_t    port{47470};
        std::string bind{"0.0.0.0"};
        uint32_t    grpc_threads{4};
        uint32_t    query_threads{0}; // 0 = cpu_count
    };
    QueryFlightSqlServer(const config::Config& cfg, Options opts);
    void serve(); // blocks until stop()
    void stop();
private:
    std::unique_ptr<arrow::flight::FlightServerBase> impl_;
};
#endif
```

### `src/cli/QueryFlightSqlServer.cpp`

Implements `arrow::flight::sql::FlightSqlServerBase`:

**`GetFlightInfo(CommandStatementQuery)`**
- Parse + plan SQL eagerly (validates syntax/schema)
- `ParseError` / `PlannerException` → gRPC `INVALID_ARGUMENT`
- Ticket = SQL text (UTF-8) — stateless, any replica redeems it
- Return `FlightInfo` with one endpoint

**`DoGetStatement(ticket)`**
- Deserialize ticket → SQL
- `context.async_backend = true`
- `QueryExecutor::executeStream()` → `PlanScheduler::schedule()` launches DAG
- `BatchQueue(capacity=3)` fed by `scheduleNext()` loop (see Part 7)
- Return `QueueFlightDataStream` backed by queue

**`DoPutCommandStatementUpdate`** (optional)
- Forward DDL to `QueryRunner::run()`

### `src/cli/sql_server_subcommand.cpp`
- Parses CLI flags
- `QueryCommandPreparer::prepare(config)`
- `QueryFlightSqlServer::serve()`

---

## Part 7 — Flight server worker loop

```cpp
void scheduleNext(BS::thread_pool&                    pool,
                  std::shared_ptr<IRecordBatchStream> stream,
                  std::shared_ptr<BatchQueue>         out_queue)
{
    pool.detach_task([&pool, stream, out_queue]() {
        try {
            auto batch = stream->nextAsync().get();
            out_queue->push(batch);
            if (batch) scheduleNext(pool, stream, out_queue);
        } catch (...) {
            out_queue->push_error(std::current_exception());
        }
    });
}
```

`QueueFlightDataStream::Next()` calls `queue_.popAsync().get()` — blocks only
for the µs until the next already-computed batch arrives.

---

## Part 8 — Full concurrency picture

```
SQL query arrives
      │
  QueryPlanner::plan()
      │  parse → logical → optimize → physical → annotateExecutionHints()
      │
  PlanScheduler::schedule(root)
      │
      │  DAG build: one PlanTask per non-CORRELATED_INNER node
      │  subquery dependencies → synthetic IO_LEAF children
      │  CPU_PIPELINE nodes → folded inline
      │
      │  t=0: submit ALL leaves simultaneously to BS_thread_pool
      │
  ┌───┴──────────────────────────────────────────────────────┐
  │ IO_LEAF A        IO_LEAF B        IO_LEAF C (subquery)   │
  │ (time_series)    (pv_metadata)    (IN subquery)          │
  │   nextAsync()      nextAsync()      nextAsync()          │
  │       │                │                │                │
  │   gRPC callback    gRPC callback    gRPC callback        │
  │   OnReadDone()     queryTable cb    queryPvMetadata cb   │
  │       │                │                │                │
  │   MLDP MongoDB     MLDP MongoDB     MLDP Annotation      │
  └───┬──────────────────────────────────────────────────────┘
      │
      │  A completes → stores result → --HashJoin.pending
      │  C completes → injects predicate into B scan → --B.pending → submit B
      │  B completes → --HashJoin.pending
      │  HashJoin.pending == 0 → submit HashJoin task (CPU, on pool worker)
      │
  HashJoin + fold Filter/Project inline → root_promise.set_value()
      │
  FutureRecordBatchStream resolves → BatchQueue feeds QueueFlightDataStream
      │
  Arrow Flight gRPC I/O thread → client
```

Properties:
- Zero threads blocked on MongoDB at any point
- All independent backend scans and subqueries overlap in time
- Join build + probe sides fetched concurrently
- CPU operators (filter, project, limit) never occupy a pool slot alone
- `mldp-pool.max-conn` remains the hard MongoDB concurrency ceiling
- 4 gRPC I/O threads serve 100+ concurrent Flight clients
- `BS_thread_pool` workers shared fairly across all concurrent queries

---

## Part 9 — Implementation order

Build bottom-up; each step is independently testable before the next.

1. `ExecutionHint` enum + `PhysicalNode::hint` field
2. `annotateExecutionHints` planner pass — wire into `QueryPlanner::plan()`
3. `BatchQueue` primitive
4. `IRecordBatchStream::nextAsync()` default wrapper
5. `AsyncMldpBidiRecordBatchStream` (§3.1) — validates async stream contract
6. `AsyncWideTableStream` single-shard (§3.2)
7. `AsyncShardedWideTableStream` multi-shard (§3.3) — kills `std::async`
8. `AsyncShardedPvStatsStream` (§3.4) — kills `std::async`
9. `AsyncAnnotationPageStream` (§3.5)
10. Update `ParallelSeriesRecordBatchStream` (§3.6)
11. `PlanTask` + `PlanScheduler` DAG build + leaf submission
12. `IO_LEAF` task body (BackendTableScan + synthetic subquery tasks)
13. `BLOCKING_AGGREGATE` task body (HashJoin, Sort, Pivot, NestedLoop)
14. Correlated NestedLoop inner-driven body
15. `FutureRecordBatchStream` wrapper
16. `QueryExecutor::executeStream()` → `PlanScheduler` when `async_backend=true`
17. `scheduleNext` loop + `QueueFlightDataStream`
18. `FlightSqlServerBase` impl + CMake wiring + subcommand

Steps 1–10: validate with existing integration tests (`context.async_backend=true`).  
Steps 11–16: validate with query planner/executor unit tests.  
Steps 17–18: validate with Flight client smoke tests.

---

## Part 10 — k8s deployment

Ticket = SQL text (stateless) → no session affinity.

```yaml
apiVersion: v1
kind: Service
metadata:
  name: mldp-sql-server
spec:
  selector:
    app: mldp-sql-server
  ports:
    - port: 47470
      targetPort: 47470
---
apiVersion: apps/v1
kind: Deployment
spec:
  replicas: 3
```

Set `mldp-pool.max-conn` per pod to
`ceil(target_concurrent_queries / replica_count)`.

---

## Verification

```bash
cmake -DMLDP_QUERY_SERVER_FLIGHT=ON ... && make -j$(nproc) mldp_pvxs_driver

./mldp_pvxs_driver sql-server --config driver.yaml --port 47470 --query-threads 16

# basic
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('SELECT * FROM mldp.pv_metadata LIMIT 5')
print(cur.fetchall())
"

# join + subquery (exercises full DAG scheduler)
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('''
  SELECT a.pv, b.description
  FROM mldp.time_series a
  JOIN mldp.pv_metadata b ON a.pv = b.pv
  WHERE a.pv IN (SELECT pv FROM mldp.pv_metadata WHERE tag = 'beam')
    AND a.time >= 1700000000 AND a.time <= 1700003600
''')
print(cur.fetchall())
"

# concurrency: 20 parallel clients
python3 -c "
import concurrent.futures, adbc_driver_flightsql.dbapi as flight
def query(_):
    conn = flight.connect('grpc://localhost:47470')
    cur = conn.cursor()
    cur.execute('SELECT * FROM mldp.pv_metadata LIMIT 100')
    return len(cur.fetchall())
with concurrent.futures.ThreadPoolExecutor(max_workers=20) as ex:
    print('rows per client:', list(ex.map(query, range(20))))
"

# bad SQL → INVALID_ARGUMENT
python3 -c "
import adbc_driver_flightsql.dbapi as flight
flight.connect('grpc://localhost:47470').cursor().execute('NOT VALID SQL')
" 2>&1 | grep -i invalid
```
