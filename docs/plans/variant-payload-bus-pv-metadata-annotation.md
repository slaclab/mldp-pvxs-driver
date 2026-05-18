# Plan: Variant-Payload Bus + MLDP Annotation Writer

## Context

The driver currently has a single `EventBatchStruct` that carries only time-series data (timestamped column frames). A new requirement calls for an MLDP annotation writer (calls `DpAnnotationService.savePvMetadata`). This new component carries a structurally different payload — no timestamps, no column frames — so the bus needs to accommodate multiple payload types without multiplying bus instances.

The solution uses `std::variant<TimeSeriesPayload, SourceMetadataPayload>` inside `EventBatchStruct`, preserves 100% backward compatibility (existing readers/writers touch only `TimeSeriesPayload`), and reuses the existing `RouteTable` for reader→writer affinity. No new bus, no new controller.

The `SourceMetadataReader` that produces `SourceMetadataPayload` is a **separate follow-on task** (not in scope here). The annotation writer is wired and ready to consume once the reader is added.

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

Generates `annotation.pb.h` / `annotation.grpc.pb.h` under the existing `lib${PROJECT_NAME}_proto` target. No other CMake change needed.

---

## Step 2 — New annotation-specific pool

`MLDPGrpcPoolConfig` carries ingestion/query URLs and provider registration — none needed by the annotation writer. Introduce a dedicated lightweight annotation pool stack.

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

**YAML schema:**
```yaml
mldp-annotation-pool:
  annotation-url: "localhost:50052"
  min-conn: 1
  max-conn: 4
  credentials: none
```

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

### 2c — `MLDPGrpcAnnotationPool`

**File:** `include/pool/MLDPGrpcAnnotationPool.h` (new)
**Impl:** `src/pool/MLDPGrpcAnnotationPool.cpp` (new)

Mirrors `MLDPGrpcQueryPool` — same acquire/release/grow logic, same metric label pattern (`{"pool", "annotation"}`), same `IObjectPool<T>` interface. Differences:

- Template parameter: `IObjectPool<MLDPGrpcAnnotationObject>`
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
    void        release(const ObjectShrdPtr& obj) override;
    std::size_t available() const override;
    std::size_t size() const;

private:
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

// Source metadata payload — one entry per source
struct SourceMetadataEntry {
    std::optional<std::vector<std::string>>      aliases;
    std::optional<std::vector<std::string>>      tags;
    std::unordered_map<std::string, std::string> attributes;  // key → value
    std::optional<std::string>                   description;
    std::optional<std::string>                   modified_by;
};
// key = source_name (internal canonical identifier — not proto field name)
using SourceMetadataPayload = std::unordered_map<std::string, SourceMetadataEntry>;

using BatchPayload = std::variant<TimeSeriesPayload, SourceMetadataPayload>;
```

### Updated `EventBatchStruct`:

```cpp
struct EventBatchStruct {
    std::string                                 reader_name;
    std::string                                 root_source;  // source name or reader name (multi-source batch)
    std::unordered_map<std::string,std::string> metadata;     // static k/v from reader config
    BatchPayload                                payload;      // variant: TimeSeriesPayload or SourceMetadataPayload
};
```

`tags` vector removed — never written anywhere; replaced by `metadata` map (subsumed from `docs/plans/static-metadata-readers.md`).

### Convenience helpers (add to `IDataBus.h` or new `BatchPayloadHelpers.h`):

```cpp
inline bool isTimeSeries(const EventBatchStruct& b) {
    return std::holds_alternative<TimeSeriesPayload>(b.payload);
}
inline bool isSourceMetadata(const EventBatchStruct& b) {
    return std::holds_alternative<SourceMetadataPayload>(b.payload);
}
inline const TimeSeriesPayload& asTimeSeries(const EventBatchStruct& b) {
    return std::get<TimeSeriesPayload>(b.payload);
}
inline const SourceMetadataPayload& asSourceMetadata(const EventBatchStruct& b) {
    return std::get<SourceMetadataPayload>(b.payload);
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

Search for `batch.frames` / `batch.end_of_batch_group` / `batch.is_tabular` assignment sites.

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

Same pattern for HDF5 writers. `return true` on wrong payload is intentional — do not penalise the reader for routing misconfiguration.

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
    return std::holds_alternative<SourceMetadataPayload>(p);
}
```

**Controller integration** (`src/controller/MLDPPVXSController.cpp`, `push()` method, ~line 186):

```cpp
if (!writer->acceptsPayload(batch.payload)) continue;
```

Belt-and-suspenders: RouteTable is the primary affinity mechanism; `acceptsPayload` prevents silent data corruption if routing is misconfigured.

---

## Step 7 — New `MLDPAnnotationWriter`

### Config struct

**File:** `include/writer/mldp_annotation/MLDPAnnotationWriterConfig.h` (new)

```cpp
struct MLDPAnnotationWriterConfig {
    std::string name;
    util::pool::MLDPGrpcAnnotationPoolConfig poolConfig;
    int deadlineSeconds{10};
    int threadPool{2};

    static MLDPAnnotationWriterConfig parse(const config::Config& node);
};
```

**YAML schema:**
```yaml
writer:
  mldp-annotation:
    - name: annotation_writer
      thread-pool: 2          # worker threads; set max-conn >= thread-pool to avoid starvation
      mldp-annotation-pool:
        annotation-url: "localhost:50052"
        min-conn: 2           # >= thread-pool
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
    void start() override;   // launches threadPool worker threads
    bool push(EventBatch batch) noexcept override;
    void stop() noexcept override;
    bool acceptsPayload(const BatchPayload& p) const noexcept override {
        return std::holds_alternative<SourceMetadataPayload>(p);
    }
