# TODO: BSAS Gen1 / Gen2 EPICS table readers — implemented (phase 1) (`epics-pvxs-bsas-gen1`, `epics-pvxs-bsas-gen2`)

## Context

Today `EpicsPVXSReader` (`epics-pvxs`) handles two very different things, selected per PV by
`option.type`:
- plain scalar / scalar-array PVs (Default mode);
- SLAC BSAS Gen1 NTTables (`type: slac-bsas-table`).

Goal: `epics-pvxs` goes back to being a **plain scalar PV reader only**, and two dedicated readers,
both subclasses of `EpicsPVXSReader`, subscribe to BSAS NTTable PVs and push data shaped like
`HDF5BsasGen1Reader::emitChunk` does for HDF5 files:

- **`epics-pvxs-bsas-gen1`** — [Gen1 table](../readers/slac-bsas-table-gen1.md): every non-timestamp
  column is a PV value column.
- **`epics-pvxs-bsas-gen2`** — [Gen2 table](../readers/slac-bsas-table-gen2.md): ingest **only** the
  same data as Gen1: `secondsPastEpoch`, `nanoseconds` and the `<PV>.Val` column (renamed to `<PV>`).
  The **pulse ID column is omitted entirely**, as are `CNT`, `RMS`, `AVG`, `MIN`, `MAX`.

This is **phase 1**. Phase 2 — carrying pulse ID and per-PV statistics through the bus and writers once
MLDP can store them — is described in [TODO-bsas-gen2-reader.md](TODO-bsas-gen2-reader.md) (§1, §6, §7).

### Design decisions

- **Output shape matches `HDF5BsasGen1Reader`.** Per NTTable update:
  - one or more tabular `EventBatch`es:
    `TimeSeriesPayload{.root_source_name = <table PV>, .is_tabular = true}`;
  - frames of up to `columnsPerFrame` columns, all sharing the row timestamps;
  - metadata = `mergedMetadataFor(pv)` + `source` + `reader_name`;
  - then an `end_of_batch_group` marker.
  Downstream writers (HDF5 tabular, MLDP) then see the same thing from file and live sources.
- **No push retry loop.** Unlike the file reader, a PVXS stream cannot be paused. A rejected
  `bus_->push()` is logged and counted as a reader error; the update is dropped.
- **Gen2 drops pid and statistics in the reader**, not in the writers. The bus model is unchanged in
  phase 1 (no `pulse_id` on `TimestampEntry`, no `extras` on `DataColumn`).
- **Filter before conversion.** Gen2 tables have six arrays per PV; only one is used. Columns are
  filtered by name before typed-array conversion so the five unused statistics are never converted.
- **NaN handling** stays as in Gen1 today: all-NaN columns are discarded (`hasValidData`).

---

## 1. Hooks in `EpicsPVXSReader`

Files: `include/reader/impl/epics/pvxs/EpicsPVXSReader.h`, `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`

- [x] Add a protected constructor `EpicsPVXSReader(bus, metrics, cfg, std::string_view clientTag, bool startMonitoring)`.
  - The public constructor delegates to it with `("epics-pvxs", true)`.
  - Subclasses pass `false` and call `addPV(pvNames())` at the end of their own constructor, so no
    event is dispatched into a partly built object.
- [x] Make `processEvent` `protected virtual`; move `logAndRecordError` to `protected`.
- [x] Extract `recordEventMetrics(pvName, sourceTag, dataBytes, emitted, processing_start)` from
  `processEvent` (processing time, events, bytes total, bytes/s with `last_event_time_`).
- [x] Move `estimatePvxsValueBytes` from the anonymous namespace to a protected static member.
- [x] Add a protected virtual `monitorRequest()` (default `field(value,alarm,timeStamp)`) so table
  readers can request what they need (e.g. `labels`). Check against the mock IOC what Gen1 needs today.

## 2. Remove table mode from `EpicsPVXSReader`

- [x] Move `processSlacBsasTableMode`, `hasTimestamp`, `makeTableEventBatch` and the frame-grouping
  logic into the table base class (§3).
