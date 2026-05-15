# Plan: Static Metadata (k/v) on Reader Configs

## Context

Readers need to attach user-defined static key/value string pairs to ingested data at configuration time. These labels (e.g. facility, system, units, experiment) should flow from the YAML config → reader config → `EventBatch` → writers. Today `EventBatchStruct` already has a `tags: vector<string>` field but it is never populated from config and is silently dropped by both writers.

This plan replaces the flat `tags` vector with a proper `map<string,string>` metadata map, wires it through from reader config to both writers.

---

## Scope

Touch these layers, in order:

1. **`EventBatchStruct`** — replace `tags` with `metadata`
2. **Both reader config structs** — add `metadata` map parsed from YAML
3. **Both reader impls** — populate `EventBatch::metadata` from config
4. **MLDPWriter** — forward metadata into `IngestDataRequest`
5. **HDF5WriterBase** — write metadata as HDF5 group attributes
6. **Tests** — add/update unit tests for config parsing and propagation

---

## Critical Files

| File | Change |
|---|---|
| `include/util/bus/IDataBus.h` | Replace `vector<string> tags` → `map<string,string> metadata` |
| `include/reader/impl/epics_archiver/EpicsArchiverReaderConfig.h` | Add `map<string,string> metadata_` to `PVConfig` and class-level `static_metadata_` |
| `src/reader/impl/epics_archiver/EpicsArchiverReaderConfig.cpp` | Parse `metadata:` YAML map under each PV and at reader level |
| `include/reader/impl/epics/shared/EpicsReaderConfig.h` | Same: add `metadata_` to `PVConfig` and class level |
| `src/reader/impl/epics/shared/EpicsReaderConfig.cpp` | Parse `metadata:` YAML map |
| `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` | Merge config metadata into `EventBatch::metadata` before push |
| `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp` | Same |
| `src/reader/impl/epics/base/EpicsBaseReader.cpp` | Same |
| `src/writer/mldp/MLDPWriter.cpp` | Forward `metadata` into protobuf request (labels/tags fields) |
| `src/writer/hdf5/HDF5WriterBase.cpp` | Write `metadata` as HDF5 attributes on the source group |

---

## Design

### 1. `EventBatchStruct` change

```cpp
// include/util/bus/IDataBus.h
struct EventBatchStruct {
    std::string                         reader_name;
    std::string                         root_source;
    std::unordered_map<std::string, std::string> metadata;  // ← replaces tags
    std::vector<util::bus::DataBatch>   frames;
    bool                                end_of_batch_group{false};
    bool                                is_tabular{false};
};
```

`tags: vector<string>` removed entirely — no existing code writes to it meaningfully.

---

### 2. Reader config structs

Two scopes for metadata:

- **Reader-level** (`static_metadata_`): applies to all PVs in this reader instance
- **PV-level** (`metadata` in `PVConfig`): per-PV overrides/additions, merged on top of reader-level

#### YAML schema (same for both reader types):

```yaml
readers:
  - type: epics-archiver
    name: my-archiver
    hostname: archiver.example.com:11200
    metadata:           # reader-level static metadata
      facility: LCLS
      experiment: CXI-2024
    pvs:
      - name: BPMS:LI21:201:X
        metadata:       # PV-level metadata (merges/overrides reader-level)
          system: BPM
      - name: BPMS:LI21:201:Y
```

#### Config struct additions (both `EpicsReaderConfig` and `EpicsArchiverReaderConfig`):

```cpp
// in class / PVConfig:
static constexpr auto MetadataKey = "metadata";

// class-level:
std::unordered_map<std::string, std::string> static_metadata_;

// PVConfig:
struct PVConfig {
    std::string name;
    std::unordered_map<std::string, std::string> metadata;  // merged at push time
    // ... existing fields ...
};

// accessor:
const std::unordered_map<std::string, std::string>& staticMetadata() const;
```

#### Merge semantics

At push time, reader merges reader-level + PV-level metadata:

```cpp
auto merged = config_.staticMetadata();          // reader-level base
for (auto& [k, v] : pv_config.metadata)
    merged[k] = v;                               // PV-level wins on conflict
batch.metadata = std::move(merged);
```

#### YAML parsing (both config `.cpp` files):

```cpp
// Parse reader-level:
if (cfg.hasKey(MetadataKey)) {
    std::unordered_map<std::string, std::string> m;
    cfg.subConfig(MetadataKey) >> m;    // uses existing Config >> map<string,string>
    static_metadata_ = std::move(m);
}

// Parse PV-level (inside PV loop):
if (pv_cfg.hasKey(MetadataKey)) {
    std::unordered_map<std::string, std::string> m;
    pv_cfg.subConfig(MetadataKey) >> m;
    pv.metadata = std::move(m);
}
```

`Config >> map<string,string>` operator already exists — no new parsing utility needed.

---

### 3. Writer changes

#### MLDPWriter (`src/writer/mldp/MLDPWriter.cpp`)

`QueueItem` already captures `tags` (rename to `metadata`). Wire into `buildRequest()` / `toDataFrame()`.

Metadata goes on **each column's provenance field**:

```cpp
// In toDataFrame(), per-column loop:
auto* col_meta = col.mutable_metadata();           // or equivalent proto accessor
auto* prov = col_meta->mutable_provenance();
for (auto& [k, v] : *item.metadata)
    (*prov->mutable_labels())[k] = v;              // exact field name TBD from proto inspection
```

All columns in a batch share the same static metadata map (reader + PV level merged).

#### HDF5WriterBase (`src/writer/hdf5/HDF5WriterBase.cpp`)

Write metadata as HDF5 attributes on the source group when group is first created:

```cpp
// When opening/creating the source group for root_source:
auto group = file_.require_group(batch.root_source);
for (auto& [k, v] : batch.metadata)
    group.attrs()[k] = v;   // HighFive API (or equivalent used in codebase)
```

Only write attributes on group creation (not every batch) to avoid repeated writes.

---

## Verification

1. **Unit tests** — `EpicsArchiverReaderConfig` parse test: YAML with reader-level and PV-level `metadata:` maps, assert fields populated correctly
2. **Unit tests** — `EpicsReaderConfig` same
3. **Unit tests** — merge semantics: PV-level key overrides reader-level key
4. **Integration** — run archiver reader in `historical_once` mode with metadata in YAML, verify `EventBatch::metadata` populated before push (log it)
5. **HDF5** — open output file, check group attributes match configured metadata
6. **MLDP gRPC** — verify labels appear in `IngestDataRequest` (inspect with gRPC interceptor or log)

---

## Open Questions

1. **HDF5 API**: Which HDF5 C++ wrapper is used (HighFive? direct HDF5 C API)? Affects attribute write syntax.
