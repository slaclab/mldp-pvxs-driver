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

### Current state — fully blocking, no streaming

`execute()` has **no `page_token` input**. It drains the entire gRPC stream, builds one giant `RecordBatch` in RAM, then returns. User blocked until ALL rows arrive. SpillManager is NOT auto-triggered — it has zero automatic threshold logic and is only called explicitly by `QueryExecutor` for JOIN hash-table intermediates. So large time-series = user blocked + RAM explosion + no disk flush.

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