- [x] Drop the `SlacBsasTable` dispatch from `processEvent`; `epics-pvxs` handles Default mode only.
- [x] `option.type: slac-bsas-table` under `epics-pvxs` → configuration error whose message points to
  `epics-pvxs-bsas-gen1`.
- [x] Check whether `EpicsBaseReader` shares `PVRuntimeConfig::Mode::SlacBsasTable` via
  `EpicsReaderBase`; keep it working there or decide separately.
- [x] Migrate existing tests, sample configs and docs that use `slac-bsas-table` under `epics-pvxs`.

## 3. Table base: `EpicsPVXSBsasTableReaderBase`

New files: `include/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.h`, `src/.../EpicsPVXSBsasTableReaderBase.cpp`

- [x] Abstract class deriving from `EpicsPVXSReader`.
- [x] Override `processEvent`:
  1. `running_` check, start timer, `sourceTag`.
  2. Call the pure virtual `extractColumns(pvName, value, emitFn)`, which yields `(colName, DataBatch)`.
  3. Group columns into frames of `columnsPerFrame` sharing the first timestamps vector; drop frames
     without timestamps (error metric) and all-NaN columns (trace log).
  4. Push tabular `EventBatch`es (flush when `columnBatchSize` frames are reached), then the marker.
  5. `recordEventMetrics(...)`.
- [x] Reuse `BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch` (timestamp lookup top-level →
  `value.*`, normalisation, parallel conversion through `reader_pool_`).

## 4. Config: `EpicsPVXSBsasTableReaderConfig`

New files: `include/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.h`, `src/.../EpicsPVXSBsasTableReaderConfig.cpp`

- [x] Wrap `EpicsPVXSReaderConfig` (`pvs:`, pool size, `columnBatchSize`, metadata, client config).
- [x] Reader-level keys, overridable per PV under `option`:

```yaml
readers:
  - epics-pvxs-bsas-gen1:
      - name: bsas_gen1
        columnsPerFrame: 1           # columns per DataBatch frame; 0 = all
        columnBatchSize: 50          # frames per bus EventBatch
        tsSeconds: secondsPastEpoch  # default
        tsNanos: nanoseconds         # default
        pvs:
          - name: "BSAS:GEN1:TABLE"
  - epics-pvxs-bsas-gen2:
      - name: bsas_gen2
        pidField: pid                # default; column always skipped
        valueSuffix: Val             # default; case-insensitive
        pvs:
          - name: "BSAS:GEN2:TABLE"
```

- [x] Validate: field names non-empty; `option.type` rejected (the mode is implied by the reader type).

## 5. Gen1 reader: `EpicsPVXSBsasGen1Reader`

- [x] `extractColumns` = direct pass-through of the `tryBuildNtTableRowTsBatch` callback.
- [x] `REGISTER_READER("epics-pvxs-bsas-gen1", EpicsPVXSBsasGen1Reader)`.

## 6. Gen2 reader: `EpicsPVXSBsasGen2Reader`

- [x] Add an optional `columnFilter` parameter to `tryBuildNtTableRowTsBatch`
  (`std::function<std::optional<std::string>(std::string_view)>`: `nullopt` = skip, value = output
  name). Default null keeps today's behaviour.
- [x] Filter:
  - skip `pidField` and the timestamp columns;
  - split the name on its last `.` (fallback `_`); keep only suffix == `valueSuffix` (case-insensitive);
  - output name = PV base name (set `DataColumn::name` too).
- [x] Check whether field names are mangled for PV names containing `:`; if so, take names from
  `labels[i]`.
- [x] `REGISTER_READER("epics-pvxs-bsas-gen2", EpicsPVXSBsasGen2Reader)`.

## 7. Build, mock IOC, tests

- [x] Add new sources to `CMakeLists.txt`.
- [x] `test/mock/sioc.cpp`: add `test:bsas_gen2_table` with `labels`, `pid` (uint64),
  `secondsPastEpoch` / `nanoseconds`, 2 PVs × `Val/CNT/RMS/AVG/MIN/MAX`.
