# Phase 4 — Queryable Implementations

← [Back to main plan](query-client-impl.md)

## Goal

Implement table ownership, schema exposure, and predicate-to-backend translation inside `MLDPQueryClient` and `MLDPAnnotationQueryClient`, returning Arrow `RecordBatch` results through the unified `IQueryable` contract.

## Ownership Boundary

- Planner validates syntax/types/ops and passes pushable predicates.
- Each `IQueryable` implementation owns:
  - virtual table declarations (`kVirtualTables`)
  - `tableSchema(table_name)`
  - `execute(table_name, pushable_predicates, projection_hint, ctx)` translation details
- No central cross-table predicate map is introduced in planner/executor.

## Tasks

- [ ] `MLDPQueryClient`: add `kVirtualTables`, `tableSchema()`, `execute()` — predicate→`querySourcesData` / `querySourcesInfo` translation

  | Table | Predicate column | Backend param |
  |---|---|---|
  | `mldp.time_series` | `pv` (`IN`, `=`) | `source_names` set |
  | `mldp.time_series` | `time` (`>=`) | `lookback_window = now - value` |
  | `mldp.time_series` | `time` (`<=`) | `forward_window = value - now` |
  | `mldp.time_series` | `timeout` (`=`) | `QuerySourcesDataOptions::timeout` |
  | `mldp.time_series` | `rpc_deadline` (`=`) | `QuerySourcesDataOptions::rpc_deadline` |
  | `mldp.pv_stats` | `pv` (`IN`, `=`) | `source_names` set |

- [ ] `MLDPAnnotationQueryClient`: add `kVirtualTables`, `tableSchema()`, `execute()` — predicate→proto criteria translation

  | Table | Predicate column | Operator(s) | Backend param |
  |---|---|---|---|
  | `mldp.pv_metadata` | `pv` | `=`, `PREFIX`, `CONTAINS`, `IN` | `PvNameCriterion` |
  | `mldp.pv_metadata` | `alias` | `=`, `PREFIX`, `CONTAINS`, `IN` | `AliasesCriterion` |
  | `mldp.pv_metadata` | `tag` | `=`, `IN` | `TagsCriterion` |
  | `mldp.pv_metadata` | `attr.<key>` | `=`, `IN` | `AttributesCriterion` |
  | `mldp.configuration` | `name` | `=`, `PREFIX`, `CONTAINS`, `IN` | `NameCriterion` |
  | `mldp.configuration` | `category` | `=`, `IN` | `CategoryCriterion` |
  | `mldp.configuration` | `tag` | `=`, `IN` | `TagsCriterion` |
  | `mldp.configuration` | `attr.<key>` | `=`, `IN` | `AttributesCriterion` |
  | `mldp.configuration` | `parent` | `=`, `IN` | `ParentCriterion` |
  | `mldp.configuration_activation` | `time` | `=` | `TimestampCriterion` |
  | `mldp.configuration_activation` | `time` | `>=`, `<=` | `TimeRangeCriterion` |
  | `mldp.configuration_activation` | `config_name` | `=`, `IN` | `ConfigurationNameCriterion` |
  | `mldp.configuration_activation` | `activation_id` | `=`, `IN` | `ClientActivationIdCriterion` |
  | `mldp.configuration_activation` | `category` | `=`, `IN` | `CategoryCriterion` |
  | `mldp.configuration_activation` | `tag` | `=`, `IN` | `TagsCriterion` |
  | `mldp.configuration_activation` | `attr.<key>` | `=`, `IN` | `AttributesCriterion` |
  | `mldp.active_configurations` | `at` | `=` | `GetActiveConfigurationsRequest::timestamp` (required) |

## Schema and Error Requirements

- `tableSchema()` must expose required columns, output columns, and supported pushable/filterable ops.
- Unknown table name in `tableSchema()` or `execute()` must throw with the supported table list.
- Unknown/unsupported column predicate in `execute()` throws `std::invalid_argument` with valid columns.
- `projection_hint` is accepted and may be ignored if backend cannot project natively; correctness still holds.

## Streaming / Paging Model

### Implemented executor pagination coverage

