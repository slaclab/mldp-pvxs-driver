# Phase 3 — Planner (multi-pass) + Executor

← [Back to main plan](query-client-impl.md)

## Goal

Turn parser output into an optimized physical plan and execute it as Arrow `RecordBatch` streams with deterministic planning and explicit error surfaces.

## Phase Inputs and Outputs

- **Input:** `QueryAST` from Phase 2.
- **Output:** `PhysicalPlan` plus execution result batches and `QueryStats`.
- **Invariant:** executor has no planner logic; all optimization decisions happen in planner passes.

## Pipeline Contract (fixed order)

1. Binder
2. TypeChecker
3. LogicalPlanner
4. PredicatePushdown
5. ConstantFolding
6. ColumnPruning
7. RequiredColumnCheck
8. PhysicalPlanner

Join-specific plan nodes and optimizers are extended in [Phase 3b](phase-3b-join-plan-optimizer.md), but this ordering stays the backbone.

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

## Required Behaviors

- Binder resolves table names via `QueryableFactory`, supports aliases, auto-qualifies unambiguous columns, and rejects ambiguous unqualified references.
- TypeChecker resolves `NOW` once per query and reuses that epoch for deterministic comparisons.
- Predicate pushdown uses `ColumnSchema::pushable_ops`; post-filter uses `ColumnSchema::filterable_ops`.
- Required column validation runs *after* pushdown to ensure required predicates are actually backend-pushable.
- `PhysicalTableScan` calls `IQueryable::execute(table_name, pushable_predicates, projection_hint, ctx)`.

## EXPLAIN and Introspection Behavior

- `EXPLAIN` runs full planning and prints physical tree instead of executing.
- `SHOW TABLES` and `DESCRIBE` short-circuit through planner-time metadata paths.

## QueryStats Contract

Stats payload fields:

- elapsed
- rows from backend (before post-filter)
- rows returned
- rpc_calls
- bytes_spilled
- spill_files
- peak_memory_bytes (`arrow::MemoryPool::max_memory()`)
- plan_summary

`--no-stats` suppresses printing only.

## Notes

- Each pass is independently testable and should keep side effects local.
- Errors remain structured (`BindError`, `TypeError`, `PlanError`) and actionable.
