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

## Data Shape Requirements

- All responses are converted to typed Arrow columns through the shared `ColumnType -> ArrowType` mapping.
- `next_page_token` is passed through where backend paging applies.
- Join-capable column names must remain stable and consistent across table schemas (`pv`, `name`, etc.).

## Notes

- `mldp.active_configurations` requires an `at` predicate (`=`) as pushable input.
- `queryProviders` / `queryProviderStats` are proto-level future work and remain out of this phase.
