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

| Node type | Condition | Hint |
|---|---|---|
| `PhysicalTableScan` | `arrow_ipc=false`, no `derived_query` | `IO_LEAF` — backend gRPC |
| `PhysicalTableScan` | `arrow_ipc=true` | `IO_LEAF` — Arrow IPC file read (disk/NFS) |
| `PhysicalTableScan` | `derived_query != nullptr` | `IO_LEAF` — sub-DAG produces the scan input (see §4.2) |
| Synthetic `in_subqueries` leaf (see §4.2) | — | `IO_LEAF` |
| Synthetic `window_subquery` leaf (see §4.2) | — | `IO_LEAF` |
| Synthetic `derived_query` leaf (see §4.2) | — | `IO_LEAF` |
| `PhysicalCreateTable` | always | `IO_LEAF` — writes Arrow IPC file to disk/NFS |
| `PhysicalFilter` | — | `CPU_PIPELINE` |
| `PhysicalProject` | — | `CPU_PIPELINE` |
| `PhysicalLimit` | — | `CPU_PIPELINE` |
| `PhysicalSort` | — | `BLOCKING_AGGREGATE` |
| `PhysicalPivot` | — | `BLOCKING_AGGREGATE` |
| `PhysicalHashJoin` | — | `BLOCKING_AGGREGATE` |
| `PhysicalBlockNestedLoopJoin` | — | `BLOCKING_AGGREGATE` |
| `PhysicalNestedLoopJoin` | `correlated_push=false` | `BLOCKING_AGGREGATE` |
| `PhysicalNestedLoopJoin` inner | `correlated_push=true` | `CORRELATED_INNER` |
| `PhysicalShowTables/Functions/Operators` | — | `CPU_PIPELINE` — trivial catalog scan |
| `PhysicalDescribe` / `PhysicalExplain` | — | `CPU_PIPELINE` — in-memory text |
| `PhysicalDropTable` | — | `CPU_PIPELINE` — single metadata delete |

`CPU_PIPELINE` nodes carry no separate `PlanTask` — they execute inside their
nearest non-`CPU_PIPELINE` ancestor's task body.

`PhysicalCreateTable` is `IO_LEAF` not DDL because `QueryTableCatalog::create()`
opens an output stream on `arrow::fs::FileSystem` (blocking on NFS/S3) and
streams all child batches to disk before the atomic rename.  It must own a pool
task and must not start until its child SELECT sub-DAG completes.

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

Old `MldpBidiRecordBatchStream` retained for unit tests that construct a bare
`ExecutionContext` without `pool_threads`.  `MLDPQueryClient::executeStream`
selects impl based on `context.pool_threads != nullptr`.

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

### 4.2  DAG build: implicit dependency injection

Three kinds of implicit dependency hang off `PhysicalTableScan` and
`PhysicalCreateTable` nodes.  During DAG build each becomes a **synthetic
`IO_LEAF` child task** that is submitted immediately; the parent waits until
all synthetic children complete before it is submitted.

#### `in_subqueries` and `window_subquery` — predicate resolution

```
PhysicalTableScan (backend) [IO_LEAF, pending=2]
  SyntheticTask: IN subquery plan    [IO_LEAF, pending=0] ← submit now
  SyntheticTask: window subquery plan [IO_LEAF, pending=0] ← submit now
```

On completion: result values injected as resolved predicate into parent
`PhysicalTableScan.pushable_predicates` / `window_literal`.
`--parent->pending_children`; when 0 → submit parent backend scan.

#### `derived_query` — inline sub-SELECT

A `PhysicalTableScan` with `derived_query != nullptr` represents
`SELECT … FROM (SELECT …) AS alias`.  The inner SELECT is a full plan tree.
During DAG build it is scheduled as a **nested `PlanScheduler` call**:

```
PhysicalTableScan (derived) [IO_LEAF, pending=1]
  SyntheticTask: DerivedQuerySubDAG [IO_LEAF, pending=0] ← schedule sub-DAG now
```

`DerivedQuerySubDAG` task body:
```cpp
// Runs on pool worker — recursively schedules inner plan, waits for promise.
auto sub_future = PlanScheduler(pool_, context_, stats_).schedule(derived_plan);
task->result = sub_future.get();   // releases thread while inner DAG runs
onTaskComplete(task);
```

On completion: `task->result` holds inner SELECT batches.  Parent scan task
receives them as its input scan source (replaces `fetchBackendPages`) and
applies any remaining predicates / qualify inline.

#### `PhysicalCreateTable` — child SELECT must finish first

```
PhysicalCreateTable [IO_LEAF, pending=1]
  SyntheticTask: source SELECT sub-DAG [IO_LEAF, pending=0] ← schedule now
```

`CreateTable` task body (runs after child completes):
```cpp
// child->result holds all source batches — write to Arrow IPC file.
context_.table_catalog->create(create_.table_name, lifetime, child->result);
// atomic rename .partial → final path happens inside create()
// no output batches — DDL
onTaskComplete(task);   // signals root_promise with empty RecordBatches
```

