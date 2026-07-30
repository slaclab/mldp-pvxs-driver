# Phase 11 — Unified Pull-Based Physical Executor

← [Back to main plan](query-client-impl.md)

## Goal

Replace the query engine's parallel materialized and streaming execution paths
with one physical-operator framework. Every physical plan node is represented
by a query-local pull operator that returns one `arrow::RecordBatch` per
`next()` call and returns `nullptr` at clean end of stream.

This phase retains Apache Arrow as the query engine's columnar representation.
It does not introduce an engine-neutral batch wrapper and does not use
`std::function` as the operator-execution abstraction. Stateful operators own
their child operators with `std::unique_ptr`, which makes resource lifetime,
cancellation, spill ownership, and backpressure explicit.

## Current state and problem

Phase 10 establishes a native MLDP bidi source stream, formatter backpressure,
and a physical wide pivot. The executor still has two runtime paths:

- `IExecutionState::execute()` recursively returns
  `std::vector<std::shared_ptr<arrow::RecordBatch>>` for the general physical
  plan tree.
- `QueryExecutor.cpp` independently recognizes a limited streamable subset
  (direct long scan, filter, project, limit, and pivot) and builds
  `IRecordBatchStream` wrappers. Other plans execute to completion and are
  wrapped in a materialized stream.

The duplicate plan dispatch makes streaming eligibility implicit, duplicates
scan and operator behavior, and makes each new physical operator require
changes in two execution frameworks.

## Scope

- Introduce one `IPhysicalOperator` pull contract and one exhaustive physical
  plan-to-operator factory.
- Make `QueryExecutor::executeStream()` the canonical execution entrypoint.
- Keep `QueryExecutor::execute()` as a compatibility collector that drains the
  same stream into a batch vector.
- Convert all current scan, relational, pivot, and statement execution states
  to pull operators.
- Preserve SQL grammar, logical and physical planning, `IQueryable`, backend
  protocols, CLI formats, query stats, cancellation, spill behavior, and
  current error semantics.
- Document whether each physical operator is pipelined or blocking on its
  first pull.

## Non-goals

- Do not change SQL syntax, backend gRPC/protobuf contracts, or the
  `IQueryable::executeStream()` provider contract.
- Do not promise that sort, joins, pivot, or table creation produce output
  before all required input is consumed. They use the common pull contract but
  may prepare/materialize or spill on their first pull.
- Do not replace Arrow with a project-defined batch type.
- Do not add dynamic third-party physical-operator plugins. The physical plan
  is a closed `std::variant`, so exhaustive compile-time dispatch remains the
  intended extension mechanism.
- Do not redesign join algorithms or alter their query semantics as part of
  this refactor. Streaming improvements after semantic parity are a later,
  separately benchmarked change.

## Target execution model

```text
QueryExecutor::executeStream(PhysicalPlan, ExecutionContext)
  |
  +-- makePhysicalOperator(PhysicalNodeVariant, context, shared stats)
        |
        +-- Backend / catalog / derived / window scan operators
        +-- Filter / project / limit operators             [pipelined]
        +-- Sort / joins / pivot operators                 [blocking as needed]
        +-- SHOW / DESCRIBE / EXPLAIN / CREATE / DROP      [statement]
        |
        +-- next() -> one Arrow RecordBatch, or nullptr at EOF
```

The canonical runtime interface is:

```cpp
class IPhysicalOperator
{
public:
    virtual ~IPhysicalOperator() = default;

    virtual std::shared_ptr<arrow::RecordBatch> next() = 0;
    virtual std::string_view typeName() const noexcept = 0;
};
```

Each operator owns zero or more `std::unique_ptr<IPhysicalOperator>` children.
A common base provides read-only `ExecutionContext`, shared `QueryStats`, and
the cancellation check. `QueryExecutor` supplies the finalizing wrapper that
updates returned-row, elapsed-time, progress, and peak-memory statistics at
EOF.

`IQueryable` remains the source boundary. Native providers return an
`IRecordBatchStream`; providers without a native cursor continue using the
existing continuation-token adapter. `IRecordBatchStream` and
`IPhysicalOperator` have the same pull/EOF convention but represent different
layers: provider transport versus physical SQL execution.

## Work plan

### 1. Establish the runtime pull contract

