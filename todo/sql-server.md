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

## Concurrency model: async producer-consumer

gRPC I/O threads are **never blocked on MongoDB**. Each `DoGet` immediately offloads query
execution to an internal worker pool, then streams completed batches back to the client as
they arrive. This allows a small gRPC thread pool (4) to serve many concurrent clients.

```
Client N  →  gRPC I/O thread (pool ~4)
                  │  reads from per-query bounded channel (2-4 slots)
             [BatchQueue<RecordBatch>]
                  ↑  pushes batches as they complete
             Query worker thread (BS_thread_pool, default = cpu_count)
                  │
             QueryExecutor::executeStream() → MLDPGrpcQueryPool → MongoDB
```

**Flow per `DoGet`:**
1. gRPC thread receives call, submits SQL to `BS_thread_pool` with a shared `BatchQueue`
2. Worker runs `IRecordBatchStream::next()` in a loop, pushes each batch into the queue
3. Worker pushes a sentinel (`nullptr`) on EOF or exception
4. gRPC thread loops: `batch = queue.pop()` → `writer->Write(*batch)` → repeat until sentinel
5. Bounded queue (capacity = 2-4) provides backpressure: slow client pauses the worker,
   freeing the worker slot for other queries

**Result:**
- 4 gRPC threads serve 100+ concurrent clients
- N worker threads multiplex all active queries (excess queries queue for a free worker)
- No gRPC thread ever blocks on MongoDB or Arrow evaluation
- `mldp-pool.max-conn` in driver config remains the ultimate concurrency ceiling
  (set `max-conn = ceil(target_concurrent_queries / replica_count)`)

`BS_thread_pool` already compiled into `lib${PROJECT_NAME}` via
`ext/BS_thread_pool/include/BS_thread_pool.hpp` — no extra dep needed.

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
- Create `BatchQueue` (bounded, capacity 3)
- Submit to `BS_thread_pool`:
  ```cpp
  pool_.detach_task([sql, queue]() {
      auto stream = QueryExecutor{}.executeStream(...);
      while (auto batch = stream->next())
          queue->push(batch);
      queue->push(nullptr);  // EOF sentinel
  });
  ```
- Return custom `arrow::flight::FlightDataStream` that calls `queue->pop()` in `Next()`
- On exception in worker: push error token to queue, `Next()` returns gRPC `INTERNAL`

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
