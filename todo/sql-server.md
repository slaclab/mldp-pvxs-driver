# Arrow Flight SQL Server

Expose the SQL query engine as an Arrow Flight SQL server so external tools
(Python ADBC, Java JDBC, ODBC, DBeaver, Grafana) can query MLDP data over
standard gRPC without needing a CLI session.

## Client ecosystem

| Client | Protocol |
|--------|----------|
| Python `adbc-driver-flightsql` | Flight SQL native |
| Java `arrow-flight-sql-jdbc-driver` | JDBC over Flight SQL |
| Dremio ODBC driver | ODBC over Flight SQL |
| DBeaver | JDBC plugin |
| `pyarrow.flight` | Flight low-level |
| curl/grpcurl | raw gRPC |

## New CMake option

```cmake
option(MLDP_QUERY_SERVER_FLIGHT "Build Arrow Flight SQL query server" OFF)
```

When ON: enables `ARROW_WITH_GRPC`, `ARROW_FLIGHT`, `ARROW_FLIGHT_SQL` (same flags as
`MLDP_QUERY_CLIENT_FLIGHT`). Update the condition at CMakeLists.txt line ~210:
```cmake
if(MLDP_QUERY_CLIENT_FLIGHT OR MLDP_QUERY_SERVER_FLIGHT)
```
Add `arrow_flight_static` + `arrow_flight_sql_static` to main executable link.

## New subcommand

```
mldp_pvxs_driver sql-server [--config <yaml>] [--port 47470] [--bind 0.0.0.0]
                             [--query-threads N]   # default = cpu_count
                             [--grpc-threads N]    # default = 4
```

Dispatch from `mldp_pvxs_driver_main.cpp` (same early-subcommand pattern as `query`, lines 315-342).
Guard with `#ifdef MLDP_QUERY_SERVER_FLIGHT`.

## Uniform cursor contract (prerequisite)

**The central design requirement:** `IRecordBatchStream::next()` must always mean
"one bounded unit of forward progress" regardless of query shape. The caller
(sql-server worker loop) never special-cases by query type.

| Query shape | One `next()` unit |
|---|---|
| `mldp.time_series` bidi | one MongoDB bucket response |
| `mldp.time_series_table` wide pivot | one window slice pivoted |
| subquery resolution phase | one subquery evaluated |
| JOIN / filter / project | one batch from upstream |

Currently `mldp.time_series_table` violates this: `executeStream()` falls back to full
materialization (all slices + spill file written) before the first `next()` returns.

**Required fix (separate task, see `todo/lazy-cursor-contract.md`):**
Extend `makeStreamingPlan` to return a true lazy stream for `mldp.time_series_table` where
each `next()` = fetch+pivot one window slice. Once this is done the cooperative resubmission
worker loop works uniformly for all query shapes.

**Until that fix lands:** `mldp.time_series_table` queries still block one worker thread
for the full query duration. The sql-server still works correctly — just with reduced
parallelism for that query shape.

## Concurrency model: cooperative resubmission

gRPC I/O threads are **never blocked on MongoDB**. Query work runs on `BS_thread_pool`
(already in codebase). After each `next()` call the worker **resubmits a continuation**
back to the pool — it holds a thread only for one unit of work, then yields. All concurrent
queries share the pool fairly.

```
Client N  →  gRPC I/O thread (pool ~4)
                  │  reads from per-query bounded BatchQueue (capacity 3)
             [BatchQueue<RecordBatch>]
                  ↑  one batch pushed per pool task
             BS_thread_pool worker (detach_task, resubmits after each next())
                  │
             IRecordBatchStream::next()  ← one bounded unit of work
                  │
             MLDPGrpcQueryPool → MongoDB
```

**Worker loop (cooperative resubmission):**
```cpp
void scheduleNext(BS::thread_pool& pool,
                  std::shared_ptr<IRecordBatchStream> stream,
                  std::shared_ptr<BatchQueue> queue)
{
    pool.detach_task([&pool, stream, queue]() {
        try {
            auto batch = stream->next();   // one unit of work, then releases thread
            queue->push(batch);            // nullptr = EOF sentinel
            if (batch)
                scheduleNext(pool, stream, queue);  // resubmit continuation
        } catch (...) {
            queue->push_error(std::current_exception());
        }
    });
}
```

**`DoGet` entry point:**
```cpp
// 1. create bounded queue
auto queue = std::make_shared<BatchQueue>(/*capacity=*/3);
// 2. build stream (fast — no MongoDB I/O)
auto stream = QueryExecutor{}.executeStream(sql, context);
// 3. schedule first unit of work
scheduleNext(pool_, stream, queue);
// 4. return FlightDataStream that pops from queue
return std::make_unique<QueueFlightDataStream>(queue);
```

