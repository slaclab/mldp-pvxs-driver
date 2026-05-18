# Plan: Variant-Payload Bus + PV Metadata Reader + Annotation Writer

## Context

The driver currently has a single `EventBatchStruct` that carries only time-series data (timestamped column frames). A new requirement calls for a PV metadata reader (reads pvName, aliases, tags, attributes from config) and an MLDP annotation writer (calls `DpAnnotationService.savePvMetadata`). These two new components carry a structurally different payload — no timestamps, no column frames — so the bus needs to accommodate multiple payload types without multiplying bus instances.

The solution uses `std::variant<TimeSeriesPayload, PvMetadataPayload>` inside `EventBatchStruct`, preserves 100% backward compatibility (existing readers/writers touch only `TimeSeriesPayload`), and reuses the existing `RouteTable` for reader→writer affinity. No new bus, no new controller.

---

## Step 1 — Add `annotation.proto` to CMake

**File:** `CMakeLists.txt` (lines 420–431, `PROTO_FILES` list)

```cmake
set(PROTO_FILES
    ${DP_GRPC_PROTO_PATH}/common.proto
    ${DP_GRPC_PROTO_PATH}/query.proto
    ${DP_GRPC_PROTO_PATH}/ingestion.proto
    ${DP_GRPC_PROTO_PATH}/annotation.proto   # ADD
)
```

This generates `annotation.pb.h` / `annotation.grpc.pb.h` as part of the existing `lib${PROJECT_NAME}_proto` target. No other CMake change needed — annotation stubs share the same link target.

---

## Step 2 — New annotation-specific pool (no changes to existing pool files)

`MLDPGrpcPoolConfig` carries ingestion/query URLs and provider registration fields — none of which the annotation writer needs. `MLDPGrpcObject` carries ingestion + query stubs — also not needed. Instead, introduce a dedicated, lightweight annotation pool stack.

### 2a — `MLDPGrpcAnnotationPoolConfig`

**File:** `include/pool/MLDPGrpcAnnotationPoolConfig.h` (new)

```cpp
namespace mldp_pvxs_driver::util::pool {

inline constexpr char AnnotationUrlKey[] = "annotation-url";

class MLDPGrpcAnnotationPoolConfig {
public:
    explicit MLDPGrpcAnnotationPoolConfig(const config::Config& root);

    bool               valid() const;
    const std::string& annotationUrl() const;
    int                minConnections() const;
    int                maxConnections() const;
    // Reuse MLDPGrpcPoolConfig::Credentials for TLS — import the type
    const MLDPGrpcPoolConfig::Credentials& credentials() const;

private:
    bool        valid_{false};
    std::string annotation_url_;
    int         min_conn_{1};
    int         max_conn_{4};
    MLDPGrpcPoolConfig::Credentials credentials_;
};

} // namespace mldp_pvxs_driver::util::pool
```

**YAML schema** (under writer config):
```yaml
mldp-annotation-pool:
  annotation-url: "localhost:50052"   # same host as query in most deployments
  min-conn: 1
  max-conn: 4
  credentials: none   # or ssl / custom map — same syntax as existing pools
```

---

### 2b — `MLDPGrpcAnnotationObject`

**File:** `include/pool/MLDPGrpcAnnotationPool.h` (new, defined inline before the pool class)

```cpp
struct MLDPGrpcAnnotationObject {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<dp::service::annotation::DpAnnotationService::Stub> stub;

    explicit MLDPGrpcAnnotationObject(std::shared_ptr<grpc::Channel> ch);

    std::unique_ptr<dp::service::annotation::DpAnnotationService::Stub>
    makeAnnotationStub() const {
        return dp::service::annotation::DpAnnotationService::NewStub(channel);
    }
};
```

---

### 2c — `MLDPGrpcAnnotationPool`

**File:** `include/pool/MLDPGrpcAnnotationPool.h` (new)  
**Impl:** `src/pool/MLDPGrpcAnnotationPool.cpp` (new)

Mirrors `MLDPGrpcQueryPool` exactly — same acquire/release/grow logic, same metric label pattern (`{"pool", "annotation"}`), same `IObjectPool<T>` interface. Only differences:

