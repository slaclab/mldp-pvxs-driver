# TODO: BSAS Gen2 NTTable reader (`epics-pvxs-bsas-gen2`)

## Context

SLAC BSAS Gen2 IOCs publish an NTTable PV with a shared row axis and six statistics per acquired PV:

```
| secondsPastEpoch | nanoseconds | pid | PV1.VAL | PV1.CNT | PV1.AVG | PV1.RMS | PV1.MIN | PV1.MAX | PV2.VAL | ... |
```

Only Gen1 exists today (`type: slac-bsas-table` inside `epics-pvxs`, one column per PV). Gen2 is
documented as "planned" in [slac-bsas-table-gen2.md](../readers/slac-bsas-table-gen2.md).

Goal: a separate reader type, built on `EpicsPVXSReader`, whose output both the HDF5 writer and the
MLDP writer can consume.

**Approach:** the bus model itself learns about pulse IDs and per-PV statistics, instead of the
reader flattening everything into fake PVs:
- `TimestampEntry` gets an optional **pulse ID**, so every row carries `(sec, ns, pid)`;
- `DataColumn` gets ordered **extra value vectors** (the statistics), so one PV column carries
  `VAL` as its main values plus `CNT/AVG/RMS/MIN/MAX` as extras.

Each writer then decides how to store these:
- **HDF5** recreates the original table layout (a `pid` dataset plus one dataset per statistic).
- **MLDP** has no field for either yet, so for now it **ignores** them and stores only the PV value (`VAL`).

### Design decisions

- **Pulse ID goes on `TimestampEntry`, not in a column.**
  - It identifies the row (the time point) exactly as the timestamp does, and every column shares it.
  - Putting it on the timestamp keeps it aligned when frames are split, merged or written.
  - Use a generic, optional field: `std::optional<uint64_t> pulse_id`. Readers without one leave it empty.
  - Cost: `sizeof(TimestampEntry)` grows from 16 to 32 bytes. That is acceptable, but the
    `estimateDataBatchBytes` note must be updated.
- **Statistics go in an ordered container, not a hash table.** The order of the original table has
  to be preserved when HDF5 rebuilds it, so use a vector of named values:

  ```cpp
  struct ExtraColumnValues { std::string name; ColumnValues values; };   // e.g. {"CNT", vector<int64_t>}
  std::vector<ExtraColumnValues> extras;                                  // in DataColumn
  ```

  If keyed lookup is needed, add a small `findExtra(name)` helper. Statistic counts are small (about 6), so a linear search is fine.
- Every `extras[i].values` must have the same length as `values` (one entry per timestamp).

---

## 1. Bus model: `include/util/bus/DataBatch.h`

- [ ] Add `std::optional<uint64_t> pulse_id;` to `TimestampEntry` as the **last** member, defaulting to empty.
  - Existing `TimestampEntry{sec, ns}` initialisers (archiver, EPICS base/pvxs, Gen1 BSAS, PythonAlgorithm) keep compiling.
  - [ ] Update the size note: 16 → 32 bytes.
- [ ] Add `struct ExtraColumnValues { std::string name; ColumnValues values; };` and `std::vector<ExtraColumnValues> extras;` to `DataColumn`. Document the "same length as `values`" rule.
- [ ] Add a `findExtra(const DataColumn&, std::string_view)` helper.
- [ ] Update `estimateDataBatchBytes`:
  - count extras (name plus payload, using the existing visitor);
  - count `TimestampEntry` at its new size.
- [ ] Check every `DataColumn` consumer still behaves correctly when extras are present; extras that a consumer doesn't use are simply ignored:
  - `src/processor/impl/PythonAlgorithm.cpp`: pass through or ignore; decide whether Python sees them.
  - `src/query/impl/mldp/MLDPQueryClient.cpp`
  - enrichers and processors that copy or rebuild columns must not drop extras or the pulse ID by accident: `grep DataColumn src/enricher src/processor`.
- [ ] Unit tests: byte estimate with extras; default `TimestampEntry` has no pulse ID.