1. In `include/query/executor/ExecutionState.h` and the matching implementation,
   replace or rename `IExecutionState` to `IPhysicalOperator` and replace
   `RecordBatches execute()` with `next()`.
2. Adapt `ExecutionStateBase` in `include/query/executor/StateInternal.h` into
   an internal operator base that owns child pull operators and exposes
   `context()`, `stats()`, `throwIfCancelled()`, and checked child access.
3. Rename the construction entrypoint to `makePhysicalOperator(...)`. It must
   reject a null root and reject every unsupported physical node with the
   existing actionable error behavior.
4. Keep `RecordBatches` only as a local helper type for blocking algorithms;
   it must no longer be the execution interface.

### 2. Make the executor use one entrypoint

1. Retain `QueryExecutor::executeStream()` as the public canonical entrypoint.
2. Construct the root operator through `makePhysicalOperator()` and wrap it in
   the existing finalization/stats stream adapter, or rename that adapter to
   make its operator role clear.
3. Reimplement `QueryExecutor::execute()` by draining `executeStream()` into
   `QueryExecutionResult::batches`. It must not construct a second execution
   tree or invoke an operator-specific materialization path.
4. Delete `makeStreamingPlan()` and `MaterializedRecordBatchStream` after all
   factory cases use the new operator tree.

### 3. Convert pipelined scan and relational operators

Convert these operators before blocking operators. Each must pull only enough
input to produce its next output batch:

- `PhysicalTableScan`: direct MLDP cursor stream, continuation-paged provider,
  window-sharded stream, catalog Arrow IPC scan, derived-query scan, and
  `IN (SELECT ...)` prerequisite handling.
- `PhysicalFilter`: skip empty post-filter batches and preserve Arrow filter
  errors and local predicate semantics.
- `PhysicalProject`: preserve ordinary and expression projection semantics,
  generated names, native union handling, and typed nulls.
- `PhysicalLimit`: retain `remaining` rows and stop pulling upstream as soon as
  the limit is satisfied.

For scans with subquery prerequisites, consume and cache only the prerequisite
values required to form predicates or normalized windows before opening the
main source. A no-match membership or window prerequisite must return clean
EOF without opening an unnecessary backend query.

### 4. Convert blocking relational and pivot operators

Every operator still presents `next()`, even when it prepares once on first
pull:

```text
first next(): consume required child streams; materialize or spill as needed
later next(): return one prepared output batch
EOF:          return nullptr
```

Convert and preserve the current semantics for:

- `PhysicalSort`: materialize/spill, order with the existing Arrow path, then
  emit output batches in order.
- `PhysicalHashJoin`: preserve right-side build, probe behavior, join warnings,
  outer-join null rows, and spill accounting.
- `PhysicalNestedLoopJoin` and `PhysicalBlockNestedLoopJoin`: retain their
  distinct planner-selected algorithms and bounded behavior.
- `PhysicalPivot`: retain spill-backed long-form ingestion, external sorting,
  duplicate-cell validation, null cells, requested PV order, cancellation, and
  incrementally emitted wide batches.

This phase may initially adapt a blocking operator by consuming child streams
into a local vector during preparation. Do not merge all results into one giant
batch merely to satisfy the interface.

### 5. Convert statements and table catalog operations

Convert `SHOW TABLES`, `SHOW FUNCTIONS`, `SHOW OPERATORS`, `DESCRIBE`,
`EXPLAIN`, `CREATE [TEMP] TABLE AS SELECT`, and `DROP TABLE` to the same
operator contract:

- one-result statements return their single synthetic Arrow batch once;
- effect-only statements run once and return EOF;
- `CREATE TABLE` drains its child pull operator directly into
  `QueryTableCatalog::create()` and publishes the catalog table only after
  complete successful consumption.

### 6. Centralize physical-plan dispatch and remove duplication

1. Replace `makeExecutionState()`, `makeRelationalExecutionState()`,
   `makeStatementExecutionState()`, and the `makeStreamingPlan()` switch with
   one exhaustive physical-node factory, retaining small private helpers only
   for source organization.
2. Use explicit `std::get_if` dispatch for the closed
   `PhysicalNodeVariant`. Do not introduce a `std::function` registry unless a
   future requirement allows runtime plugin-defined plan node types.
