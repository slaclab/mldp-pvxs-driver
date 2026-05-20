# TODO-02: Add `metadata` fields to both reader config structs

## Goal
Add `static_metadata_` (reader-level) and `PVConfig::metadata` (PV-level) to both
`EpicsArchiverReaderConfig` and `EpicsReaderConfig`, with YAML parsing.

## Files to Change

### EpicsArchiverReaderConfig
- `include/reader/impl/epics_archiver/EpicsArchiverReaderConfig.h`
  - Add `#include <unordered_map>` and `#include <string>`
  - Add field to class: `std::unordered_map<std::string, std::string> static_metadata_;`
  - Add accessor: `const std::unordered_map<std::string, std::string>& staticMetadata() const { return static_metadata_; }`
  - In `PVConfig` struct: add field `std::unordered_map<std::string, std::string> metadata;`
  - Add `static constexpr auto kMetadataKey = "metadata";`

- `src/reader/impl/epics_archiver/EpicsArchiverReaderConfig.cpp`
  - After parsing other reader-level keys, add:
    ```cpp
    if (cfg.hasChild(kMetadataKey)) {
        std::map<std::string, std::string> m;
        cfg.subConfig(kMetadataKey).front() >> m;
        static_metadata_.insert(m.begin(), m.end());
    }
    ```
  - Inside the PV parsing loop, after constructing each `PVConfig`, add:
    ```cpp
    if (pv_cfg.hasChild(kMetadataKey)) {
        std::map<std::string, std::string> m;
        pv_cfg.subConfig(kMetadataKey).front() >> m;
        pv.metadata.insert(m.begin(), m.end());
    }
    ```
  - Add `#include <map>` if not present (needed for `std::map` intermediate used with `operator>>`)

### EpicsReaderConfig (EPICS base + PVXS shared config)
- `include/reader/impl/epics/shared/EpicsReaderConfig.h`
  - Same additions as EpicsArchiverReaderConfig header above

- `src/reader/impl/epics/shared/EpicsReaderConfig.cpp`
  - Same parsing additions as EpicsArchiverReaderConfig.cpp above

## YAML schema (for reference / tests)
```yaml
readers:
  - type: epics-archiver
    metadata:
      facility: LCLS
      experiment: CXI-2024
    pvs:
      - name: BPMS:LI21:201:X
        metadata:
          system: BPM
```

## Config API notes
- Use `cfg.hasChild(key)` NOT `cfg.hasKey(key)`
- Use `cfg.subConfig(key).front() >> m` where `m` is `std::map<string,string>`
- `Config::operator>>(map<string,string>&)` is implemented in `src/config/Config.cpp:237`

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Config unit tests still pass
ctest --test-dir build -R "reader_config" -V 2>&1 | tail -20
```

## Commit
```
feat(metadata): add static_metadata and PVConfig::metadata to reader configs

Both EpicsArchiverReaderConfig and EpicsReaderConfig now parse an optional
metadata: YAML map at reader-level and per-PV level. Reader impls will
consume these fields in the next todo.
```
