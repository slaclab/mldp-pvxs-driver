# Query Engine Architecture

This document describes the embedded query engine used by `mldp_pvxs_driver query` from a developer perspective.

> **Related:** [Query CLI Guide](../guides/query-cli.md) | [Query Clients](../dev/query-client.md) | [Architecture Overview](architecture.md)

---

## End-to-End Flow

1. CLI parses query runtime options in `src/cli/mldp_pvxs_driver_main.cpp`.
2. `prepareQuerySubcommand(...)` prepares `QueryableFactory` from the `queryable:` config.
3. `runQueryRepl(...)` reads one line at a time from `stdin`.
4. Each line is parsed (`parseQuery`), planned (`QueryPlanner::plan`), then executed (`QueryExecutor::execute`).

Core files:

- `src/query/QuerySubcommand.cpp`
- `src/query/QueryPlanner.cpp`
- `src/query/QueryExecutor.cpp`
- `include/query/plan/LogicalPlan.h`
- `include/query/plan/PhysicalPlan.h`

---

## Planner Pipeline

`QueryPlanner::plan(...)` runs a fixed pass order for `SELECT` statements:

1. `bindSelect(...)`
2. `typeCheckSelect(...)`
3. `buildLogicalPlan(...)`
4. `applyPredicatePushdown(...)`
5. `applyJoinOrderOptimizer(...)`
6. `applyConstantFolding(...)`
7. `applyColumnPruning(...)`
8. `requiredColumnCheck(...)`
9. `buildPhysicalPlan(...)`
10. `applyCorrelatedPushOptimizer(...)`

Non-`SELECT` statements map directly to physical nodes:

- `SHOW TABLES` → `PhysicalShowTables`
- `DESCRIBE <table>` → `PhysicalDescribe`
- `EXPLAIN <query>` → `PhysicalExplain`

---

## Logical and Physical Plans

Logical nodes (`include/query/plan/LogicalPlan.h`):

- `LogicalScan`
- `LogicalFilter`
- `LogicalProject`
- `LogicalLimit`
- `LogicalJoin`

Physical nodes (`include/query/plan/PhysicalPlan.h`):

- `PhysicalTableScan`
- `PhysicalFilter`
- `PhysicalProject`
- `PhysicalLimit`
- `PhysicalHashJoin`
- `PhysicalNestedLoopJoin`
- `PhysicalBlockNestedLoopJoin`
- `PhysicalShowTables`
- `PhysicalDescribe`
- `PhysicalExplain`

---

## Join Planning and Optimization

### Join order optimization

`applyJoinOrderOptimizer(...)` annotates boundedness and may reorder **inner joins** to put a bounded side first.  
If both sides are unbounded, it emits a warning:

`PlanWarning: joining two unbounded sides; spill is expected under memory pressure`

### Correlated push optimization

`applyCorrelatedPushOptimizer(...)` rewrites eligible hash-join shapes into nested-loop joins with `correlated_push=true` when the right side is a direct `PhysicalTableScan`.

### Required-column safety

`requiredColumnCheck(...)` enforces required schema columns must be constrained by a pushable predicate or covered by an equi-join key, preventing unconstrained scans on required dimensions.

---

## Execution Model

`QueryExecutor::execute(...)` recursively evaluates the physical tree:

- scans call `QueryableFactory::createByTable(...)->execute(...)`
- filters and projects apply in-memory Arrow transforms
- joins combine child batches and execute through shared join helpers
- SHOW/DESCRIBE/EXPLAIN materialize synthetic Arrow `RecordBatch` outputs

Execution returns:

- output batches
- `QueryStats` (`include/query/QueryStats.h`) with:
  - elapsed time
  - backend rows
  - returned rows
  - RPC calls
  - spill bytes/files
  - peak memory bytes
  - plan summary
  - plan warnings

---

## Memory and Spill

Runtime controls are passed via `QueryCliOptions` → `ExecutionContext`:

- `memory_mb` → `memory_limit_bytes`
- `spill_dir`
- `spill_partitions`
- `join_batch_size`

`SpillManager` is attached in `runQueryRepl(...)` and used by join execution under memory pressure (notably on build-side materialization paths). Spill activity is reflected in `QueryStats.bytes_spilled` and `QueryStats.spill_files`.

---

## Extension Points

- Add/extend parser grammar in `src/query/parser/grammar/*`.
- Add planner passes under `src/query/planner/*` and wire in `QueryPlanner.cpp`.
- Add physical operators in `include/query/plan/PhysicalPlan.h` + `QueryExecutor.cpp`.
- Add backend table/query behavior by implementing/extending `IQueryable` providers and registering through `QueryableFactory`.