- Template parameter: `IObjectPool<MLDPGrpcAnnotationObject>` instead of `IObjectPool<MLDPGrpcObject>`
- Config type: `MLDPGrpcAnnotationPoolConfig`
- `createChannel()` uses `config_.annotationUrl()` and creates `MLDPGrpcAnnotationObject`
- No provider registration (annotation service has no `registerProvider` RPC)

```cpp
class MLDPGrpcAnnotationPool
    : public IObjectPool<MLDPGrpcAnnotationObject>
    , public std::enable_shared_from_this<MLDPGrpcAnnotationPool>
{
public:
    using Ptr = std::shared_ptr<MLDPGrpcAnnotationPool>;
    static Ptr create(const MLDPGrpcAnnotationPoolConfig& config,
                      std::shared_ptr<metrics::Metrics> metrics = nullptr);

    PooledHandle<MLDPGrpcAnnotationObject> acquire() override;
    void    release(const ObjectShrdPtr& obj) override;
    std::size_t available() const override;
    std::size_t size() const;

private:
    // ... same Item/mutex/cv/vector structure as MLDPGrpcQueryPool
    std::shared_ptr<MLDPGrpcAnnotationObject> createChannel();
};
```

---

## Step 3 — Introduce variant payload types in `IDataBus.h`

**File:** `include/util/bus/IDataBus.h`

Replace the current flat `EventBatchStruct` with a payload-variant design.

### New types (add before `EventBatchStruct`):

```cpp
// Existing time-series payload — moved from EventBatchStruct fields
struct TimeSeriesPayload {
    std::vector<util::bus::DataBatch> frames;
    bool end_of_batch_group{false};
    bool is_tabular{false};
};

// New annotation payload — one entry per PV
struct PvMetadataEntry {
    std::optional<std::vector<std::string>>      aliases;
    std::optional<std::vector<std::string>>      tags;
    std::unordered_map<std::string, std::string> attributes;  // key → value
    std::optional<std::string>                   description;
    std::optional<std::string>                   modified_by;
};
// key = canonical PV name
using PvMetadataPayload = std::unordered_map<std::string, PvMetadataEntry>;

using BatchPayload = std::variant<TimeSeriesPayload, PvMetadataPayload>;
```

### Updated `EventBatchStruct`:

```cpp
struct EventBatchStruct {
    std::string                                reader_name;
    std::string                                root_source;  // PV name (single-PV) or reader name (multi-PV batch)
    std::unordered_map<std::string,std::string> metadata;   // static k/v from reader config (replaces tags vector)
    BatchPayload                               payload;      // variant: TimeSeriesPayload or PvMetadataPayload
};
```

`tags` vector removed — never written anywhere; replaced by `metadata` map, which also fulfills the `static_metadata_` plan from `docs/plans/static-metadata-readers.md` (subsumed by this field).

### Convenience helpers (add to `IDataBus.h` or new `BatchPayloadHelpers.h`):

```cpp
inline bool isTimeSeries(const EventBatchStruct& b) {
    return std::holds_alternative<TimeSeriesPayload>(b.payload);
}
inline bool isPvMetadata(const EventBatchStruct& b) {
    return std::holds_alternative<PvMetadataPayload>(b.payload);
}
inline const TimeSeriesPayload& asTimeSeries(const EventBatchStruct& b) {
    return std::get<TimeSeriesPayload>(b.payload);
}
inline const PvMetadataPayload& asPvMetadata(const EventBatchStruct& b) {
    return std::get<PvMetadataPayload>(b.payload);
}
```

---

## Step 4 — Migrate existing readers to `TimeSeriesPayload`

All three existing readers (`EpicsPVXSReader`, `EpicsBaseReader`, `EpicsArchiverReader`) currently build `EventBatchStruct` by setting `frames`, `end_of_batch_group`, `is_tabular` directly.

