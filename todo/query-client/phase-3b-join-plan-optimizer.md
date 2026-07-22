# Phase 3b — Join Plan Types + Optimizer Passes

← [Back to main plan](query-client-impl.md)

Extends [Phase 3](phase-3-planner-executor.md).

## AST Join Nodes

- [ ] Extend `QueryAST.h` with `JoinClause` (type, table_ref, alias, `ON` condition), `QualifiedColumn` (alias + name)
- [ ] Extend `QueryParser` to parse `[INNER | LEFT [OUTER]] JOIN … ON …`; multi-join chains produce a list of `JoinClause`
- [ ] Extend `Binder` to: resolve both sides of each join; validate `ON` columns exist in respective table schemas; auto-qualify unambiguous unqualified column refs; emit `BindError` on ambiguous names

## Logical Join Node

- [ ] Add `LogicalJoin` to `LogicalPlan.h`: `{JoinType, JoinCondition, left: LogicalNodePtr, right: LogicalNodePtr, predicates}`
- [ ] `LogicalPlanner` builds left-deep join tree for multi-join chains

## Physical Join Nodes

- [ ] Add `PhysicalHashJoin`, `PhysicalNestedLoopJoin`, `PhysicalBlockNestedLoopJoin` to `PhysicalPlan.h`
- [ ] Add `JoinType`, `JoinCondition`, `JoinAlgorithm` enums/structs to `PhysicalPlan.h`

## Optimizer Passes

- [ ] `src/cli/query/planner/JoinOrderOptimizer.h/.cpp` — classify each side bounded/unbounded via predicate heuristic; assign build/probe; emit `PlanWarning` on unbounded×unbounded
- [ ] `src/cli/query/planner/CorrelatedPushOptimizer.h/.cpp` — detect pushable join key on probe side; promote `PhysicalHashJoin` → `PhysicalNestedLoopJoin` with correlated predicate injection; respect `--join-batch-size`

## Executor Join Execution (Arrow-native)

- [ ] `PhysicalHashJoin` executor: accumulate build-side `RecordBatch` into partitioned hash map; when `pool->bytes_allocated() > memory_limit_bytes` → spill partition via `SpillManager`; probe side streamed; matched rows assembled with `arrow::compute::Take`; `LEFT JOIN` null-padding via `arrow::MakeArrayOfNull`
- [ ] Grace-hash spill: load each spill partition via `SpillReader` (memory-mapped IPC), build in-memory hash, probe all probe rows, `SpillReader` destructor deletes file
- [ ] `PhysicalNestedLoopJoin` executor: batch outer rows by `batch_size`; build `IN(values)` predicate from join key column; call inner `execute()` per batch; match within batch
- [ ] `PhysicalBlockNestedLoopJoin` executor: spill entire outer side; multi-pass probe; emits `PlanWarning`
- [ ] `NULL` / nullable columns: `arrow::field(..., nullable=true)` on right side of `LEFT JOIN`; `PhysicalFilter` calls `arrow::compute::IsNull` for three-valued logic
- [ ] Join output schema: qualified column names (`alias.column`) via `arrow::Field::WithName`

## New CLI Flags

- [ ] `--memory-mb N` (default 256) — `ExecutionContext::memory_limit_bytes`
- [ ] `--spill-dir PATH` (default system temp) — `SpillManager` root under `arrow::fs::LocalFileSystem`
- [ ] `--spill-partitions N` (default 16) — grace-hash partitions for large joins
- [ ] `--join-batch-size N` (default 100) — outer rows per inner RPC in correlated nested-loop join
- [ ] Add all flags to `QuerySubcommand` arg parse; pack into `ExecutionContext`

## Notes

- All joins are client-side — no server-side join execution possible across separate gRPC backends
- `NESTED_LOOP` chosen only when `CorrelatedPushOptimizer` can push join key as `IN(...)` to inner backend — not the default
- `BLOCK_NESTED_LOOP` is a fallback with a `PlanWarning` — both sides unbounded
- Join optimizer passes run after `PredicatePushdown`, before `RequiredColumnCheck`
