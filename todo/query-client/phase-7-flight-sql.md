# Phase 7 — Arrow Flight SQL Server (future)

← [Back to main plan](query-client-impl.md)

**Not in initial scope.** Design must not preclude it. Key invariant: `QueryExecutor::execute()` always returns `arrow::RecordBatch` streams, never `vector<string>` rows.

## Tasks

- [ ] `src/cli/query/FlightSQLServer.h/.cpp` — `arrow::flight::sql::FlightSqlServerBase` subclass
- [ ] `GetFlightInfo(CommandStatementQuery)` → parse + plan → `FlightInfo` with schema + ticket
- [ ] `DoGet(Ticket)` → `QueryExecutor::execute()` → stream `RecordBatch` to client via `FlightDataStream`
- [ ] `GetTables()` → `QueryableFactory::registeredTables()`
- [ ] `GetSchema(CommandStatementQuery)` → planner Binder pass only → return Arrow schema
- [ ] Wire `--serve-flight HOST:PORT` in `mldp_pvxs_driver_main.cpp`

## Architecture

```
Client (DBeaver, pandas, ADBC, etc.)
    │  Arrow Flight SQL protocol (gRPC + IPC)
    ▼
FlightSQLServer
    ├─ GetFlightInfo → parse + plan → FlightInfo
    ├─ DoGet         → execute PhysicalPlan → stream RecordBatches
    └─ GetSchema     → EXPLAIN → schema only
         │
         ▼ (same path as CLI query mode)
    QueryPlanner → PhysicalPlan → QueryExecutor → SpillManager → IQueryable → gRPC backends
```

## Notes

- `FlightSQLServer` wraps `QueryExecutor` directly — no architectural change to planner/executor/spill
- `arrow_flight` CMake target linked optionally so default binary stays lean (Phase 0 prerequisite)
- Compatible clients: DBeaver, `pandas` + `adbc_driver_flight_sql`, JDBC/ODBC Flight drivers
- `SHOW TABLES` maps to `GetTables()` Flight SQL call
- `DESCRIBE` maps to `GetSchema()` Flight SQL call
