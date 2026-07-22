# Phase 6 — Tests

← [Back to main plan](query-client-impl.md)

## Unit Tests

### Parser
- [ ] Lexer — all token types, edge cases (empty input, unterminated strings, unknown chars)
- [ ] Parser — valid `SELECT`, `SHOW TABLES`, `DESCRIBE`, `EXPLAIN`; malformed SQL → `ParseError` with position
- [ ] Parser — `INNER JOIN`, `LEFT JOIN`, multi-join chains, `ON` equi-condition

### Planner Passes
- [ ] Binder — unknown table, unknown column, unsupported op, missing required column → correct `BindError`
- [ ] Binder — ambiguous column → `BindError`; unambiguous auto-qualify; unknown table in JOIN
- [ ] TypeChecker — `NOW` resolution, `NOW±duration` folding, type mismatch → `TypeError`
- [ ] LogicalPlanner — AST → correct `LogicalPlan` tree shape
- [ ] PredicatePushdown — pushable predicates land in `PhysicalTableScan`, non-pushable in `PhysicalFilter`
- [ ] ConstantFolding — dedup predicates, `LIMIT 0` rejected
- [ ] ColumnPruning — projection hint contains only referenced columns
- [ ] RequiredColumnCheck — missing required column post-pushdown → `PlanError`
- [ ] Full planner pipeline end-to-end per table (each virtual table × representative query shapes)

### Join Optimizer
- [ ] `JoinOrderOptimizer` — bounded+bounded picks smaller `IN` list as build; unbounded+unbounded emits `PlanWarning`
- [ ] `CorrelatedPushOptimizer` — hash join promoted to nested-loop when join key is pushable on inner side; non-pushable key stays hash join

### Executor
- [ ] `QueryExecutor` `PhysicalFilter` — in-memory row filtering (no backend)
- [ ] `PhysicalHashJoin` — INNER: only matched rows; LEFT: unmatched left rows get NULL right columns
- [ ] `PhysicalNestedLoopJoin` — batching: outer rows split into `batch_size` chunks; inner scan called once per chunk; correct row pairing
- [ ] `NULL` three-valued logic in `PhysicalFilter` — `NULL = 'x'` → false; `NULL != 'x'` → false
- [ ] Qualified column names in `QueryResult` output (`alias.column`)
- [ ] `EXPLAIN` output format — plan tree text matches expected shape
- [ ] `EXPLAIN` join plan output — algorithm name, build/probe labels, correlated column annotation
- [ ] `QueryStats` populated correctly (elapsed, row counts)

### SpillManager
- [ ] `SpillManager` with `arrow::fs::MockFileSystem` — spill + read round-trip; `cleanup()` deletes all files; `SpillReader` destructor deletes file
- [ ] `SpillManager` Arrow IPC round-trip — schema preserved; all Arrow types survive
- [ ] `PhysicalHashJoin` spill path — build side triggers spill at memory limit; grace-hash partitions load/match correctly; zero rows lost vs in-memory reference
- [ ] `arrow::MemoryPool` tracking — `peak_memory_bytes` in `QueryStats` matches pool `max_memory()`

### Queryable Implementations
- [ ] `MLDPQueryClient::execute()` predicate→backend mapping (mock pool, no network)
- [ ] `MLDPAnnotationQueryClient::execute()` predicate→proto criteria mapping (mock pool, no network)

### Output Formatter
- [ ] JSON and CSV formatters produce valid output for all `ColumnType` variants

## Integration Tests

- [ ] Real backend calls via both query clients
- [ ] Two-table `INNER JOIN` (`mldp.pv_stats` × `mldp.pv_metadata`) — correct row pairing on `pv`
- [ ] `LEFT JOIN` — PVs with no metadata record appear with NULL right columns
- [ ] Correlated push join — verify single batched RPC per outer block (mock pool RPC counter)