private:
    struct WorkItem {
        std::string         source_name;
        SourceMetadataEntry entry;
    };

    void workerLoop();
    void saveSourceMetadata(const std::string& sourceName, const SourceMetadataEntry& entry);

    MLDPAnnotationWriterConfig              config_;
    std::shared_ptr<MLDPGrpcAnnotationPool> pool_;

    std::queue<WorkItem>      work_queue_;
    std::mutex                queue_mutex_;
    std::condition_variable   queue_cv_;
    std::vector<std::thread>  workers_;
    std::atomic<bool>         stop_{false};
};
```

**`start()` logic:**
```cpp
void MLDPAnnotationWriter::start() {
    pool_ = MLDPGrpcAnnotationPool::create(config_.poolConfig, metrics_);
    workers_.reserve(config_.threadPool);
    for (int i = 0; i < config_.threadPool; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}
```

**`stop()` logic:**
```cpp
void MLDPAnnotationWriter::stop() noexcept {
    stop_.store(true);
    queue_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}
```

**`push()` logic — enqueues work items, does not block:**
```cpp
bool MLDPAnnotationWriter::push(EventBatch batch) noexcept {
    if (!std::holds_alternative<SourceMetadataPayload>(batch.payload)) return true;
    const auto& meta = std::get<SourceMetadataPayload>(batch.payload);
    {
        std::lock_guard lock(queue_mutex_);
        for (const auto& [sourceName, entry] : meta)
            work_queue_.push({sourceName, entry});
    }
    queue_cv_.notify_all();
    return true;
}
```

**`workerLoop()` — each thread pulls items and calls one RPC per pool connection:**
```cpp
void MLDPAnnotationWriter::workerLoop() {
    while (true) {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return stop_ || !work_queue_.empty(); });
        if (work_queue_.empty()) return;  // stop_ set, queue drained
        auto item = std::move(work_queue_.front());
        work_queue_.pop();
        lock.unlock();
        saveSourceMetadata(item.source_name, item.entry);
    }
}
```

**`saveSourceMetadata()` logic:**
```cpp
void MLDPAnnotationWriter::saveSourceMetadata(const std::string& sourceName,
                                               const SourceMetadataEntry& entry) {
    auto handle = pool_->acquire();
    if (!handle->stub)
        handle->stub = handle->makeAnnotationStub();

    dp::service::annotation::SavePvMetadataRequest req;
    req.set_pvname(sourceName);   // proto field name unchanged
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

## Step 8 — Full YAML config example

```yaml
writer:
  mldp:
    - name: mldp_ingestion
      thread-pool: 2
      mldp-pool:
        ingestion-url: "localhost:50051"
        query-url: "localhost:50052"

  mldp-annotation:
    - name: annotation_writer
      thread-pool: 2
      mldp-annotation-pool:
        annotation-url: "localhost:50052"
        min-conn: 2
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

  # source-metadata reader is a follow-on task — annotation writer ready to consume when added

routing:
  - writer: mldp_ingestion
    from: [pvxs_reader]
```

---

## Implementation Order (safe build sequence)

| Step | File(s) | Change |
|------|---------|--------|
| 1 | `CMakeLists.txt` | Add `annotation.proto` to `PROTO_FILES` |
| 2a | New: `include/pool/MLDPGrpcAnnotationPoolConfig.h` + `.cpp` | Lightweight pool config |
| 2b | New: `include/pool/MLDPGrpcAnnotationPool.h` + `src/pool/MLDPGrpcAnnotationPool.cpp` | `MLDPGrpcAnnotationObject` + `MLDPGrpcAnnotationPool` |
| 3 | `include/util/bus/IDataBus.h` | Add `TimeSeriesPayload`, `SourceMetadataPayload`, `BatchPayload`, update `EventBatchStruct` |
| 4 | 3 reader `.cpp` files | Wrap existing frames in `TimeSeriesPayload{}` |
| 5 | `MLDPWriter.cpp`, `HDF5WriterBase.cpp` | Use `asTimeSeries()` helper; guard with `isTimeSeries()` |
| 6 | `include/writer/IWriter.h` | Add `acceptsPayload()` virtual (default `return true`) |
| 7 | `src/controller/MLDPPVXSController.cpp` | Add `acceptsPayload` gate in `push()` fan-out |
| 8 | New: `MLDPAnnotationWriterConfig.h`, `MLDPAnnotationWriter.h/.cpp` | New writer + REGISTER_WRITER |

Each step compiles independently — the build stays green after every step.

---

## Relation to `static_metadata_` plan (commit 9ff4252)

The planned `static_metadata_` feature (reader-level k/v → `EventBatch::metadata`) is **orthogonal** to `SourceMetadataPayload`:

| Mechanism | Destination | Purpose |
|-----------|-------------|---------|
| `EventBatchStruct::metadata` map | Time-series archive via `ColumnMetadata.attributes` | Ingestion-time lineage stamped on every column |
| `SourceMetadataPayload` | Annotation service (`DpAnnotationService`) | User-facing durable source metadata record |

---

## Verification

1. **Build clean**: `cmake --build build` — zero compile errors after migration
2. **Existing tests pass**: `ctest --test-dir build` — no regressions in `MLDPWriter` integration test
3. **Annotation writer test**: start driver with example config; observe `savePvMetadata` RPC in server log
4. **Route isolation**: configure `pvxs_reader` with no route to `annotation_writer` — annotation writer receives zero batches
5. **Payload defence**: push `TimeSeriesPayload` batch directly to `MLDPAnnotationWriter::push()` — must return `true`, call zero gRPC stubs