- [x] Tests in `test/reader/impl/epics/pvxs/` (mirror `epics_pvxs_reader_test`):
  - Gen1: tabular batches with all PV columns, correct row timestamps, `columnsPerFrame` grouping, marker.
  - Gen2: only `<PV>` columns with `.Val` values; no `pid`, no statistics; marker.
  - Gen2 filter unit test without IOC (naming, separators, case).
  - Config: defaults, overrides, invalid values, `option.type` rejected.
  - `epics-pvxs` scalar tests unchanged; `slac-bsas-table` under `epics-pvxs` now fails config.

## 8. Docs

- [x] `docs/readers/slac-bsas-table-gen1.md`: new reader type and config.
- [x] `docs/readers/slac-bsas-table-gen2.md`: new reader type, config; "currently ingests `Val` only;
  pulse ID and statistics are omitted"; update the implementation-references table.
- [x] `docs/readers/readers.md`: list both readers; `epics-pvxs` is scalar-only.

## Critical files

- **Modify:** `EpicsPVXSReader.{h,cpp}`, `BSASEpicsDataBatchConversion.{h,cpp}`, `EpicsPVXSReaderConfig`
  (reject table type), `CMakeLists.txt`, `test/mock/sioc.cpp`, docs above.
- **New:** `EpicsPVXSBsasTableReaderBase`, `EpicsPVXSBsasTableReaderConfig`, `EpicsPVXSBsasGen1Reader`,
  `EpicsPVXSBsasGen2Reader` (`.h` + `.cpp`), tests.
- **Reuse:** `PvxsClientConfig::buildContext`, `EpicsReaderBase::mergedMetadataFor`,
  `BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch`, batch + marker pattern from
  `HDF5BsasGen1Reader::emitChunk`.

## Suggested order

1. Base hooks (§1), run existing `epics-pvxs` tests.
2. Table base + config + Gen1 reader (§3–5); move table mode out of base (§2) and migrate tests.
3. Gen2 filter + reader + mock PV (§6–7).
4. Docs (§8).

## Verification

```bash
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml exec devcontainer bash -lc \
  "cmake --build /workspace/build --parallel && cd /workspace/build && \
   ./bin/mldp_pvxs_driver_test --gtest_filter='*Bsas*:*EpicsPVXS*'"
```

- [x] Then the full suite (`--gtest_filter='-Python*:EpicsBase*:MLDPPVXSControllerProcessorIntegrationTest.*'`).
- [ ] Manual: run the driver with both readers on the mock PVs and the HDF5 writer in tabular mode;
  each group holds the timestamps plus PV columns only (no `pid`, no statistics for Gen2).

## Implementation notes (deviations from the plan above)

- §1 `monitorRequest()`: passed as a constructor argument of the protected
  `EpicsPVXSReader` constructor instead of a virtual (a virtual cannot be used from the
  base constructor). Table readers request `field()` (whole NTTable incl. `labels`).
- §1 `stopProcessing()` added; the table base destructor drains the pool before its
  members (`table_config_`, per-PV filters) are destroyed.
- §3 `extractColumns` was not needed: the base passes a per-PV `ColumnFilter` (built
  once from the table options) to `tryBuildNtTableRowTsBatch`. Gen1 has no filter.
- §6 the filter reads `labels[i]` when `labels` maps 1:1 onto `value.*` (real PV names
  such as `X:Y.Val`), else the sanitized field name (`X_Y_Val`).
- §2 `EpicsBaseReader` keeps `option.type: slac-bsas-table` unchanged.
- `config validate` knows both new reader types and flags `option.type` under
  `epics-pvxs` / the table readers; the wizard round-trips both reader types.
- `Metrics::readerEventsTotal/readerErrorsTotal` were declared but not defined; now
  defined (with optional labels) and used by the tests.
- Mock IOC: `test/mock/BsasTablePV` publishes deterministic tables
  (`test:bsas_gen1_*`, `test:bsas_gen2_*`) so tests assert exact values/timestamps.
- Table batching keys (2026-11-04): `rows-per-data-batch` (default 120), `pvs-per-data-batch`
  (default 1), `data-batches-per-push` (default 50); all reader-level with per-PV override.
  `column-batch-size` is rejected by the table readers. DataBatches are ordered by row
  chunk, then PV column.