## 2. Hook in `EpicsPVXSReader`

Files: `include/reader/impl/epics/pvxs/EpicsPVXSReader.h`, `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`

- [ ] Add a protected constructor `EpicsPVXSReader(bus, metrics, cfg, std::string_view clientTag, bool startMonitoring)`.
  - The public constructor delegates to it with `("epics-pvxs", true)`.
  - The subclass passes `false` and calls `addPV(pvNames())` at the end of its own constructor, so no event dispatches into a partly built object.
- [ ] Make `processEvent` `protected virtual` and move `logAndRecordError` to `protected`.
- [ ] Extract `recordEventMetrics(pvName, sourceTag, dataBytes, emitted, processing_start)` from `processEvent`, including the `last_event_time_` logic.
- [ ] Run `epics_pvxs_reader_test` to confirm `epics-pvxs` behaviour is unchanged.

## 3. Config: `EpicsPVXSBsasGen2ReaderConfig`

New files: `include/reader/impl/epics/pvxs/EpicsPVXSBsasGen2ReaderConfig.h`, `src/.../EpicsPVXSBsasGen2ReaderConfig.cpp`

- [ ] Wrap `EpicsPVXSReaderConfig`, which already provides `pvs:`, pool size, `columnBatchSize`, metadata and client config.
- [ ] Parse the per-PV `option` subtree:

```yaml
readers:
  - epics-pvxs-bsas-gen2:
      - name: bsas_gen2
        columnBatchSize: 50                          # frames per bus EventBatch
        pvs:
          - name: "BSAS:GEN2:TABLE"
            option:
              pidField: pid                          # default: pid
              tsSeconds: secondsPastEpoch            # default
              tsNanos: nanoseconds                   # default
              valueStat: VAL                         # statistic used as DataColumn::values; default VAL
              stats: [VAL, CNT, AVG, RMS, MIN, MAX]  # default; suffix match is case-insensitive
```

- [ ] Validate:
  - field names are not empty;
  - `stats` is not empty and has no duplicates;
  - `valueStat` is one of the `stats`.

## 4. Conversion: `BSASGen2DataBatchConversion`

New files: `include/reader/impl/epics/pvxs/BSASGen2DataBatchConversion.h`, `src/.../BSASGen2DataBatchConversion.cpp`

- [ ] Write a pure static function (testable without an IOC) that builds one `DataBatch`:
  - [ ] `timestamps[i] = {sec[i], ns[i], pid[i]}`; the pulse ID is set on every row.
  - [ ] One `DataColumn` per acquired PV:
    - `name` = PV base name;
    - `values` = the `valueStat` array;
    - `extras` = the other statistics, in the configured order.
- [ ] Locate `value`, then the pid and timestamp arrays: top level first, falling back to `value.*`, as Gen1 does. Reuse or factor out the timestamp normalisation helpers from `BSASEpicsDataBatchConversion.cpp`.
- [ ] Row count is the pid array length. A statistic array with a different length is logged and dropped; the rest of its PV is kept. If the `valueStat` array is the bad one, drop the whole PV.
- [ ] Take display names from `labels[i]` when present (PV names contain `:`), otherwise from field names.
- [ ] Split each name on its last `.` or `_` and match the suffix against `stats`, ignoring case. Columns that match nothing (and are not pid or timestamps) are skipped with a debug log.
- [ ] Value types:
  - float arrays → `std::vector<double>`;
  - integer arrays → `std::vector<int64_t>`, so `CNT` stays an integer.
- [ ] Column metadata: `{"bsas_table": tablePv}`.

## 5. Reader: `EpicsPVXSBsasGen2Reader`

New files: `include/reader/impl/epics/pvxs/EpicsPVXSBsasGen2Reader.h`, `src/.../EpicsPVXSBsasGen2Reader.cpp`