File write (`OpenOutputStream` + `IPC writer` + `Move`) is the I/O work that
justifies the pool task — on NFS/S3 this can be hundreds of milliseconds.

#### Summary table

| Synthetic child type | Parent node | What it does on completion |
|---|---|---|
| `IN` subquery | `PhysicalTableScan` | injects resolved IN predicate values |
| `window_subquery` | `PhysicalTableScan` | injects resolved window `[begin_ns, end_ns]` |
| `derived_query` sub-DAG | `PhysicalTableScan` | delivers inner SELECT batches as scan input |
| source SELECT sub-DAG | `PhysicalCreateTable` | delivers batches for IPC file write |

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

**`IO_LEAF` — backend gRPC scan (`arrow_ipc=false`, no `derived_query`):**
```cpp
// All in_subqueries + window_subquery already resolved into scan node by
// synthetic child tasks before this task was submitted.
auto stream = makeAsyncBackendStream(scan, context_); // AsyncMldpBidiRecordBatchStream etc.
while (true) {
    auto batch = stream->nextAsync().get();  // free thread during gRPC I/O
    if (!batch) break;
    task->result.push_back(batch);
}
task->result = applyCpuPipeline(node, task->result, context_); // fold Filter/Project/Limit
onTaskComplete(task);
```

**`IO_LEAF` — Arrow IPC catalog scan (`arrow_ipc=true`):**
```cpp
// QueryTableCatalog::read() opens file on arrow::fs — may block on NFS/S3.
// Runs on pool worker; thread held for file I/O duration (typically <50ms local).
task->result = readCatalogTable(scan, context_);
task->result = applyCpuPipeline(node, task->result, context_);
onTaskComplete(task);
```

**`IO_LEAF` — derived-query scan (`derived_query != nullptr`, via synthetic child):**
```cpp
// SyntheticDerivedQueryTask runs first (see §4.2); its result is already in
// synthetic_child->result when parent scan task is submitted.
task->result = synthetic_child->result;
applyPredicates(task->result, scan.pushable_predicates, context_);
qualify(task->result, scan);
task->result = applyCpuPipeline(node, task->result, context_);
onTaskComplete(task);
```

**`IO_LEAF` — `PhysicalCreateTable` (via synthetic child):**
```cpp
// Source SELECT sub-DAG already complete; batches in synthetic_child->result.
if (!context_.table_catalog) throw std::runtime_error("CREATE TABLE has no catalog");
const auto status = context_.table_catalog->create(
    create.table_name,
    create.temporary ? TableLifetime::Session : TableLifetime::Persistent,
    synthetic_child->result);   // streams batches to IPC file, atomic rename
if (!status.ok()) throw std::runtime_error(status.ToString());
// DDL — no output batches
onTaskComplete(task);  // signals root_promise({})
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

### 4.5  `IExecutionState` kept for unit tests only

`IExecutionState::execute()` is unchanged and used only by low-level executor
unit tests that need synchronous, deterministic execution without a thread pool.
All production callers — CLI and Flight server — go through `PlanScheduler`.

`QueryExecutor::execute()` (the fully-materializing variant) is also updated:
it builds an `ExecutionContext` with `pool_threads` set and delegates to
`executeStream()`, draining the resulting stream to collect all batches.  The
separate `makeExecutionState` path is retired from production use.

---

## Part 5 — `ExecutionContext` changes + `QueryExecutor` integration

### 5.1  `ExecutionContext` additions

Add two fields to `include/query/ExecutionContext.h`:

```cpp
struct ExecutionContext
{
    // ... existing fields unchanged ...

