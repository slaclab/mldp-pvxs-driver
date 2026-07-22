# Phase 3 — Planner (multi-pass) + Executor

← [Back to main plan](query-client-impl.md)

## Logical / Physical Plan Types

- [ ] `include/query/plan/LogicalPlan.h` — `LogicalScan`, `LogicalFilter`, `LogicalProject`, `LogicalLimit` variant + `LogicalNodePtr`
- [ ] `include/query/plan/PhysicalPlan.h` — `PhysicalTableScan`, `PhysicalFilter`, `PhysicalProject`, `PhysicalLimit` variant + `PhysicalNodePtr`
- [ ] `include/query/plan/PlannerError.h` — `BindError`, `TypeError`, `PlanError` variant + formatted `what()` helpers

## Planner Passes

- [ ] `src/cli/query/planner/Binder.h/.cpp` — resolves table→`IQueryable`+schema; validates column names/operators; expands `attr.<key>`; validates `SELECT` columns; checks `required` columns present; emits `BindError`
- [ ] `src/cli/query/planner/TypeChecker.h/.cpp` — locks `NOW` to single epoch; folds `NOW±duration`; validates literal types against `ColumnType`; emits `TypeError`
- [ ] `src/cli/query/planner/LogicalPlanner.h/.cpp` — AST → `LogicalScan → LogicalFilter → LogicalProject → LogicalLimit` tree
- [ ] `src/cli/query/planner/PredicatePushdown.h/.cpp` — classifies predicates into pushable vs post-filter using `ColumnSchema::pushable_ops`; merges multi-value same-column predicates where possible
- [ ] `src/cli/query/planner/ConstantFolding.h/.cpp` — dedup identical predicates; catch `LIMIT 0`; fold remaining literal expressions
- [ ] `src/cli/query/planner/ColumnPruning.h/.cpp` — computes referenced column set; attaches to `PhysicalTableScan` as projection hint
- [ ] `src/cli/query/planner/RequiredColumnCheck.h/.cpp` — post-pushdown: verify `required=true` columns have pushable predicates; emits `PlanError`
- [ ] `src/cli/query/planner/PhysicalPlanner.h/.cpp` — walks optimised logical tree → `PhysicalPlan`
- [ ] `src/cli/query/QueryPlanner.h/.cpp` — orchestrates all passes; single `plan(AST) → PhysicalPlan` entry point; handles `SHOW TABLES` / `DESCRIBE` / `EXPLAIN` as plan-time short-circuits

## Executor

- [ ] `include/query/QueryStats.h` — `QueryStats` struct (elapsed, rows, rpc_calls, bytes_spilled, spill_files, peak_memory_bytes, plan_summary)
- [ ] `src/cli/query/QueryExecutor.h/.cpp` — walks `PhysicalPlan` recursively producing `arrow::RecordBatch` streams; `PhysicalFilter` uses `arrow::compute::Filter`; `PhysicalProject` uses `arrow::compute::Project`; `PhysicalLimit` truncates batches; all nodes receive `ExecutionContext`; `EXPLAIN` short-circuits to plan dump

## Notes

- Pass order: Binder → TypeChecker → LogicalPlanner → PredicatePushdown → ConstantFolding → ColumnPruning → RequiredColumnCheck → PhysicalPlanner
- Each pass is a separate class, independently testable
- `NOW` locked once at plan time — deterministic across predicate pairs in the same query
- Join plan types and join optimizer passes are in [Phase 3b](phase-3b-join-plan-optimizer.md)