- [ ] Declare `class EpicsPVXSBsasGen2Reader : public EpicsPVXSReader` with `REGISTER_READER("epics-pvxs-bsas-gen2", ...)`.
- [ ] Override `processEvent`:
  - [ ] Convert. On failure, call `logAndRecordError` and return.
  - [ ] Push tabular `EventBatch`es, following the pattern in `HDF5BsasGen1Reader::emitChunk`:
    - payload `TimeSeriesPayload{.root_source_name = tablePv, .is_tabular = true}`;
    - metadata from `mergedMetadataFor(pv)`, plus `reader_name`;
    - frames of up to `columnBatchSize` PVs, all sharing the timestamps (with pulse IDs).
  - [ ] Push the `end_of_batch_group` marker, then call `recordEventMetrics(...)`.
- [ ] Keep all-NaN statistics. They are summaries and NaN carries meaning (unlike Gen1, which drops them).
- [ ] Add the new sources to `CMakeLists.txt`.

## 6. HDF5 writer: write pulse IDs and extras

Files: `src/writer/hdf5/HDF5WriterBase.cpp`, `HDF5WriterPerSource.cpp`, `HDF5WriterMerge.cpp`, `include/writer/hdf5/HDF5WriterBase.h`

Target layouts:

- **One file per source** (`HDF5WriterPerSource::appendFrame`): next to the existing `timestamps` dataset, write a `pulse_id` dataset when the timestamps carry one. Next to each column dataset `<PV>`, write `<PV>.<STAT>` datasets for its extras.
  The file then holds `ts, pid, VAL, CNT, AVG, RMS, MIN, MAX`, the same as the original table.
- **Aggregated / tabular** (`accumulateTabularFrame` / `flushTabularBuffer`, one group per table source): add a shared `pulse_id` dataset next to the shared `secondsPastEpoch` / `nanoseconds`. Every PV contributes `<PV>` plus `<PV>.<STAT>` columns, alongside all other PVs in the batch.
  This rebuilds the original wide NTTable.

Tasks:
- [ ] `TabularBuffer`: add `std::vector<int64_t> pulseIds` and a `hasPulseId` flag, filled in `accumulateTabularFrame` together with `tsSeconds` / `tsNanos`.
  - If one round mixes frames with and without pulse IDs, log a warning and store pulse ID `-1` for the rows that have none.
- [ ] `accumulateTabularFrame`: register each extra as a regular tabular column named `<col.name>.<extra.name>`, in order, right after its parent column. This reuses the existing per-column variant storage, the schema lock and the unknown-column handling.
- [ ] `flushTabularBuffer`: write `<group>/pulse_id` (int64) when `hasPulseId` is set.
- [ ] `HDF5WriterPerSource::appendFrame` / `writeColumnsImpl`: write `pulse_id` next to `timestamps`, and the `<PV>.<STAT>` datasets for extras (1-D and 2-D array variants through the existing helpers).
- [ ] `HDF5WriterMerge::appendFrameMerge`: same as per-source, under the merge group prefix. Include extras in the byte estimate (`approxBytes`).
- [ ] Write the column metadata on extra datasets too, plus a `parent_column` attribute naming the PV.
- [ ] Batches without pulse IDs or extras must produce byte-identical files to today. Cover this with a regression test on the existing HDF5 tests.
- [ ] Tests:
  - per-source file contains `timestamps`, `pulse_id`, `<PV>`, `<PV>.CNT`…;
  - tabular group contains the shared ts and pid plus all PVs and their statistics;
  - merge mode does the same;
  - integer `CNT` keeps an int64 dataset type.

## 7. MLDP writer: ignore pulse ID and extras (for now)

File: `src/writer/mldp/MLDPWriter.cpp` (`DataBatch` → `DataFrame` conversion, ~line 600)

- [ ] No functional change: the MLDP writer sends only `DataColumn::values` (the PV value, e.g. `VAL`)
      with the frame timestamps. `TimestampEntry::pulse_id` and `DataColumn::extras` are ignored.
- [ ] Add a short comment at the conversion site stating extras/pulse ID are intentionally dropped until
      MLDP has a native field for them.
- [ ] Test: a `DataBatch` with extras and pulse IDs produces exactly the same `DataFrame` as the same
      batch without them.

