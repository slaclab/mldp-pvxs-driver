# Phase 7 — Arrow Flight SQL Server (future)

← [Back to main plan](query-client-impl.md)

## Scope

Future phase. Not part of initial delivery. Current phases must preserve compatibility for this phase.

## Core Invariant

`QueryExecutor::execute()` returns Arrow `RecordBatch` streams (not string rows), so the same execution pipeline can be streamed over Flight SQL without planner/executor redesign.

## Goal

Expose existing query planner/executor over Arrow Flight SQL gRPC APIs so external clients (DBeaver, pandas/ADBC, JDBC/ODBC Flight drivers) can query virtual tables remotely.

## Tasks

- [ ] `src/cli/query/FlightSQLServer.h/.cpp` — `arrow::flight::sql::FlightSqlServerBase` subclass
- [ ] `GetFlightInfo(CommandStatementQuery)` → parse + plan → `FlightInfo` with schema + ticket
- [ ] `DoGet(Ticket)` → `QueryExecutor::execute()` → stream `RecordBatch` to client via `FlightDataStream`
- [ ] `GetTables()` → `QueryableFactory::registeredTables()`
- [ ] `GetSchema(CommandStatementQuery)` → planner Binder pass only → return Arrow schema
- [ ] Wire `--serve-flight HOST:PORT` in `mldp_pvxs_driver_main.cpp`

## API Mapping

- `SHOW TABLES` semantic equivalent maps to Flight `GetTables()`.
- `DESCRIBE <table>` semantic equivalent maps to Flight `GetSchema()`.
- SQL statement execution maps to `GetFlightInfo` + `DoGet`.

## Runtime and Build Requirements

- `arrow_flight` stays optional in CMake; default CLI-only binary remains lean.
- Planner, optimizer, spill manager, and queryable backends are reused unchanged.
- Error messages from parse/planner layers should map to Flight status responses with clear causes.

## Architecture

```
Client (DBeaver, pandas, ADBC, etc.)
    │  Arrow Flight SQL protocol (gRPC + IPC)
    ▼
FlightSQLServer
    ├─ GetFlightInfo → parse + plan → FlightInfo
    ├─ DoGet         → execute PhysicalPlan → stream RecordBatches
    └─ GetSchema     → EXPLAIN/schema path
         │
         ▼ (same path as CLI query mode)
    QueryPlanner → PhysicalPlan → QueryExecutor → SpillManager → IQueryable → gRPC backends
```

## Notes

- This phase is additive only; it does not change query semantics from CLI mode.
- Phase 0 optional Flight linkage and Phase 5 output plumbing should already be aligned for this extension.