**Migration pattern** (same change in each reader's push site):

```cpp
// Before:
EventBatchStruct batch;
batch.frames = std::move(data_frames);
batch.end_of_batch_group = true;
batch.is_tabular = false;
bus_->push(std::move(batch));

// After:
EventBatchStruct batch;
batch.root_source = pv_name;
batch.reader_name = name();
batch.payload = TimeSeriesPayload{
    .frames             = std::move(data_frames),
    .end_of_batch_group = true,
    .is_tabular         = false,
};
bus_->push(std::move(batch));
```

**Affected files:**
- `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`
- `src/reader/impl/epics/base/EpicsBaseReader.cpp`
- `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp`

Search all readers for `batch.frames` / `batch.end_of_batch_group` / `batch.is_tabular` assignment sites.

---

## Step 5 — Migrate existing writers to read from `TimeSeriesPayload`

**MLDPWriter** (`src/writer/mldp/MLDPWriter.cpp`) and **HDF5WriterBase** (`src/writer/hdf5/HDF5WriterBase.cpp`) both currently access `batch.frames`, `batch.end_of_batch_group`, `batch.is_tabular`.

**Migration pattern:**

```cpp
bool MLDPWriter::push(EventBatch batch) noexcept {
    if (!isTimeSeries(batch)) return true;  // wrong payload type — accept+discard
    const auto& ts = asTimeSeries(batch);
    // existing logic uses ts.frames, ts.end_of_batch_group, ts.is_tabular
}
```

Same pattern for HDF5 writers. `return true` on wrong payload is intentional — do not penalise the reader for routing misconfiguration; rely on RouteTable to prevent it.

**Affected files:**
- `src/writer/mldp/MLDPWriter.cpp` — `push()` and `buildRequest()`
- `src/writer/hdf5/HDF5WriterBase.cpp` — `push()` and queue consumer

---

## Step 6 — Add `acceptsPayload()` to `IWriter`

**File:** `include/writer/IWriter.h`

```cpp
// Default: accept all (preserves backward compatibility with existing writers).
virtual bool acceptsPayload(const BatchPayload& payload) const noexcept { return true; }
```

Override in `MLDPWriter` and HDF5 writers:
```cpp
bool acceptsPayload(const BatchPayload& p) const noexcept override {
    return std::holds_alternative<TimeSeriesPayload>(p);
}
```

Override in `MLDPAnnotationWriter`:
```cpp
bool acceptsPayload(const BatchPayload& p) const noexcept override {
    return std::holds_alternative<PvMetadataPayload>(p);
}
```

**Controller integration** (`src/controller/MLDPPVXSController.cpp`, `push()` method, ~line 186):

```cpp
if (!writer->acceptsPayload(batch.payload)) continue;
```

Belt-and-suspenders: RouteTable is the primary affinity mechanism; `acceptsPayload` prevents silent data corruption if routing is misconfigured.

---

## Step 7 — New `PvMetadataReader`

### Config struct

**File:** `include/reader/impl/pv_metadata/PvMetadataReaderConfig.h` (new)

```cpp
struct PvMetadataReaderConfig {
    std::string name;

    struct PVEntry {
        std::string pvName;                               // canonical name (= root_source per batch)
        std::optional<std::vector<std::string>> aliases;
        std::optional<std::vector<std::string>> tags;
        std::unordered_map<std::string, std::string> attributes;
        std::optional<std::string> description;
        std::optional<std::string> modified_by;
    };
    std::vector<PVEntry> pvs;

    static PvMetadataReaderConfig parse(const config::Config& node);
};
```

**YAML schema:**
```yaml
reader:
  - pv-metadata:
      - name: pv_meta_reader
        pvs:
          - name: KLYS:LI22:21:PHAS
            aliases: [LI22_21_KLYS_PHAS]
            tags: [klystron, linac]
            attributes:
              sector: "22"
              subsystem: rf
            description: "Klystron LI22-21 phase"
          - name: BPMS:LI21:201:X
            tags: [bpm]
            attributes:
              sector: "21"
```

### Reader class

**File:** `include/reader/impl/pv_metadata/PvMetadataReader.h` (new)

```cpp
class PvMetadataReader : public Reader {
public:
    PvMetadataReader(std::shared_ptr<util::bus::IDataBus> bus,
                     std::shared_ptr<metrics::Metrics> metrics,
                     const config::Config& cfg);
    std::string name() const override { return config_.name; }
    void start();  // fires publish() once at startup — no background thread needed
private:
    void publish();
    PvMetadataReaderConfig config_;
};
```

**`publish()` logic — one batch per PV** (recommended over single multi-PV batch):
- `root_source` = `entry.pvName` → enables `RouteTable::acceptsSource` glob filtering per PV
- `reader_name` = `config_.name`
- `payload` = `PvMetadataPayload{ {entry.pvName, PvMetadataEntry{...}} }`

**Registration** (`src/reader/impl/pv_metadata/PvMetadataReader.cpp`):
```cpp
REGISTER_READER("pv-metadata", PvMetadataReader)
```

---

## Step 8 — New `MLDPAnnotationWriter`

### Config struct

**File:** `include/writer/mldp_annotation/MLDPAnnotationWriterConfig.h` (new)

```cpp
struct MLDPAnnotationWriterConfig {
    std::string name;
    util::pool::MLDPGrpcAnnotationPoolConfig poolConfig;  // dedicated annotation pool config
    int deadlineSeconds{10};

    static MLDPAnnotationWriterConfig parse(const config::Config& node);
};
```

**YAML schema:**
```yaml
writer:
  mldp-annotation:
    - name: annotation_writer
      mldp-annotation-pool:
        annotation-url: "localhost:50052"
        min-conn: 1
        max-conn: 4
        credentials: none
      deadline-seconds: 10
```

### Writer class

**File:** `include/writer/mldp_annotation/MLDPAnnotationWriter.h` (new)

```cpp
class MLDPAnnotationWriter : public IWriter {
public:
    explicit MLDPAnnotationWriter(const config::Config& root,
                                  std::shared_ptr<metrics::Metrics> metrics = nullptr);
    std::string name() const override;
    void start() override;
    bool push(EventBatch batch) noexcept override;
    void stop() noexcept override;
    bool acceptsPayload(const BatchPayload& p) const noexcept override {
        return std::holds_alternative<PvMetadataPayload>(p);
    }
private:
    void savePvMetadata(const std::string& pvName, const PvMetadataEntry& entry);
    MLDPAnnotationWriterConfig                 config_;
    std::shared_ptr<MLDPGrpcAnnotationPool>    pool_;  // dedicated annotation pool
};
```

**`push()` logic:**
```cpp
bool MLDPAnnotationWriter::push(EventBatch batch) noexcept {
    if (!std::holds_alternative<PvMetadataPayload>(batch.payload)) return true;
    const auto& meta = std::get<PvMetadataPayload>(batch.payload);
    for (const auto& [pvName, entry] : meta) {
        savePvMetadata(pvName, entry);
    }
    return true;
}
```

**`savePvMetadata()` logic:**
```cpp
void MLDPAnnotationWriter::savePvMetadata(const std::string& pvName,
                                           const PvMetadataEntry& entry) {
    auto handle = pool_->acquire();          // PooledHandle<MLDPGrpcAnnotationObject>
    if (!handle->stub)
        handle->stub = handle->makeAnnotationStub();

    dp::service::annotation::SavePvMetadataRequest req;
    req.set_pvname(pvName);
    if (entry.aliases) for (auto& a : *entry.aliases) req.add_aliases(a);
    if (entry.tags)    for (auto& t : *entry.tags)    req.add_tags(t);
    for (auto& [k, v] : entry.attributes) {
        auto* attr = req.add_attributes();
        attr->set_name(k);
        attr->set_value(v);
    }
    if (entry.description) req.set_description(*entry.description);
    if (entry.modified_by) req.set_modifiedby(*entry.modified_by);

    dp::service::annotation::SavePvMetadataResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(config_.deadlineSeconds));
    auto status = handle->stub->savePvMetadata(&ctx, req, &resp);
    if (!status.ok()) { /* log gRPC transport error */ return; }
    if (resp.result_case() == SavePvMetadataResponse::kExceptionalResult) { /* log service error */ }
}
```

**Registration** (`src/writer/mldp_annotation/MLDPAnnotationWriter.cpp`):
```cpp
REGISTER_WRITER("mldp-annotation", MLDPAnnotationWriter)
```

---

## Step 9 — Full YAML config example

```yaml
writer:
  mldp:
    - name: mldp_ingestion
      thread-pool: 2
      mldp-pool:
        ingestion-url: "localhost:50051"
        query-url: "localhost:50052"

  mldp-annotation:
    - name: mldp_annotation
      mldp-annotation-pool:
        annotation-url: "localhost:50052"
        min-conn: 1
        max-conn: 4
        credentials: none
      deadline-seconds: 10

reader:
  - epics-pvxs:
      - name: pvxs_reader
        thread-pool: 2
        pvs:
          - name: "BPMS:LI21:201:X"
          - name: "KLYS:LI22:21:PHAS"

  - pv-metadata:
      - name: pv_meta_reader
        pvs:
          - name: KLYS:LI22:21:PHAS
            tags: [klystron, linac]
            attributes:
              sector: "22"
          - name: BPMS:LI21:201:X
            tags: [bpm]
            attributes:
              sector: "21"

routing:
  - writer: mldp_ingestion
    from: [pvxs_reader]
  - writer: mldp_annotation
    from: [pv_meta_reader]
```

---

## Implementation Order (safe build sequence)

| Step | File(s) | Change |
|------|---------|--------|
| 1 | `CMakeLists.txt` | Add `annotation.proto` to `PROTO_FILES` |
| 2a | New: `include/pool/MLDPGrpcAnnotationPoolConfig.h` + `.cpp` | Lightweight pool config (`annotation-url`, min/max conn, credentials) |
| 2b | New: `include/pool/MLDPGrpcAnnotationPool.h` + `src/pool/MLDPGrpcAnnotationPool.cpp` | `MLDPGrpcAnnotationObject` + `MLDPGrpcAnnotationPool` — mirrors query pool pattern |
| 3 | `include/util/bus/IDataBus.h` | Add `TimeSeriesPayload`, `PvMetadataPayload`, `BatchPayload`, update `EventBatchStruct` |
| 4 | 3 reader `.cpp` files | Wrap existing frames in `TimeSeriesPayload{}` |
| 5 | `MLDPWriter.cpp`, `HDF5WriterBase.cpp` | Use `asTimeSeries()` helper; guard with `isTimeSeries()` |
| 6 | `include/writer/IWriter.h` | Add `acceptsPayload()` virtual (default `return true`) |
| 7 | `src/controller/MLDPPVXSController.cpp` | Add `acceptsPayload` gate in `push()` fan-out |
| 8 | New: `PvMetadataReaderConfig.h`, `PvMetadataReader.h/.cpp` | New reader + REGISTER_READER |
| 9 | New: `MLDPAnnotationWriterConfig.h`, `MLDPAnnotationWriter.h/.cpp` | New writer using `MLDPGrpcAnnotationPool` + REGISTER_WRITER |
| 10 | `docs/plans/static-metadata-readers.md` | Mark superseded by `EventBatchStruct::metadata` map |

Each step compiles independently — the build stays green after every step.

---

## Relation to `static_metadata_` plan (commit 9ff4252)

The planned `static_metadata_` feature (reader-level k/v → `EventBatch::metadata`) is **orthogonal** to `PvMetadataPayload`:

| Mechanism | Destination | Purpose |
|-----------|-------------|---------|
| `EventBatchStruct::metadata` map | Time-series archive via `ColumnMetadata.attributes` | Ingestion-time lineage stamped on every column |
| `PvMetadataPayload` | Annotation service (`DpAnnotationService`) | User-facing durable PV metadata record |

Both are useful and non-redundant. `PvMetadataReader` can read the same YAML metadata fields as the static-metadata feature, letting operators define metadata once and have it flow both to the archive and to the annotation service.

---

## Verification

1. **Build clean**: `cmake --build build` — zero compile errors after migration
2. **Existing tests pass**: `ctest --test-dir build` — no regressions in `MLDPWriter` integration test
3. **Annotation writer test**: start driver with example config; observe `savePvMetadata` RPC in server log; call `getPvMetadata("KLYS:LI22:21:PHAS")` and verify fields match YAML
4. **Route isolation**: configure `pvxs_reader` with no route to `mldp_annotation` — annotation writer receives zero batches
5. **Payload defence unit test**: push `TimeSeriesPayload` batch directly to `MLDPAnnotationWriter::push()` — must return `true`, call zero gRPC stubs
