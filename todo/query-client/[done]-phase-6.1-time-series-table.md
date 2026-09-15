# Phase 6.1 — Native MLDP Time-Series Table

← [Back to main plan](query-client-impl.md)

> **Status: complete.** Verified in the devcontainer on 2026-07-23 with 37
> focused parser, planner/executor, wide-table client, and integration tests
> passing (1.12 s).

## Goal

Add `mldp.time_series_table` alongside the existing long-form
`mldp.time_series`.  It retains MLDP's native `TABLE_FORMAT_COLUMN` response
as an Arrow table: one shared `time` column and one typed Arrow column for
each retained MLDP `DataColumn`.

```sql
SELECT *
FROM mldp.time_series_table
WHERE pv IN ('PV1', 'PV2')
  AND column_type = 'double'
  AND attributes.namespace = 'mldp_sample'
  AND time >= NOW - 1h
  AND time <= NOW;
```

This phase does not pivot long rows, resample, interpolate, forward-fill, or
join PV data.  It directly maps the MLDP column table returned by one
`queryTable` request.

## Query Contract

- `mldp.time_series` remains unchanged: it returns long rows with `pv`,
  `time`, and dense-union `value` columns.
- `mldp.time_series_table` requires `pv = ...` or `pv IN (...)`.  The
  requested PV names define the candidate runtime data columns and are sent
  in `QueryTableRequest.pvNameList`.
- `time` range predicates are pushed to `QueryTableRequest.beginTime` and
  `endTime`.
- `column_type`, `tag`, `attributes.<key>`, and `provenance.<key>` are normal
  locally evaluated predicates.  They select whole returned `DataColumn`s,
  rather than individual timestamp rows.
- `SELECT *` returns `time` followed by retained PV columns in requested-PV
  order.  Column metadata is not duplicated into every timestamp row.
- This is a special runtime-shaped table: it accepts `SELECT *` only.  It does
  not support joins, `ORDER BY`, explicit projections, or predicates on
  generated PV fields.  Use `column_type`, `tag`, `attributes.<key>`, and
  `provenance.<key>` to select whole PV columns.
- `pv IN (SELECT pv ...)` is supported only on this table. The child output is
  evaluated first and must contain exactly one non-null string `pv` field; its
  row order is the requested PV order.
- `window IN (SELECT time, end_time ...)` is supported only on this table.
  Each child row is a closed range. The executor sorts and coalesces overlapping
  or directly adjacent ranges, then emits one typed wide batch per normalized
  request window. Open/null, empty, malformed, and inverted ranges are errors.

## Column Types and Metadata

Expose `column_type` as a string queryable field for both time-series tables.
It represents the native MLDP data-value kind using stable SQL names, not a
generated protobuf spelling:

| MLDP value kind | `column_type` |
|---|---|
| double | `double` |
| float | `float` |
| signed/unsigned 32-bit integer | `int32` / `uint32` |
| signed/unsigned 64-bit integer | `int64` / `uint64` |
| boolean | `bool` |
| string | `string` |
| binary | `binary` |
| timestamp | `timestamp` |
| array, structure, image | `array`, `structure`, `image` |

Continue to expose `tags`, `attributes`, `attributes.<key>`, `provenance`, and
`provenance.<key>` from `DataColumn.columnMetadata`.  `column_type` and the
metadata predicates use the same local equality/list filtering model as the
existing time-series metadata fields.

Each retained `mldp.time_series_table` field has the matching native Arrow
type; the wide table must not use the long table's dense-union `value`
representation.  This phase deliberately does not attach MLDP column metadata
to Arrow field metadata.

## Implementation Tasks

- [x] Register `mldp.time_series_table` in `MLDPQueryClient`; preserve the
  current `mldp.time_series` behavior and schema.
- [x] Add `column_type` to the time-series schema, SQL documentation, and
  response conversion.  Add a single shared mapping from MLDP value kind to
  public SQL type name and Arrow type.
- [x] Reuse the existing `TABLE_FORMAT_COLUMN` request path.  Decode the
  shared `dataTimestamps` vector once and build the `time` Arrow column.
- [x] Evaluate type, tag, attribute, and provenance predicates once for each
  returned `DataColumn`.  Drop non-matching data columns before producing the
  table schema.
- [x] Build one Arrow array and field per retained `DataColumn`; preserve
  requested-PV order and validate unique returned PV names.
- [x] Represent a returned data column shorter than the shared timestamp
  vector with trailing Arrow nulls.  Reject a column longer than the timestamp
  vector as an invalid MLDP response.
- [x] Enforce the special-table SQL contract: `SELECT *` only; no joins,
  `ORDER BY`, explicit projections, or predicates over generated PV fields.
- [x] Fail clearly when a requested PV is absent from the MLDP response;
  include the PV name in the error.

## Tests

- [x] Extend the MLDP query-client mock with multiple `DataColumn`s sharing
  timestamps, with distinct native types, tags, attributes, and provenance.
- [x] Verify existing `mldp.time_series` output remains long-form and exposes
  / filters `column_type` correctly.
- [x] Verify `mldp.time_series_table` returns one row per shared timestamp,
  native typed fields named after PVs, stable requested-PV order, and Arrow
  fields without Arrow metadata payload.
- [x] Verify combined `pv`, time, `column_type`, tag, attribute, and
  provenance filters retain only the matching PV columns.
- [x] Verify the `SELECT *` contract and reject explicit projections and
  `ORDER BY` for the special runtime-shaped table.
- [x] Verify null trailing values, all-null columns, malformed longer value
  vectors, absent requested PVs, and duplicate returned PV names.
- [x] Add query CLI examples and update query-engine architecture coverage for
  the new virtual table and its metadata/filter semantics.
- [x] Support metadata-driven `pv IN (SELECT pv ...)` and closed multi-window
  `window IN (SELECT time, end_time ...)` wide-table requests, including
  output validation, window coalescing, and activation `end_time` exposure.

## Exit Criteria

- A bounded multi-PV query produces a typed Arrow table with one timestamp
  row per MLDP shared timestamp and one data column per retained PV.
- Existing long-form queries remain behaviorally and schema compatible.
- Metadata and `column_type` filtering select whole virtual PV columns without
  inventing values or timestamps.
- Documentation shows a complete `SELECT *` query using
  `mldp.time_series_table`, `column_type`, and column metadata filters.
- [x] A metadata-derived PV list and configuration-derived closed activation
  ranges produce ordered, typed wide batches without crossing gaps.

## Verification

Passed in the devcontainer on 2026-07-23:

```bash
ctest --test-dir /workspace/build \
  -R '^(QueryParserTest\.|QueryCommandTest\.EnforcesTheSpecialTimeSeriesTableSelectStarContract|MLDPQueryClientTest\.|PlannerExecutorTest\.|QueryableMldpIntegrationTest\.)' \
  --output-on-failure
```

Result: **37/37 tests passed** in **1.12 seconds**.
