# Multi-Endpoint Writer System — Implementation Plan

> **Status tracking:** Each checkbox below represents one discrete unit of work.
> Check them off as you complete them.  The implementation order in
> [Section 5](#5-implementation-order) must be respected because of header
> dependencies.

---

## 1. Context & Goal

The driver currently has a single hard-wired output path:

```
Readers → push(EventBatch) → MLDPPVXSController → gRPC → MLDP
```

This plan extends the output layer with:

1. **A pluggable `IWriter` interface** (under `include/writer/`) so any number of output
   destinations can be added without changing existing reader or controller logic.
   Mirrors the existing `include/reader/` pattern exactly.
2. **An HDF5 file writer** that stores data per-source as a table (all PVs as
   columns, timestamps as rows), with per-source file rotation on age and size.
   Uses a dedicated `HDF5FilePool` analogous to the existing `MLDPGrpcIngestionePool`.
3. **YAML configuration** (`writer:` block) to select which writers are active
   (one, both, or any future ones).
4. **Full backward compatibility**: configs with no `writer:` block continue to work identically.

Project structure after implementation:

```
include/reader/   ← unchanged (sources)
include/writer/   ← NEW (destinations)
include/pool/     ← unchanged (shared by both readers and writers)
```

---

## 2. Design Decisions

| Decision | Choice |
|----------|--------|
| Folder naming | `include/writer/` + `src/writer/` — mirrors `reader/` |
| HDF5 file granularity | One file per `root_source`; file name = `<sanitised_source>_<ISO8601>.h5` |
| HDF5 internal layout | Flat datasets at root: `timestamps` (int64 ns-epoch) + one dataset per DataFrame column |
| HDF5 file pool | In `include/writer/hdf5/HDF5FilePool.h` — mirrors `MLDPGrpcIngestionePool` from `include/pool/` |
| Multi-writer failure | **Best-effort fan-out**: each writer is independent; `push()` returns `true` if any writer accepts |
| File rotation trigger | Age **or** size, both configurable via YAML |
| Build-time optionality | `MLDP_PVXS_ENABLE_HDF5` CMake option (default ON); runtime error if YAML requests HDF5 but built with OFF |
| Query path | Unchanged — `querySourcesInfo`/`querySourcesData` use `MLDPGrpcQueryPool` from `pool/` directly |
| Pool ownership | `pool/` stays at top level, shared — `MLDPGrpcWriter` uses `MLDPGrpcIngestionePool`, `HDF5Writer` uses `HDF5FilePool` |

---

## 3. Files Overview

### 3.1 New Files

| File | Role |
|------|------|
| `include/writer/IWriter.h` | Pure abstract interface all writers implement |
| `include/writer/WriterConfig.h` | Top-level `writer:` YAML config struct |
| `src/writer/WriterConfig.cpp` | Parser for the `writer:` block |
| `include/writer/grpc/MLDPGrpcWriter.h` | gRPC writer header (logic moved from controller) |
| `src/writer/grpc/MLDPGrpcWriter.cpp` | gRPC writer implementation |
| `include/writer/grpc/MLDPGrpcWriterConfig.h` | gRPC writer config (currently lives in `MLDPPVXSControllerConfig`) |
| `src/writer/grpc/MLDPGrpcWriterConfig.cpp` | gRPC writer config parser |
| `include/writer/hdf5/HDF5WriterConfig.h` | HDF5-specific config struct |
| `src/writer/hdf5/HDF5WriterConfig.cpp` | HDF5 config parser |
| `include/writer/hdf5/HDF5FilePool.h` | Per-source HDF5 file-handle pool header |
| `src/writer/hdf5/HDF5FilePool.cpp` | Pool implementation (open/rotate/close) |
| `include/writer/hdf5/HDF5Writer.h` | HDF5 writer header |
| `src/writer/hdf5/HDF5Writer.cpp` | HDF5 writer thread + flush thread |

### 3.2 Modified Files

| File | Change |
|------|--------|
| `include/controller/MLDPPVXSControllerConfig.h` | Add `WriterConfig` member; `mldp-pool` becomes optional |
| `src/controller/MLDPPVXSControllerConfig.cpp` | Add `parseWriter()`; `parsePool()` becomes conditional |
| `include/controller/MLDPPVXSController.h` | Replace gRPC-specific private members with `vector<IWriterUPtr>` |
| `src/controller/MLDPPVXSController.cpp` | Constructor builds & starts writers; `push()` fans out; remove `workerLoop()`/`buildRequest()` |
| `CMakeLists.txt` | Add `find_package(HDF5)`, conditional source files and link targets |

---

## 4. Detailed Specifications

### 4.1 `IWriter`
**File:** `include/writer/IWriter.h`

Namespace: `mldp_pvxs_driver::writer`

```cpp
class IWriter {
public:
    virtual ~IWriter() = default;
    virtual std::string name() const = 0;
    virtual void start() = 0;                          // throws std::runtime_error on hard failure
    virtual bool push(util::bus::IDataBus::EventBatch batch) noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual bool isHealthy() const noexcept { return true; }
};
using IWriterUPtr = std::unique_ptr<IWriter>;
```

- `push()` is `noexcept`: no writer exception may unwind through the fan-out loop.
- Uses the existing `EventBatch` type from `include/util/bus/IDataBus.h`.

---

### 4.2 `HDF5WriterConfig`
**Files:** `include/writer/hdf5/HDF5WriterConfig.h`, `src/writer/hdf5/HDF5WriterConfig.cpp`

```cpp
struct HDF5WriterConfig {
    std::string               basePath;            // required — writer.hdf5.base-path
    std::chrono::seconds      maxFileAge{3600};    // writer.hdf5.max-file-age-s
    uint64_t                  maxFileSizeMB{512};  // writer.hdf5.max-file-size-mb
    std::chrono::milliseconds flushInterval{1000}; // writer.hdf5.flush-interval-ms
    int                       compressionLevel{0}; // writer.hdf5.compression-level (0-9)

    static HDF5WriterConfig parse(const config::Config& node);
};
```

Validation: `basePath` non-empty; `maxFileAge > 0`; `maxFileSizeMB > 0`;
`compressionLevel` in `[0, 9]`.

---

### 4.3 `MLDPGrpcWriterConfig`
**Files:** `include/writer/grpc/MLDPGrpcWriterConfig.h`, `src/writer/grpc/MLDPGrpcWriterConfig.cpp`

Extracts the gRPC-specific knobs currently embedded in `MLDPPVXSControllerConfig`:

```cpp
struct MLDPGrpcWriterConfig {
    util::pool::MLDPGrpcPoolConfig poolConfig;
    int                            threadPoolSize{1};
    std::size_t                    streamMaxBytes{2 * 1024 * 1024};
    std::chrono::milliseconds      streamMaxAge{200};

    static MLDPGrpcWriterConfig parse(const config::Config& writerGrpcNode,
                                      const config::Config& root);
    // reads pool from root's mldp-pool key; thread/stream knobs from writerGrpcNode
};
```

---

### 4.4 `WriterConfig`
**Files:** `include/writer/WriterConfig.h`, `src/writer/WriterConfig.cpp`

```cpp
struct WriterConfig {
    bool grpcEnabled = true;    // writer.grpc.enabled  (default true)
    bool hdf5Enabled = false;   // writer.hdf5.enabled  (default false)

    std::optional<MLDPGrpcWriterConfig> grpcConfig;
    std::optional<HDF5WriterConfig>     hdf5Config;

    static WriterConfig parse(const config::Config& writerNode,
                              const config::Config& root);
    bool anyEnabled() const noexcept { return grpcEnabled || hdf5Enabled; }
};
```

`parse()` rules:
- If `writer.grpc` present → read `enabled` (default `true`); if true, parse `MLDPGrpcWriterConfig`.
- If `writer.hdf5` present and `enabled: true` → call `HDF5WriterConfig::parse()`.
- If `!anyEnabled()` → throw `std::invalid_argument`.
- If `hdf5.enabled=true` but built without `MLDP_PVXS_HDF5_ENABLED` → throw a clear error.

---

### 4.5 Changes to `MLDPPVXSControllerConfig`

**`MLDPPVXSControllerConfig.h`:**
- Add `#include "writer/WriterConfig.h"`.
- Change `MLDPGrpcPoolConfig pool_` → `std::optional<util::pool::MLDPGrpcPoolConfig> pool_`.
- Add member `writer::WriterConfig writerConfig_`.
- Add accessor `const writer::WriterConfig& writerConfig() const`.

**`MLDPPVXSControllerConfig.cpp`** — new `parse()` call sequence:
```
parseThreadPool(root)    // unchanged
parseStreamLimits(root)  // unchanged (values now forwarded to MLDPGrpcWriterConfig)
parseWriter(root)        // NEW — must run before parsePool
parsePool(root)          // CHANGED: early-return when grpcEnabled=false
parseReaders(root)       // unchanged
parseMetrics(root)       // unchanged
```

New `parseWriter()`:
```cpp
void MLDPPVXSControllerConfig::parseWriter(const config::Config& root) {
    if (root.hasChild("writer"))
        writerConfig_ = writer::WriterConfig::parse(
                            root.subConfig("writer").front(), root);
    // else: default = grpc enabled, hdf5 disabled (backward compat)
}
```

Modified `parsePool()`:
```cpp
void MLDPPVXSControllerConfig::parsePool(const config::Config& root) {
    if (!writerConfig_.grpcEnabled) return;    // ← new guard
    if (!root.hasChild(MldpPoolKey))
        throw Error("writer.grpc is enabled but 'mldp-pool' block is missing");
    pool_ = util::pool::MLDPGrpcPoolConfig(root.subConfig(MldpPoolKey).front());
}
```

---

### 4.6 `HDF5FilePool`
**Files:** `include/writer/hdf5/HDF5FilePool.h`, `src/writer/hdf5/HDF5FilePool.cpp`

Mirrors `MLDPGrpcIngestionePool` from `include/pool/` — mutex-protected map of
`source_name → shared_ptr<FileEntry>`.

```cpp
struct FileEntry {
    H5::H5File                            file;
    std::filesystem::path                 path;
    std::chrono::steady_clock::time_point openedAt;
    uint64_t                              bytesWritten{0};
};

class HDF5FilePool {
public:
    explicit HDF5FilePool(HDF5WriterConfig config);
    std::shared_ptr<FileEntry> acquire(const std::string& sourceName);
    void recordWrite(const std::string& sourceName, uint64_t bytes);
    void flushAll() noexcept;
    void closeAll() noexcept;
};
```

Key behaviours:
- `acquire()`: mutex-protected lookup → create-or-rotate.
- Mutex held **only** during map lookup/rotation, **not** during HDF5 I/O.
- Workers hold `shared_ptr<FileEntry>`: in-flight writes survive a concurrent rotation.
- File name pattern: `<base-path>/<safe_source>_<YYYYMMDDTHHMMSSz>.h5`
  (`:` and other non-safe chars replaced with `_`).
- `needsRotation()`: `age >= maxFileAge || bytesWritten >= maxFileSizeMB * 1'048'576`.

---

### 4.7 `HDF5Writer`
**Files:** `include/writer/hdf5/HDF5Writer.h`, `src/writer/hdf5/HDF5Writer.cpp`

Architecture:
- `push()` enqueues `EventBatch` into a bounded MPSC queue (default cap 8192).
  Returns `false` immediately (does not block) when full.
- **Writer thread** drains queue → `writeBatch()` → `HDF5FilePool::acquire()` → `appendFrame()`.
- **Flush thread** calls `HDF5FilePool::flushAll()` every `flush-interval-ms`.

HDF5 dataset layout inside each source file:
```
/ (root)
├── timestamps          int64   nanoseconds-since-epoch    shape=(N,) unlimited chunked
├── <col_name_0>        dtype   matches DataFrame column   shape=(N,) unlimited chunked
├── <col_name_1>        …
└── …
```

`ensureDataset()`: uses `H5File::nameExists()` → create once (chunked + optional DEFLATE),
then open on subsequent frames.

`appendFrame()`: `DataSet::extend()` + hyperslab select + `DataSet::write()` per column.

`protoTypeToH5Type()`: maps `dp::service::common::DataType` enum values:

| Proto type | H5 type |
|-----------|---------|
| INT32 | `NATIVE_INT32` |
| INT64 | `NATIVE_INT64` |
| FLOAT | `NATIVE_FLOAT` |
| DOUBLE | `NATIVE_DOUBLE` |
| BOOL | `NATIVE_HBOOL` |
| STRING | `StrType(C_S1, H5T_VARIABLE)` |

> ⚠️ **Verify the exact enum value names** against
> `${CMAKE_BINARY_DIR}/proto/common.pb.h` after proto compilation.

---

### 4.8 `MLDPGrpcWriter`
**Files:** `include/writer/grpc/MLDPGrpcWriter.h`, `src/writer/grpc/MLDPGrpcWriter.cpp`

**Migration strategy:** Cut-and-paste the existing `WorkerChannel`, `QueueItem`,
`workerLoop()`, and `buildRequest()` from `src/controller/MLDPPVXSController.cpp` verbatim.
Rename member references as needed. **No logic changes.**

Constructor:
```cpp
explicit MLDPGrpcWriter(MLDPGrpcWriterConfig config);
```

`start()`: construct `MLDPGrpcIngestionePool` (from `pool/`) and spawn worker threads.
`push() noexcept`: round-robin dispatch across channels (identical to current controller logic).
`stop() noexcept`: signal shutdown, join threads.

The `MLDPGrpcIngestionePool` is still in `include/pool/` — `MLDPGrpcWriter` depends on it
exactly as the controller does today.

---

### 4.9 Changes to `MLDPPVXSController`

**Header `MLDPPVXSController.h`:**
- Remove: `WorkerChannel`, `QueueItem`, `workerLoop`, `buildRequest`, `channels_`,
  `mldp_ingestion_pool_`, `next_channel_`, `queued_items_`.
- Add: `#include "writer/IWriter.h"` and member
  `std::vector<writer::IWriterUPtr> writers_`.
- Keep: `mldp_query_pool_`, `metrics_`, `logger_` (unchanged).

**Constructor `MLDPPVXSController.cpp`:**
```cpp
if (cfg.writerConfig().grpcEnabled)
    writers_.push_back(std::make_unique<writer::MLDPGrpcWriter>(
        cfg.writerConfig().grpcConfig.value()));

if (cfg.writerConfig().hdf5Enabled)
    writers_.push_back(std::make_unique<writer::HDF5Writer>(
        cfg.writerConfig().hdf5Config.value()));

for (auto& w : writers_) w->start();
```

**Destructor:** `for (auto& w : writers_) w->stop();`

**`push()` — best-effort fan-out:**
```cpp
bool MLDPPVXSController::push(EventBatch batch_values) {
    bool anyAccepted = false;
    for (std::size_t i = 0; i < writers_.size(); ++i) {
        bool last = (i == writers_.size() - 1);
        bool ok   = last ? writers_[i]->push(std::move(batch_values))
                         : writers_[i]->push(batch_values);  // copy all but last
        if (!ok) { /* log + metric */ }
        anyAccepted = anyAccepted || ok;
    }
    return anyAccepted;
}
```

---

### 4.10 `CMakeLists.txt`

```cmake
# ---- HDF5 (optional) ----
option(MLDP_PVXS_ENABLE_HDF5 "Build with HDF5 writer support" ON)
if(MLDP_PVXS_ENABLE_HDF5)
    find_package(HDF5 REQUIRED COMPONENTS CXX)
    message(STATUS "HDF5 ${HDF5_VERSION} found")
    add_compile_definitions(MLDP_PVXS_HDF5_ENABLED)
endif()
```

Add to the library's `target_sources()`:
```cmake
    src/writer/WriterConfig.cpp
    src/writer/grpc/MLDPGrpcWriterConfig.cpp
    src/writer/grpc/MLDPGrpcWriter.cpp
    $<$<BOOL:${MLDP_PVXS_ENABLE_HDF5}>:
        src/writer/hdf5/HDF5WriterConfig.cpp
        src/writer/hdf5/HDF5FilePool.cpp
        src/writer/hdf5/HDF5Writer.cpp
    >
```

Add to `target_link_libraries()` (private):
```cmake
    $<$<BOOL:${MLDP_PVXS_ENABLE_HDF5}>:${HDF5_CXX_LIBRARIES}>
```

Add to `target_include_directories()` (private):
```cmake
    $<$<BOOL:${MLDP_PVXS_ENABLE_HDF5}>:${HDF5_INCLUDE_DIRS}>
```

The existing `include/` public include path already covers `include/writer/`.

All HDF5 translation units wrap their entire body with:
```cpp
#ifdef MLDP_PVXS_HDF5_ENABLED
// … content …
#endif
```

---

## 5. Implementation Order

Must be followed to keep the codebase compiling at each step:

| Step | Task | Compiles after? |
|------|------|-----------------|
| 1 | Create `include/writer/IWriter.h` | ✅ (header only) |
| 2 | Create `HDF5WriterConfig.h` + `.cpp` | ✅ |
| 3 | Create `MLDPGrpcWriterConfig.h` + `.cpp` | ✅ |
| 4 | Create `WriterConfig.h` + `.cpp` | ✅ (depends on 2, 3) |
| 5 | Update `MLDPPVXSControllerConfig.h` + `.cpp` | ✅ (depends on 4) |
| 6 | Create `HDF5FilePool.h` + `.cpp` | ✅ (depends on 2) |
| 7 | Create `HDF5Writer.h` + `.cpp` | ✅ (depends on 1, 2, 6) |
| 8 | Create `MLDPGrpcWriter.h` + `.cpp` | ✅ (depends on 1, 3; logic cut from controller) |
| 9 | Update `MLDPPVXSController.h` + `.cpp` | ✅ (depends on 1, 7, 8) |
| 10 | Update `CMakeLists.txt` | ✅ (final link step) |

---

## 6. Progress Checklist

### Step 1 — `IWriter` interface
- [ ] Create `include/writer/IWriter.h`
- [ ] Verify it compiles with `make` (header-only, no source)

### Step 2 — `HDF5WriterConfig`
- [ ] Create `include/writer/hdf5/HDF5WriterConfig.h`
- [ ] Create `src/writer/hdf5/HDF5WriterConfig.cpp`
- [ ] Validate: `basePath` required; `maxFileAge > 0`; `maxFileSizeMB > 0`; `compressionLevel` in `[0,9]`

### Step 3 — `MLDPGrpcWriterConfig`
- [ ] Create `include/writer/grpc/MLDPGrpcWriterConfig.h`
- [ ] Create `src/writer/grpc/MLDPGrpcWriterConfig.cpp`
- [ ] Reads `mldp-pool` from root config node; thread/stream knobs from `writer.grpc` node

### Step 4 — `WriterConfig`
- [ ] Create `include/writer/WriterConfig.h`
- [ ] Create `src/writer/WriterConfig.cpp`
- [ ] Handle `#ifndef MLDP_PVXS_HDF5_ENABLED` guard for HDF5 parse path
- [ ] Throw `std::invalid_argument` when `!anyEnabled()`

### Step 5 — `MLDPPVXSControllerConfig` updates
- [ ] Add `#include "writer/WriterConfig.h"` to header
- [ ] Change `pool_` to `std::optional<MLDPGrpcPoolConfig>`
- [ ] Add `writerConfig_` member and `writerConfig()` accessor
- [ ] Add `parseWriter()` method
- [ ] Make `parsePool()` conditional on `grpcEnabled`
- [ ] Update `parse()` call sequence
- [ ] Existing unit tests in `test/config/mldppvxs_controller_config_test.cpp` still pass

### Step 6 — `HDF5FilePool`
- [ ] Create `include/writer/hdf5/HDF5FilePool.h`
- [ ] Create `src/writer/hdf5/HDF5FilePool.cpp`
- [ ] `acquire()` creates file on first call for a source
- [ ] `acquire()` rotates on age or size threshold
- [ ] File path pattern: `<base-path>/<safe_source>_<ISO8601UTC>.h5`
- [ ] `recordWrite()` updates `bytesWritten`
- [ ] `flushAll()` and `closeAll()` work correctly
- [ ] Wrap in `#ifdef MLDP_PVXS_HDF5_ENABLED`

### Step 7 — `HDF5Writer`
- [ ] Create `include/writer/hdf5/HDF5Writer.h`
- [ ] Create `src/writer/hdf5/HDF5Writer.cpp`
- [ ] `push()` enqueues to bounded queue (cap 8192), returns `false` when full
- [ ] Writer thread drains queue → `appendFrame()` per DataFrame in batch
- [ ] Flush thread calls `HDF5FilePool::flushAll()` every `flush-interval-ms`
- [ ] `ensureDataset()` creates chunked unlimited dataset on first call
- [ ] `protoTypeToH5Type()` covers all DataType enum values (verify against `common.pb.h`)
- [ ] `appendFrame()`: extend dataset + hyperslab select + write for timestamps and all columns
- [ ] Wrap in `#ifdef MLDP_PVXS_HDF5_ENABLED`
- [ ] `stop()` drains remaining queue items before closing files

### Step 8 — `MLDPGrpcWriter`
- [ ] Create `include/writer/grpc/MLDPGrpcWriter.h`
- [ ] Create `src/writer/grpc/MLDPGrpcWriter.cpp`
- [ ] Cut `WorkerChannel`, `QueueItem`, `workerLoop()`, `buildRequest()` from `MLDPPVXSController.cpp`
      into this class (no logic change)
- [ ] `start()` constructs `MLDPGrpcIngestionePool` (from `pool/`) and spawns worker threads
- [ ] `push()` round-robins frames across `WorkerChannel`s (identical to prior controller logic)
- [ ] `stop()` signals shutdown and joins threads

### Step 9 — `MLDPPVXSController` refactor
- [ ] Remove `WorkerChannel`, `QueueItem`, `workerLoop`, `buildRequest` from header and source
- [ ] Remove `mldp_ingestion_pool_`, `channels_`, `next_channel_`, `queued_items_` members
- [ ] Add `#include "writer/IWriter.h"` and `writers_` vector member
- [ ] Constructor builds and starts writers from `writerConfig()`
- [ ] Destructor calls `w->stop()` for all writers
- [ ] `push()` implements best-effort fan-out (copy for all-but-last writer)
- [ ] `querySourcesInfo()` and `querySourcesData()` unchanged

### Step 10 — `CMakeLists.txt`
- [ ] Add `option(MLDP_PVXS_ENABLE_HDF5 ...)` with `find_package(HDF5 REQUIRED COMPONENTS CXX)`
- [ ] Add `add_compile_definitions(MLDP_PVXS_HDF5_ENABLED)` when found
- [ ] Add new `.cpp` files to `target_sources` with generator expression for HDF5 files
- [ ] Link `${HDF5_CXX_LIBRARIES}` conditionally
- [ ] Include `${HDF5_INCLUDE_DIRS}` conditionally

### Step 11 — Tests
- [ ] Existing tests in `test/config/mldppvxs_controller_config_test.cpp` still pass
- [ ] New config tests in `test/config/writer_config_test.cpp`:
  - [ ] No `writer:` block → `grpcEnabled=true`, `hdf5Enabled=false`
  - [ ] Both writers enabled
  - [ ] HDF5-only (`mldp-pool` absent) → no throw
  - [ ] Both disabled → throw
- [ ] New `test/writer/hdf5/hdf5_writer_test.cpp`:
  - [ ] Push a batch with 2 frames → stop → open `.h5` file → verify dataset names and row count
- [ ] Fan-out integration test with a `MockWriter` stub:
  - [ ] Both writers receive same batch
  - [ ] `push()` returns `true` even when one mock rejects
- [ ] File rotation test: `max-file-age-s: 1`, push for 2 s → verify 2 files created

---

## 7. YAML Reference

### Both writers enabled
```yaml
controller-thread-pool: 4
controller-stream-max-bytes: 2097152
controller-stream-max-age-ms: 200

mldp-pool:                              # required when writer.grpc.enabled=true
  provider-name: my-provider
  ingestion-url: dp-ingestion:50051
  query-url: dp-query:50052
  min-conn: 1
  max-conn: 4

writer:
  grpc:
    enabled: true
  hdf5:
    enabled: true
    base-path: /data/hdf5
    max-file-age-s: 3600
    max-file-size-mb: 512
    flush-interval-ms: 1000
    compression-level: 6      # DEFLATE 0–9 (0 = no compression)

reader:
  - epics-pvxs:
      - name: my_reader
        pvs:
          - name: "MY:PV:1"
          - name: "MY:PV:2"

metrics:
  endpoint: 0.0.0.0:9464
```

### HDF5 only (`mldp-pool` omitted)
```yaml
controller-thread-pool: 2

writer:
  grpc:
    enabled: false
  hdf5:
    enabled: true
    base-path: /mnt/archive/hdf5
    max-file-age-s: 7200
    max-file-size-mb: 1024
    compression-level: 4

reader:
  - epics-pvxs:
      - name: my_reader
        pvs:
          - name: "MY:PV:1"
```

### Legacy (no `writer:` block — full backward compatibility)
```yaml
controller-thread-pool: 2
mldp-pool:
  provider-name: my-provider
  ingestion-url: dp-ingestion:50051
reader:
  - epics-pvxs:
      - name: my_reader
        pvs:
          - name: "MY:PV:1"
```

---

## 8. Directory Layout After Implementation

```
include/
├── pool/                               ← unchanged (shared by readers and writers)
│   ├── MLDPGrpcPool.h
│   └── MLDPGrpcPoolConfig.h
├── reader/                             ← unchanged
│   └── …
├── controller/
│   ├── MLDPPVXSController.h            ← modified
│   └── MLDPPVXSControllerConfig.h      ← modified
└── writer/                             ← NEW (mirrors reader/)
    ├── IWriter.h                       ← NEW
    ├── WriterConfig.h                  ← NEW
    ├── grpc/
    │   ├── MLDPGrpcWriterConfig.h      ← NEW
    │   └── MLDPGrpcWriter.h            ← NEW
    └── hdf5/
        ├── HDF5WriterConfig.h          ← NEW
        ├── HDF5FilePool.h              ← NEW
        └── HDF5Writer.h               ← NEW

src/
├── pool/                               ← unchanged
├── reader/                             ← unchanged
├── controller/
│   ├── MLDPPVXSController.cpp          ← modified
│   └── MLDPPVXSControllerConfig.cpp    ← modified
└── writer/                             ← NEW
    ├── WriterConfig.cpp                ← NEW
    ├── grpc/
    │   ├── MLDPGrpcWriterConfig.cpp    ← NEW
    │   └── MLDPGrpcWriter.cpp          ← NEW
    └── hdf5/
        ├── HDF5WriterConfig.cpp        ← NEW
        ├── HDF5FilePool.cpp            ← NEW
        └── HDF5Writer.cpp             ← NEW

CMakeLists.txt                          ← modified
```
