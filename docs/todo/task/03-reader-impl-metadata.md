# TODO-03: Populate `batch.metadata` from merged reader + PV config in reader impls

## Goal
Replace the placeholder `batch.metadata["source"] = X` (set in TODO-01) with proper
merged metadata: reader-level `staticMetadata()` + PV-level `pv_config.metadata`
(PV-level wins on key conflict).

## Depends On
- TODO-01 (metadata field exists on EventBatchStruct)
- TODO-02 (staticMetadata() and PVConfig::metadata available from config)

## Files to Change

### EpicsArchiverReader
- `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp`
  - Find where `batch.metadata["source"] = batch.root_source` was set (from TODO-01 placeholder)
  - Replace with merge pattern:
    ```cpp
    auto merged = config_.staticMetadata();          // reader-level copy
    for (auto& [k, v] : pv_config.metadata)          // PV-level wins
        merged[k] = v;
    batch.metadata = std::move(merged);
    ```
  - `pv_config` is the `PVConfig` object for the current PV being processed. Check the
    surrounding code to find the correct variable name.

### EpicsPVXSReader
- `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`
  - Same merge pattern at each point where `eventBatch.metadata`, `tableBatch.metadata`,
    `markerBatch.metadata` are set (previously `tags.push_back`)
  - Locate the `PVConfig` for the current PV in scope — likely accessible via member config
    or a passed parameter

### EpicsBaseReader
- `src/reader/impl/epics/base/EpicsBaseReader.cpp`
  - Same merge pattern at each `.metadata` assignment

## Pattern (copy-paste ready)
```cpp
// Build merged metadata: reader-level base, PV-level overrides
auto merged = config_.staticMetadata();
for (auto& [k, v] : pv_cfg.metadata)
    merged[k] = v;
batch.metadata = std::move(merged);
```

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Optionally run reader integration tests
ctest --test-dir build -R "epics.*reader|archiver" -V 2>&1 | tail -30
```

## Commit
```
feat(metadata): wire reader + PV metadata into EventBatch

All reader impls now merge reader-level staticMetadata() and per-PV
metadata into EventBatch::metadata before pushing to the bus.
PV-level keys override reader-level on conflict.
```