`test/query/query_planner_executor_test.cpp` includes
`PlannerExecutorTest.AccumulatesBackendPagesAndTracksEveryRpc`.  Its
`FakeQueryable` exposes `fake.paged`, returns `A` with the continuation token
`second-page` on the first call, then returns `B` with an empty token.  The
test executes `SELECT pv FROM fake.paged` through the production planner and
executor and asserts:

- two returned Arrow batches, in `A`, then `B` order;
- `QueryStats::rpc_calls == 2`;
- `QueryStats::rows_from_backend == 2`; and
- `QueryStats::rows_returned == 2`.

This validates empty-token start, token forwarding, multi-page accumulation,
and empty-token termination at the executor boundary.  It is focused unit
coverage; service-backed paging and predicate translation still require their
dedicated mock and CI integration tests.

## Real MLDP Service Integration Suite

`test/query/queryable_mldp_integration_test.cpp` is the dedicated
service-backed suite. It executes only in the existing `docker-compose-test.yml`
stack, which provides MongoDB, `dp-ingestion`, `dp-query`, and `dp-annotation`.
Every assertion uses the production SQL path:

```text
parseQuery() -> QueryPlanner::plan() -> QueryExecutor::execute()
```

### CMake and CTest registration

- `queryable_mldp_integration_test` is registered beside
  `query_planner_executor_test` in `CMakeLists.txt`.
- It links like the existing query tests: `gtest_main` plus the complete driver
  archive, with `include/`, `test/`, and the binary directory on its include
  path.
- Test discovery uses
  `gtest_discover_tests(queryable_mldp_integration_test PROPERTIES LABELS "integration;query")`.
- Run the focused suite in the CI stack with:

  ```bash
  ctest --test-dir build -L 'integration;query' --output-on-failure
  ```

### Implemented seed and cleanup behavior

- Each test creates a namespace from a nanosecond timestamp plus atomic suffix.
  Every PV, metadata record, configuration name, and activation ID uses that
  namespace. SQL predicates are exact-name or `IN` predicates; the suite never
  queries broad archive state.
- Tests seed deterministic integer time-series samples through `MLDPWriter`
  (`mldp`) to `dp-ingestion:50051`. The `DataBatch` column name and the source
  PV are identical, which validates the expected time-series identity mapping.
- Tests seed PV metadata through `MLDPPVMetadataWriter`
  (`mldp-pv-metadata`) and configurations plus activations through
  `MLDPConfigurationWriter` (`mldp-configuration`) at `dp-annotation:50053`.
  Each writer is stopped after its seed phase, so its queue drains before SQL
  polling begins.
- `pollSql()` has a fixed 30-second deadline and 250-ms retry interval. It
  executes production SQL on every attempt and includes both the namespace and
  missing record type in timeout failures.
- Fixture teardown deletes annotation records in dependency order:
  configuration activations, configurations, then PV metadata. Archived
  samples remain under their unique namespace because the archive has no
  deterministic delete API suitable for the suite.

### SQL scenarios covered against real MLDP services

- The fixture prepares real `MLDPQueryClient` and
  `MLDPAnnotationQueryClient` instances through `QueryableFactory`, using
  `dp-query:50052` and `dp-annotation:50053` (with the matching ingestion
  endpoint present in the pool config).

| SQL scenario | Real-service coverage |
|---|---|
| `mldp.time_series` | Seeds five integer samples, queries one exact PV with `join_batch_size = 1`, and verifies all five rows across backend pages. Assertions cover `pv`, UTC nanosecond `time`, dense-union `value`, integer union type ID `5`, values `100..104`, timestamp ordering, and `rpc_calls > 1`. |
| `mldp.pv_stats` | Seeds two independently named PVs, queries them with `pv IN (...)`, and verifies every requested row plus nonzero bucket counts. This exercises the supported two-value `IN` predicate contract. |
| `mldp.pv_metadata` | Seeds two exact-name metadata records, queries `pv IN (...)`, and verifies both namespace-scoped rows. |
| `mldp.configuration` | Seeds two configurations, queries `name IN (...)`, and verifies both rows arrive through SQL. |
| `mldp.configuration_activation` | Seeds two adjacent, closed activation windows in one category, queries `activation_id IN (...)`, and verifies both rows arrive through SQL. |
| `mldp.active_configurations` | Seeds a separate-category, open-ended activation, then queries `WHERE at = NOW` and waits until its unique activation ID is returned. |
| Metadata/time-series join | Executes `mldp.time_series ts INNER JOIN mldp.pv_metadata meta ON ts.pv = meta.pv` with an exact time-series PV predicate; asserts exactly the three seeded samples and no unrelated rows. |
| Activation/configuration join | Executes `mldp.configuration_activation activation INNER JOIN mldp.configuration configuration ON activation.config_name = configuration.name`; asserts the seeded configuration name and category. |

