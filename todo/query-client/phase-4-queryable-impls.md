# Phase 4 — Queryable Implementations

← [Back to main plan](query-client-impl.md)

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

## Notes

- Unknown column → `execute()` throws `std::invalid_argument` listing valid columns from `tableSchema()`
- Predicate→backend mapping is owned entirely by each implementation — planner has no column knowledge
- `execute()` switches on `table_name`; each case translates predicates to proto criteria structs