3. Place each operator in the existing scan, relational, and statement source
   families where that keeps the diff understandable; rename files/classes
   from `*ExecutionState` to `*Operator` consistently as they are migrated.
4. Update `CMakeLists.txt` for renamed source files and remove obsolete files
   only after every factory reference and test is migrated.

### 7. Preserve lifecycle, cancellation, and observability

- Every `next()` checks cancellation before backend work and within long
  materialization, sorting, merging, and spill loops.
- Backend cursor streams, pooled handles, spill readers/writers, and temporary
  catalog resources are owned by the operator that uses them and release on
  normal EOF, exception, cancellation, and abandoned root-stream destruction.
- Retain `QueryStats` meanings: backend response rows, returned rows, RPCs,
  spill bytes/files, materialized catalog bytes/files, warnings, plan summary,
  peak Arrow-pool memory, and elapsed time.
- Preserve `QueryProgressTracker` updates around backend pulls and blocking
  stages without adding terminal control output to non-interactive formats.

### 8. Documentation and completion cleanup

1. Update `docs/reference/query-engine-architecture.md` to describe one
   pull-based physical-operator tree, including the pipelined/blocking table.
2. Update its extension guidance: a new physical node needs physical lowering,
   one factory case, one operator, tests, and documentation—not two executor
   implementations.
3. Update `todo/query-client/query-client-impl.md` with Phase 11 and replace
   its historical runtime-state description after implementation is complete.
4. Search `src/query`, headers, docs, and tests for `IExecutionState`,
   `makeStreamingPlan`, and `MaterializedRecordBatchStream`; remove obsolete
   references before marking the phase complete.

## Required tests and acceptance

Add focused coverage before deleting the old framework.

### Operator contract tests

- Every physical operator handles first pull, multiple batches, empty input,
  repeated EOF, and destruction before EOF correctly.
- Filter, project, and limit prove that a second upstream batch is not pulled
  before the consumer requests another output batch.
- Limit proves it does not request additional backend data after satisfying the
  row limit.
- Scan variants cover direct stream, continuation adapter, catalog, derived
  source, `IN` prerequisite, and literal/subquery windows.

### Semantic-parity tests

Before removing the compatibility collector, compare a drained
`executeStream()` result with `execute()` for each currently supported plan.
Assert identical Arrow schemas/values, warnings, and final statistics for
filters, projections, limits, sort, all join forms, pivot, scans, catalog
operations, and result-producing statements.

### Lifecycle and spill tests

- Preserve Phase 10 bidi cursor-next ordering, terminal `Finish()` failures,
  cancellation, and stream-destruction cleanup tests.
- Cover cancellation during scan, sort, joins, pivot ingestion/sort/merge, and
  table creation.
- Verify temporary spill/catalog files are released on EOF, failure,
  cancellation, and abandoned stream destruction.
- Verify statements execute once, and `CREATE TABLE` does not publish partial
  results on error or cancellation.

### Devcontainer verification

Use the Linux devcontainer as the authoritative build/test environment:

```sh
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml \
  exec devcontainer bash -lc "cmake --build /workspace/build --target mldp_pvxs_driver_test --parallel && ctest --test-dir /workspace/build -R '(MLDPQueryClientTest|QueryParserTest|QueryPlannerExecutorTest|QueryRunnerTest|QueryContinuationRegistryTest|QueryableMldpIntegrationTest|QueryFormatterTest)' --output-on-failure"
git diff --check
```

Phase 11 is complete only when this focused suite passes in the devcontainer,
the plan-to-operator factory is the only physical execution dispatcher, all
public execution enters through the pull path, and the architecture document
no longer describes materialized and streaming executor frameworks as
coexisting implementations.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| A "pull" interface hides full materialization and overpromises streaming. | Document every operator as pipelined or blocking; add backpressure tests for the pipelined subset. |
| Join or spill semantics change during a structural refactor. | Convert one existing algorithm at a time and use parity tests before behavior/performance improvements. |
| Large all-at-once rename obscures regressions. | Land in buildable stages: interface/collector, pipelined operators, source variants, blocking operators, statements, cleanup/docs. |
| Resource leaks from abandoned streams. | Make operator ownership explicit with `unique_ptr` and retain Phase 10 cancellation/destruction coverage. |

