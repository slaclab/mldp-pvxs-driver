# Phase 6.1 — Native MLDP Time-Series Table

← [Back to main plan](query-client-impl.md)

## Goal

Add `mldp.time_series_table` alongside the existing long-form
`mldp.time_series`.  It retains MLDP's native `TABLE_FORMAT_COLUMN` response
as an Arrow table: one shared `time` column and one typed Arrow column for
each retained MLDP `DataColumn`.

```sql
SELECT time, "PV1", "PV2"
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
- Quoted PV identifiers select, filter, and sort generated table columns:

  ```sql
  SELECT time, "SYS:MAGNET:CURRENT", "SYS:VACUUM:PRESSURE"
  FROM mldp.time_series_table
  WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE')
    AND "SYS:MAGNET:CURRENT" > 10.0
    AND time >= NOW - 1h;
  ```

- A predicate on a quoted PV applies after the Arrow table is built.  A null
  or missing value does not match the predicate.
- `SELECT *` returns `time` followed by retained PV columns in requested-PV
  order.  Column metadata is not duplicated into every timestamp row.

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

For each retained column in `mldp.time_series_table`, attach its MLDP column
metadata to the corresponding Arrow field metadata.  The field's Arrow type
matches the native MLDP data type; the wide table must not use the long
table's dense-union `value` representation.

## Implementation Tasks

- [ ] Register `mldp.time_series_table` in `MLDPQueryClient`; preserve the
  current `mldp.time_series` behavior and schema.
- [ ] Add `column_type` to the time-series schema, SQL documentation, and
  response conversion.  Add a single shared mapping from MLDP value kind to
  public SQL type name and Arrow type.
- [ ] Reuse the existing `TABLE_FORMAT_COLUMN` request path.  Decode the
  shared `dataTimestamps` vector once and build the `time` Arrow column.
- [ ] Evaluate type, tag, attribute, and provenance predicates once for each
  returned `DataColumn`.  Drop non-matching data columns before producing the
  table schema.
- [ ] Build one Arrow array and field per retained `DataColumn`; preserve
  requested-PV order and validate unique returned PV names.
- [ ] Represent a returned data column shorter than the shared timestamp
  vector with trailing Arrow nulls.  Reject a column longer than the timestamp
  vector as an invalid MLDP response.
- [ ] Extend binding/planning so the mandatory PV predicate provides the
  runtime schema used to resolve quoted PV projection, predicate, and sort
  references.  Retain fixed schema entries for `time`, `column_type`, and
  metadata/control predicates.
- [ ] Fail clearly when a projected quoted PV is absent from the MLDP response
  or was excluded by a metadata/type predicate; include the PV name in the
  error.
- [ ] Permit value predicates only for Arrow types that support the requested
  comparison and return a binding/type error for unsupported comparisons.

## Tests

- [ ] Extend the MLDP query-client mock with multiple `DataColumn`s sharing
  timestamps, with distinct native types, tags, attributes, and provenance.
- [ ] Verify existing `mldp.time_series` output remains long-form and exposes
  / filters `column_type` correctly.
- [ ] Verify `mldp.time_series_table` returns one row per shared timestamp,
  native typed fields named after PVs, stable requested-PV order, and Arrow
  field metadata.
- [ ] Verify combined `pv`, time, `column_type`, tag, attribute, and
  provenance filters retain only the matching PV columns.
- [ ] Verify projections, `SELECT *`, quoted PV predicates, and `ORDER BY`
  over supported scalar PV columns.
- [ ] Verify null trailing values, malformed longer value vectors, absent
  projected PVs, duplicate returned PV names, and unsupported comparisons.
- [ ] Add query CLI examples and update query-engine architecture coverage for
  the new virtual table and its metadata/filter semantics.

## Exit Criteria

- A bounded multi-PV query produces a typed Arrow table with one timestamp
  row per MLDP shared timestamp and one data column per retained PV.
- Existing long-form queries remain behaviorally and schema compatible.
- Metadata and `column_type` filtering select whole virtual PV columns; quoted
  PV predicates filter table rows without inventing values or timestamps.
- Documentation shows a complete query using `mldp.time_series_table`, quoted
  EPICS PV names, `column_type`, and column metadata filters.