    /// Thread pool for DAG scheduler and async backend tasks.
    /// Must be non-null for executeStream(); shared across concurrent queries.
    BS::light_thread_pool* pool_threads{nullptr};
};
```

`async_backend` is NOT a flag — the async engine is unconditional once
`pool_threads` is set.  The synchronous legacy path is retained only when
`pool_threads == nullptr` (unit tests that construct a bare `ExecutionContext`).

### 5.2  CLI context wiring (`QueryCommand.cpp`)

`QueryCommandPreparer::prepare()` (`src/query/QueryCommand.cpp` line 1217)
already owns long-lived resources.  Add one `BS::light_thread_pool` member:

```cpp
class QueryCommandPreparer {
    // ... existing ...
    mutable BS::light_thread_pool query_pool_{
        static_cast<BS::concurrency_t>(std::thread::hardware_concurrency())};
};
```

In the context-building block (around line 1337):

```cpp
context.pool_threads = &preparer.query_pool_;
```

Single pool shared across all queries in the session — fair round-robin
scheduling across concurrent interactive queries.  Pool size defaults to
`cpu_count`; overridable via `--query-threads` (same flag as sql-server).

### 5.3  `QueryExecutor::executeStream()`

`src/query/QueryExecutor.cpp` line 220 becomes:

```cpp
QueryStreamExecutionResult QueryExecutor::executeStream(
    const plan::PhysicalNodePtr& root, ExecutionContext context) const
{
    auto stats = std::make_shared<QueryStats>();
    collectPlanWarnings(root, stats->plan_warnings);
    stats->plan_summary = plan::physicalPlanToString(root);

    if (!context.pool_threads) {
        // Unit-test / bare-context fallback: synchronous legacy path.
        return legacyExecuteStream(root, std::move(context), stats);
    }

    auto sched  = std::make_shared<PlanScheduler>(*context.pool_threads, context, *stats);
    auto future = sched->schedule(root);

    auto stream = std::make_unique<FutureRecordBatchStream>(std::move(future));
    return QueryStreamExecutionResult{
        .stream = std::make_unique<FinalizingRecordBatchStream>(
                      std::move(stream), std::move(context), stats,
                      std::chrono::steady_clock::now()),
        .stats  = std::move(stats)};
}
```

`FutureRecordBatchStream` wraps `future<RecordBatches>`, yielding batches one
at a time from the resolved vector.  `nextAsync()` returns immediately if the
future is already resolved.

### 5.4  CLI stream consumption — no change needed

`QueryCommand.cpp` line 1404 already drains `stream->next()` in a loop.
`FutureRecordBatchStream::next()` blocks until the DAG future resolves on the
first call, then yields pre-computed batches one by one — same observable
behaviour as before, zero code change at the call site.

The interactive pagination path (`InteractivePageStream`, `continuations`)
wraps the stream after `executeStream()` returns — unchanged, works over any
`IRecordBatchStream` including `FutureRecordBatchStream`.

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
- `context.pool_threads = &query_pool_` (owned by `QueryFlightSqlServer`)
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
  ┌────────────────────────────────────────────────────────────────────────┐
  │ IO_LEAF A           IO_LEAF B        IO_LEAF C         IO_LEAF D       │
  │ (time_series gRPC)  (pv_metadata)    (IN subquery)     (IPC file read) │
  │   nextAsync()         nextAsync()      nextAsync()       readCatalog()  │
  │       │                   │                │                 │          │
  │   gRPC callback       gRPC callback    gRPC callback     arrow::fs      │
  │   OnReadDone()        queryTable cb    queryPvMeta cb    OpenInput()    │
  │       │                   │                │                 │          │
  │   MLDP MongoDB        MLDP MongoDB     MLDP Annotation   local/NFS     │
  └───┬────────────────────────────────────────────────────────────────────┘
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
- Zero threads blocked on MongoDB or local file I/O at any point
- All independent leaves overlap in time: backend scans, subqueries, IPC reads, derived-query sub-DAGs
- Join build + probe sides fetched concurrently
- `CREATE TABLE` write starts the moment its source SELECT sub-DAG completes
- `derived_query` inner SELECT runs as a full nested DAG — inherits all parallelism
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
12. `IO_LEAF` task body — backend gRPC scan (calls async streams from Part 3)
13. `IO_LEAF` task body — Arrow IPC catalog scan (`readCatalogTable` on pool worker)
14. `IO_LEAF` task body — `derived_query` synthetic child (nested `PlanScheduler` call)
15. `IO_LEAF` task body — `PhysicalCreateTable` (IPC file write after source sub-DAG)
16. `BLOCKING_AGGREGATE` task body (HashJoin, Sort, Pivot, NestedLoop)
17. Correlated NestedLoop inner-driven body
18. `FutureRecordBatchStream` wrapper
19. `ExecutionContext::pool_threads` field + `QueryExecutor::executeStream()` DAG dispatch
20. `QueryCommandPreparer` — add `BS::light_thread_pool` member, wire `context.pool_threads`
21. `scheduleNext` loop + `QueueFlightDataStream`
22. `FlightSqlServerBase` impl + CMake wiring + subcommand

Steps 1–10: validate with existing integration tests (bare `ExecutionContext` still uses sync path).  
Steps 11–20: validate with query planner/executor unit tests including:
  - `SELECT * FROM (SELECT ...) AS t` — derived query DAG
  - `CREATE TABLE t AS SELECT ...` followed by `SELECT * FROM t` — IPC write + read
  - `SELECT ... WHERE pv IN (SELECT pv FROM ...)` — subquery injection
Steps 21–22: validate with Flight client smoke tests.

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

# join + subquery (backend gRPC leaves + IN subquery synthetic leaf, all concurrent)
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

# CREATE TABLE then SELECT (IPC write IO_LEAF + IPC read IO_LEAF)
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('CREATE TEMP TABLE beam_pvs AS SELECT pv FROM mldp.pv_metadata WHERE tag = \'beam\'')
cur.execute('SELECT * FROM beam_pvs LIMIT 10')
print(cur.fetchall())
"

# derived query (nested sub-DAG)
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('SELECT pv FROM (SELECT pv, description FROM mldp.pv_metadata WHERE tag = \'beam\') AS t LIMIT 5')
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