The real-service suite proves SQL parsing, planning, queryable predicate
translation, Arrow conversion, and joins together. Time-series continuation
is exercised through the production stack; executor continuation-token
forwarding remains covered deterministically by the existing fake
`PlannerExecutorTest.AccumulatesBackendPagesAndTracksEveryRpc` unit test.

### Acceptance and CI proof

- The test is intended to pass independently under the `integration;query`
  label and as part of the existing full
  `docker compose -f docker-compose-test.yml up --build ci` run.
- The test does not use direct MongoDB writes or custom service stubs for its
  assertions; project writers are the only seed path and the production parser,
  planner, executor, and queryables are the only query path.

### Previous state — fully blocking, no streaming

Before the interface change, `execute()` had no `page_token` input. It drained the entire gRPC stream, built one giant `RecordBatch` in RAM, then returned. SpillManager is not auto-triggered; it is only called explicitly by `QueryExecutor` for JOIN hash-table intermediates.

### Required interface change — add `page_token` input

`IQueryable::execute()` must accept an opaque resumption cursor:

```cpp
// IQueryable.h — add page_token parameter
virtual QueryResult execute(std::string_view table_name,
                            const std::vector<Predicate>& pushable_predicates,
                            const std::set<std::string>& projection_hint,
                            const ExecutionContext& context,
                            std::string_view page_token = {}) = 0;
```

`QueryExecutor` must be updated to loop: call `execute()` with empty token first, then with `result.next_page_token` until token is empty.

### Streaming execution model

Each `execute()` call:

1. If `page_token` is empty → open gRPC stream, start consuming rows.
2. If `page_token` is non-empty → resume previously opened stream (keyed by token).
3. Consume rows until `context.join_batch_size` rows accumulated OR stream exhausted.
4. Return `{batch, next_page_token}` immediately — do NOT wait for stream end.
5. `next_page_token` empty = last page, stream closed.

Each impl holds internal stream state (open gRPC `ClientReader` or async reader) keyed by an opaque token string. Token format is impl-defined (e.g. UUID → stored reader map, or base64-encoded proto cursor).

### Batch-size cap

`context.join_batch_size` doubles as page size for ALL queries (not just JOINs):

- `context.join_batch_size > 0` → cap rows per call at that value; applies to single-table scans and JOINs equally.
- `context.join_batch_size == 0` → no cap; accumulate full result (backward-compat for small queries or tests).

Single-table queries benefit the same as JOINs: first batch returns after `join_batch_size` rows, gRPC stream still flowing. SpillManager kicks in on top only when `QueryExecutor` accumulates multi-page batches for JOIN probe-build phases.

### SpillManager — NOT called by `execute()` impls

`execute()` impls do NOT call `SpillManager`. Each batch is already bounded by cap → fits in RAM. SpillManager is called by `QueryExecutor` when it accumulates multi-page batches for JOIN operations. The IQueryable layer has no spill responsibility.

### User-visible latency

With cap active, first batch returns as soon as `join_batch_size` rows arrive from gRPC. User sees first results without waiting for full dataset. Subsequent pages arrive on demand via `QueryExecutor` loop.

## Data Shape Requirements

- All responses are converted to typed Arrow columns through the shared `ColumnType -> ArrowType` mapping.
- `next_page_token` is passed through where backend paging applies; empty string signals last page.
- Token must be opaque to caller — impl serialises whatever cursor the backend needs.
- Join-capable column names must remain stable and consistent across table schemas (`pv`, `name`, etc.).

## Notes

- `mldp.active_configurations` requires an `at` predicate (`=`) as pushable input.
- `queryProviders` / `queryProviderStats` are proto-level future work and remain out of this phase.