## 8. Mock IOC and tests

- [ ] `test/mock/sioc.cpp`: add a Gen2 NTTable PV `test:bsas_gen2_table` with:
  - `labels`;
  - `pid` (uint64);
  - `secondsPastEpoch` / `nanoseconds`;
  - 2 PVs × 6 statistics: `CNT` uint32, the rest float64.
- [ ] Conversion unit tests (`test/reader/impl/epics/pvxs/bsas_gen2_conversion_test.cpp`):
  - [ ] naming (labels vs field names, separators, case);
  - [ ] pulse IDs on the timestamps;
  - [ ] `values` comes from `valueStat`, and extras keep the configured order;
  - [ ] missing pid or timestamps fails;
  - [ ] a statistic with a mismatched length is dropped;
  - [ ] unknown columns are ignored.
- [ ] Config tests: defaults, overrides, invalid values.
- [ ] Reader test against the mock IOC: the bus receives 2 PV columns, each with 5 extras, timestamps with pulse IDs, and the end-of-batch marker.
- [ ] End-to-end tests: the reader feeding the HDF5 writer in per-source, merge and tabular modes, and the reader feeding the MLDP writer (devcontainer MLDP stack); in MLDP only `<PV>` values are queryable.

## 9. Docs

- [ ] `docs/readers/slac-bsas-table-gen2.md`: the new reader type, its config, and how the data is mapped (pulse ID on timestamps, statistics as extras).
- [ ] `docs/writers/hdf5-writer.md`: the `pulse_id` dataset and the `<PV>.<STAT>` datasets in each mode.
- [ ] `docs/writers/mldp-writer.md`: note that pulse IDs and extra columns are not written yet.
- [ ] `docs/readers/readers.md`: remove "(planned)" and add the reader.

## Critical files

- **Modify:**
  - `include/util/bus/DataBatch.h`
  - `include/reader/impl/epics/pvxs/EpicsPVXSReader.h`
  - `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`
  - `include/writer/hdf5/HDF5WriterBase.h`
  - `src/writer/hdf5/HDF5WriterBase.cpp`, `HDF5WriterPerSource.cpp`, `HDF5WriterMerge.cpp`
  - `src/writer/mldp/MLDPWriter.cpp` (comment + test only)
  - `CMakeLists.txt`
  - `test/mock/sioc.cpp`
  - the docs above
- **New:**
  - `EpicsPVXSBsasGen2Reader{.h,.cpp}`
  - `EpicsPVXSBsasGen2ReaderConfig{.h,.cpp}`
  - `BSASGen2DataBatchConversion{.h,.cpp}`
  - the tests above
- **Reuse:**
  - `PvxsClientConfig::buildContext`
  - `EpicsReaderBase::mergedMetadataFor`
  - the Gen1 timestamp helpers
  - the batch-and-marker pattern from `HDF5BsasGen1Reader::emitChunk`
  - the HDF5 `TabularBuffer` per-column variant storage
  - `ensureDataset` / `append1D`

## Suggested order

1. Bus model (§1), plus tests: everything else depends on it.
2. HDF5 writer (§6), testable with hand-built `DataBatch`es; MLDP no-change test (§7).
3. Reader hook, config, conversion and reader (§2–5).
4. Mock IOC and end-to-end tests (§8), then docs (§9).

## Verification

```bash
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml exec devcontainer bash -lc \
  "cmake --build /workspace/build --parallel && cd /workspace/build && \
   ./bin/mldp_pvxs_driver_test --gtest_filter='-Python*:EpicsBase*:MLDPPVXSControllerProcessorIntegrationTest.*'"
```

- [ ] The full suite passes, including the new Gen2, HDF5 extras and MLDP flattening tests.
- [ ] Manual check: run the driver with the Gen2 reader on the mock PV and both writers enabled.
  - `h5dump -H` shows `pulse_id` and `<PV>.<STAT>` datasets.
  - `SELECT pv, time, value FROM mldp.time_series WHERE pv = '<PV>' AND time >= NOW - 5m` returns the `VAL` series only.
