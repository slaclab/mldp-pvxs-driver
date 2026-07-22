# Phase 1 — `IQueryable` Contract + Arrow Foundation

← [Back to main plan](query-client-impl.md)

## Tasks

- [ ] `include/query/ArrowTypeMap.h` — `ColumnType → std::shared_ptr<arrow::DataType>` mapping
- [ ] `include/query/QueryResult.h` — `QueryResult { shared_ptr<RecordBatch>, next_page_token }`
- [ ] `include/query/ExecutionContext.h` — `ExecutionContext { MemoryPool, SpillManager, limits }`
- [ ] Update `IQueryable.h`: `ColumnSchema` with `pushable_ops`/`filterable_ops`; `execute()` signature takes `pushable_predicates`, `projection_hint`, `ExecutionContext`
- [ ] `include/query/SpillManager.h` + `src/cli/query/SpillManager.cpp` — `arrow::fs::FileSystem`-backed; `spill()`, `read()`, `cleanup()`; uses `arrow::ipc::MakeFileWriter` / `OpenFile`
- [ ] Extend `QueryableFactory::prepare<T>()` to register `T::kVirtualTables`; add `createByTable()` and `registeredTables()`
- [ ] Add `query` subcommand dispatch in `mldp_pvxs_driver_main.cpp`
- [ ] `QuerySubcommand` builds `ExecutionContext` (Arrow memory pool + `LocalFileSystem` SpillManager); calls `QueryableFactory::prepare<T>(config)` only — no `MLDPPVXSController`

## Notes

- `ColumnType → arrow::DataType` mapping lives in `ArrowTypeMap.h` — used by both `IQueryable` implementations and the executor
- `SpillManager` takes an injected `arrow::fs::FileSystem`; tests inject `MockFileSystem`
- `QuerySubcommand` reads only `queryable:` config subtree — no readers/writers/routing keys required