**Properties:**
- 4 gRPC threads serve 100+ concurrent clients
- N worker threads round-robin across all active queries (one batch per pool task)
- No query monopolizes the pool — slow queries yield after each slice
- Bounded queue (cap=3) provides backpressure: slow client pauses worker resubmission
- `mldp-pool.max-conn` remains the hard concurrency ceiling for MongoDB connections

`BS_thread_pool` already compiled into `lib${PROJECT_NAME}` via
`ext/BS_thread_pool/include/BS_thread_pool.hpp` — no extra dep needed.

## Future: full async

The cooperative resubmission structure is the natural precursor to full async.
Once an async MongoDB C++ driver or C++20 coroutine support is available,
`scheduleNext` becomes `co_await stream->nextAsync()` — the continuation structure
is already in place, only the blocking call changes.

## New files

### `include/cli/QueryFlightSqlServer.h`
```cpp
#ifdef MLDP_QUERY_SERVER_FLIGHT
class QueryFlightSqlServer {
public:
    struct Options {
        uint16_t    port{47470};
        std::string bind{"0.0.0.0"};
        uint32_t    grpc_threads{4};
        uint32_t    query_threads{0};  // 0 = cpu_count
    };
    QueryFlightSqlServer(const config::Config& config, Options opts);
    void serve();  // blocks until stop()
    void stop();
private:
    std::unique_ptr<arrow::flight::FlightServerBase> impl_;
};
#endif
```

### `src/cli/QueryFlightSqlServer.cpp`
Implements `arrow::flight::sql::FlightSqlServerBase`:

**`GetFlightInfo(CommandStatementQuery)`**
- Parse SQL, plan it (validates syntax/schema eagerly)
- On `ParseError` / `PlannerException` → return gRPC `INVALID_ARGUMENT`
- Ticket = SQL text (UTF-8 bytes) — stateless, any replica can redeem it
- Return `FlightInfo` with one endpoint pointing to this server

**`DoGet(ticket)`**
- Deserialize ticket → SQL
- Build stream via `QueryExecutor::executeStream()`
- Kick off cooperative resubmission loop (see above)
- Return `QueueFlightDataStream` backed by bounded `BatchQueue`
- Exceptions in worker propagate via `BatchQueue::push_error()` → gRPC `INTERNAL`

**`DoPutCommandStatementUpdate`** (optional)
- Forward DDL (CREATE TABLE / DROP TABLE) to `QueryRunner::run()`

### `src/cli/sql_server_subcommand.cpp`
- Parses `--port`, `--bind`, `--config`, `--query-threads`, `--grpc-threads`
- Calls `QueryCommandPreparer::prepare(config)`
- Calls `QueryFlightSqlServer::serve()`

## Key reuse (no changes needed to these)

| Symbol | File |
|--------|------|
| `QueryCommandPreparer::prepare()` | `include/query/QueryCommand.h` |
| `QueryExecutor::executeStream()` | `include/query/QueryExecutor.h` |
| `IRecordBatchStream` | `include/query/IQueryable.h` |
| `BS_thread_pool` | `ext/BS_thread_pool/include/BS_thread_pool.hpp` |

## k8s deployment

Ticket = SQL text (stateless) → no session affinity needed. Scale freely:

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
  # no sessionAffinity — tickets are stateless SQL bytes
---
apiVersion: apps/v1
kind: Deployment
spec:
  replicas: 3  # scale horizontally for more query throughput
```

Each replica independently connects to MLDP backend. Set `mldp-pool.max-conn` per pod to
`ceil(target_concurrent_queries / replica_count)`.

## Verification

```bash
# build
cmake -DMLDP_QUERY_SERVER_FLIGHT=ON ... && make -j$(nproc) mldp_pvxs_driver

# start
./mldp_pvxs_driver sql-server --config driver.yaml --port 47470 --query-threads 16

# Python ADBC
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('SELECT * FROM mldp.pv_metadata LIMIT 5')
print(cur.fetchall())
"

# pyarrow direct
python3 -c "
import pyarrow.flight as fl
client = fl.connect('grpc://localhost:47470')
info = client.get_flight_info(fl.FlightDescriptor.for_command(b'SELECT 1'))
reader = client.do_get(info.endpoints[0].ticket)
print(reader.read_all())
"

# concurrency test: 20 parallel clients
python3 -c "
import concurrent.futures, adbc_driver_flightsql.dbapi as flight
def query(_):
    conn = flight.connect('grpc://localhost:47470')
    cur = conn.cursor()
    cur.execute('SELECT * FROM mldp.pv_metadata LIMIT 100')
    return len(cur.fetchall())
with concurrent.futures.ThreadPoolExecutor(max_workers=20) as ex:
    results = list(ex.map(query, range(20)))
print('rows per client:', results)
"

# error handling: bad SQL → gRPC INVALID_ARGUMENT
python3 -c "
import adbc_driver_flightsql.dbapi as flight
conn = flight.connect('grpc://localhost:47470')
cur = conn.cursor()
cur.execute('NOT VALID SQL')
" 2>&1 | grep -i invalid
```
