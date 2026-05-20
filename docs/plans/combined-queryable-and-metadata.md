# Plan: Queryable Factory + Static Metadata + Annotation Queryable

## Context

Three separate needs unified into one plan:

1. **QueryableFactory** — `MLDPQueryClient` constructed directly today; no factory. A new
   **singleton** `QueryableFactory` is introduced. `IQueryable` is a **factory marker only** —
   virtual dtor, no virtual API methods. At startup the controller calls
   `QueryableFactory::instance().prepare<T>(cfg)` for each queryable type it needs, storing a
   closure that captures the config. At runtime `QueryableFactory::instance().create<T>()`
   invokes the stored closure and returns `std::unique_ptr<T>` directly — callable from anywhere,
   not only from controller-managed code. No config passed at runtime. Since `T` is already known
   at the call site, no intermediate cast or holder is needed.

2. **Static metadata on readers** — users want to attach facility/system/experiment labels in YAML
   at reader-level and per-PV. Today `EventBatchStruct::tags` is a flat `vector<string>` that no
   writer populates. Replace with `map<string,string>` and wire through both writers.

3. **Annotation queryable** — `DpAnnotationService` exposes `queryPvMetadata` / `getPvMetadata`
   (user-curated PV metadata: aliases, tags, attributes, description) and full
   Configuration + ConfigurationActivation CRUD. No driver-side client exists yet.
   `MLDPAnnotationQueryClient` wraps these RPCs. It inherits `IQueryable` (factory marker only)
   and defines all PV-metadata + configuration methods directly on the concrete class.

**End result — two queryable objects, one interface, one factory, one holder:**
| Object | Base | Backs |
|---|---|---|
| `MLDPQueryClient` | `IQueryable` | `DpQueryService` — ingested time-series data |
| `MLDPAnnotationQueryClient` | `IQueryable` | `DpAnnotationService` — PV metadata + configuration |

**Factory lifecycle — Meyer's singleton, prepare at startup, create anywhere:**

`QueryableFactory` is a **Meyer's singleton**. No CRTP base. No macro. Controller calls
`prepare<T>()` at startup; any code anywhere calls `create<T>()` at runtime without holding
a controller reference.

- **Startup** — controller calls `QueryableFactory::instance().prepare<T>(cfg, metrics)`.
  Stores `type_index → closure`. Must complete before any worker thread calls `create<T>()`.
- **Runtime** — any subsystem calls `QueryableFactory::instance().create<T>()`.
  Returns `std::unique_ptr<T>`. Throws if `T` was not prepared.

```cpp
// include/query/QueryableFactory.h
#include <shared_mutex>
#include <typeindex>

namespace mldp_pvxs_driver::query {

class QueryableFactory {
public:
    // C++11 static-local init is thread-safe — no double-checked locking needed.
    static QueryableFactory& instance() {
        static QueryableFactory inst;
        return inst;
    }

    // Startup: bind type T to its config. Exclusive write lock.
    // Must complete before any create<T>() call (startup is single-threaded).
    template <typename T>
    void prepare(const config::Config& cfg,
                 std::shared_ptr<metrics::Metrics> metrics = nullptr)
    {
        std::unique_lock lock(mutex_);
        creators_[std::type_index(typeid(T))] =
            [cfg, metrics]() -> IQueryableUPtr {
                return std::make_unique<T>(cfg, metrics);
            };
    }

    // Runtime: create instance of T. Returns unique_ptr<T> directly — no intermediate cast.
    // static_cast is safe: the closure stored by prepare<T> always produces a T.
    // Creator copied under shared lock, invoked outside it — slow T ctor does not block peers.
    template <typename T>
    std::unique_ptr<T> create()
    {
        std::function<IQueryableUPtr()> creator;
        {
            std::shared_lock lock(mutex_);
            auto it = creators_.find(std::type_index(typeid(T)));
            if (it == creators_.end())
                throw std::runtime_error(
                    std::string("QueryableFactory: type not prepared: ") + typeid(T).name());
            creator = it->second;
        }
        return std::unique_ptr<T>(static_cast<T*>(creator().release()));
    }

    // Shared read lock — concurrent with other isPrepared / create calls.
    template <typename T>
    bool isPrepared() const {
        std::shared_lock lock(mutex_);
        return creators_.count(std::type_index(typeid(T))) > 0;
    }

    // Test helper: exclusive write lock. Call in TearDownTestSuite.
    void reset() {
        std::unique_lock lock(mutex_);
        creators_.clear();
    }

private:
    QueryableFactory() = default;
    mutable std::shared_mutex mutex_;   // shared_mutex: concurrent reads, exclusive writes
    std::unordered_map<std::type_index, std::function<IQueryableUPtr()>> creators_;
};

} // namespace
```

`IQueryable` — virtual dtor only, factory marker:

```cpp
// include/query/IQueryable.h
class IQueryable {
public:
    virtual ~IQueryable() = default;
    // No virtual API methods — factory marker only.
    // Use QueryableFactory::create<T>() to get a concrete unique_ptr<T> directly.
};
using IQueryableUPtr = std::unique_ptr<IQueryable>;
```

`QueryableHolder` — owns `IQueryableUPtr`, typed downcast:

```cpp
// include/query/QueryableHolder.h
class QueryableHolder {
public:
    QueryableHolder() = default;
    explicit QueryableHolder(IQueryableUPtr impl) : impl_(std::move(impl)) {}
    bool valid() const { return impl_ != nullptr; }

    template <typename T>
    T* as() const { return dynamic_cast<T*>(impl_.get()); }

private:
    IQueryableUPtr impl_;
};
```

**YAML config structure** — `queryable:` is a flat sequence with explicit `type:` field.
Note: `readers:` uses type-as-key (e.g. `epics-archiver:`); `queryable:` uses `type:` field
instead — fewer expected entries, simpler dispatch, no need to iterate registered type names.
Verified against `MLDPPVXSControllerConfig.cpp`: `hasChild` + `subConfig` + `isSequence` are
the correct `Config` API calls.

```yaml
queryable:
  - type: mldp
    ingestion-url: "localhost:50051"
    query-url:     "localhost:50052"
    provider-name: "my-provider"
    min-connections: 1
    max-connections: 4

  - type: mldp-annotation
    annotation-url: "localhost:50053"
    min-connections: 1
    max-connections: 2
```

Each entry has a required `type:` key. Remaining keys are implementation-specific and parsed
by the concrete class constructor via `MLDPGrpcPoolConfig(cfg)`.

Controller config parses `queryable:` into `vector<QueryableEntry>` — same pattern as
`readerEntries_` in `MLDPPVXSControllerConfig`:

```cpp
// include/controller/MLDPPVXSControllerConfig.h  (additions)
struct QueryableEntry {
    std::string    type;
    config::Config cfg;
};
const std::vector<QueryableEntry>& queryableEntries() const { return queryable_entries_; }
```

```cpp
// src/controller/MLDPPVXSControllerConfig.cpp  (additions)
static constexpr auto QueryableKey = "queryable";

if (root.hasChild(QueryableKey)) {
    if (!root.isSequence(QueryableKey))
        throw Error("queryable must be a sequence");
    for (const auto& node : root.subConfig(QueryableKey)) {
        const auto type = node.get("type", "");
        if (type.empty()) throw Error("queryable entry missing 'type' field");
        queryable_entries_.push_back({type, node});
    }
}
```

`MLDPPVXSController` dispatches `prepare<T>()` by type string. All known types in a static
`type → lambda` map — explicit, no macro:

```cpp
// src/controller/MLDPPVXSController.cpp
static void prepareQueryables(const MLDPPVXSControllerConfig& cfg,
                               std::shared_ptr<metrics::Metrics> metrics)
{
    using PrepFn = std::function<void(const config::Config&,
                                     std::shared_ptr<metrics::Metrics>)>;
    static const std::unordered_map<std::string, PrepFn> kDispatch = {
        {"mldp",
         [](const config::Config& c, std::shared_ptr<metrics::Metrics> m) {
             QueryableFactory::instance().prepare<MLDPQueryClient>(c, std::move(m));
         }},
        {"mldp-annotation",
         [](const config::Config& c, std::shared_ptr<metrics::Metrics> m) {
             QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(c, std::move(m));
         }},
    };
    for (const auto& entry : cfg.queryableEntries()) {
        auto it = kDispatch.find(entry.type);
        if (it == kDispatch.end())
            throw std::runtime_error("Unknown queryable type: " + entry.type);
        it->second(entry.cfg, metrics);
    }
}
```

Called once in `MLDPPVXSController::start()` before any worker thread starts:

```cpp
prepareQueryables(controller_cfg_, metrics_);
```

Any code creates instances at runtime — no controller reference needed:

```cpp
// In any subsystem, service, callback, etc.:
auto q = QueryableFactory::instance().create<MLDPQueryClient>();
auto infos = q->querySourcesInfo(source_names);

auto a = QueryableFactory::instance().create<MLDPAnnotationQueryClient>();
auto meta = a->getPvMetadata("BPMS:LI21:201:X");
```

`create<T>()` returns `unique_ptr<T>` directly — `T` is already known at the call site, no
intermediate cast needed. Throws `std::runtime_error` if `T` was not prepared at startup.
Concrete classes need only a `Config`-based constructor — no macro, no static registration.

---

## Scope

| Layer | Change |
|---|---|
| `EventBatchStruct` | `tags: vector<string>` → `metadata: unordered_map<string,string>` |
| Both reader config structs | Add `static_metadata_` (reader-level) + `PVConfig::metadata` (PV-level), parse from YAML |
| Both reader impls | Merge reader + PV metadata into `EventBatch::metadata` before push |
| `MLDPWriter` | Forward `metadata` into column provenance labels |
| `HDF5WriterBase` | Write `metadata` as HDF5 group attributes on source group creation |
| `IQueryable.h` | Strip to virtual-dtor-only marker; add `IQueryableUPtr` alias |
| `QueryableHolder.h` (new) | Header-only wrapper: owns `IQueryableUPtr`, exposes `as<T>()` — for heterogeneous storage only |
| `QueryableFactory.h` (new) | Meyer's singleton: `prepare<T>(cfg)` at startup, `create<T>()` returns `unique_ptr<T>` directly |
| `MLDPQueryClient` | Add `Config`-ctor; remove `override` on virtual methods |
| `MLDPGrpcAnnotationPool` (new) | Pool holding `DpAnnotationService::Stub` |
| `MLDPGrpcPoolConfig` | Add `annotation_url_` + `annotationUrl()` + YAML key `annotation-url` |
| `MLDPAnnotationQueryClient` (new) | Inherits `IQueryable`; all annotation API as concrete methods |
| `CMakeLists.txt` | Add new source files |

---

## Critical Files

| File | Action |
|---|---|
| `include/util/bus/IDataBus.h` | `vector<string> tags` → `unordered_map<string,string> metadata` |
| `include/reader/impl/epics_archiver/EpicsArchiverReaderConfig.h` | Add `static_metadata_` + `PVConfig::metadata` |
| `src/reader/impl/epics_archiver/EpicsArchiverReaderConfig.cpp` | Parse `metadata:` YAML map at reader + PV level |
| `include/reader/impl/epics/shared/EpicsReaderConfig.h` | Same as above |
| `src/reader/impl/epics/shared/EpicsReaderConfig.cpp` | Same as above |
| `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` | Merge metadata into `EventBatch::metadata` |
| `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp` | Same |
| `src/reader/impl/epics/base/EpicsBaseReader.cpp` | Same |
| `src/writer/mldp/MLDPWriter.cpp` | Forward `metadata` into column provenance labels |
| `src/writer/hdf5/HDF5WriterBase.cpp` | Write `metadata` as HDF5 group attributes |
| `include/pool/MLDPGrpcPoolConfig.h` | Add `annotation_url_` field + `annotationUrl()` accessor |
| `src/pool/MLDPGrpcPoolConfig.cpp` | Parse `annotation-url` YAML key |
| `include/pool/MLDPGrpcAnnotationPool.h` | **NEW** — annotation pool class |
| `src/pool/MLDPGrpcAnnotationPool.cpp` | **NEW** |
| `include/query/IQueryable.h` | Strip to virtual-dtor only + `IQueryableUPtr` alias |
| `include/query/QueryableHolder.h` | **NEW** — header-only `as<T>()` wrapper |
| `include/query/QueryableFactory.h` | **NEW** — header-only; `prepare<T>(cfg)` at startup, `create<T>()` at runtime |
| `include/query/impl/mldp/MLDPQueryClient.h` | Add `Config`-ctor; remove `override` |
| `src/controller/MLDPPVXSController.cpp` | Add `prepareQueryables(cfg, metrics)` call in startup path |
| `src/controller/MLDPPVXSControllerConfig.cpp` | Parse `queryable:` section; add `QueryableKey` constant; store entries (type + sub-config) |
| `include/controller/MLDPPVXSControllerConfig.h` | Add `queryableEntries()` accessor; `QueryableEntry { string type; Config cfg; }` |
| `src/query/MLDPQueryClient.cpp` | Add delegating `Config`-ctor |
| `include/query/impl/mldp/MLDPAnnotationQueryClient.h` | **NEW** |
| `src/query/MLDPAnnotationQueryClient.cpp` | **NEW** |
| `CMakeLists.txt` | Add new `.cpp` sources to `libmldp_pvxs_driver` |

---

## Design

### 1. `EventBatchStruct` change

```cpp
// include/util/bus/IDataBus.h
struct EventBatchStruct {
    std::string                                    reader_name;
    std::string                                    root_source;
    std::unordered_map<std::string, std::string>   metadata;   // replaces vector<string> tags
    std::vector<util::bus::DataBatch>              frames;
    bool                                           end_of_batch_group{false};
    bool                                           is_tabular{false};
};
```

---

### 2. Reader config structs (both `EpicsArchiverReaderConfig` and `EpicsReaderConfig`)

```cpp
static constexpr auto kMetadataKey = "metadata";
std::unordered_map<std::string, std::string> static_metadata_;

struct PVConfig {
    std::string name;
    std::unordered_map<std::string, std::string> metadata;
    // ... existing fields ...
};

const std::unordered_map<std::string, std::string>& staticMetadata() const { return static_metadata_; }
```

YAML schema (identical for both reader types):

```yaml
readers:
  - type: epics-archiver
    metadata:           # reader-level — applies to all PVs
      facility: LCLS
      experiment: CXI-2024
    pvs:
      - name: BPMS:LI21:201:X
        metadata:       # PV-level — merges on top of reader-level; PV wins on conflict
          system: BPM
```

Parsing (`Config::hasChild` not `hasKey`; `operator>>(map<string,string>)` requires YAML map node):

```cpp
// reader-level
if (cfg.hasChild(kMetadataKey)) {
    std::map<std::string, std::string> m;
    cfg.subConfig(kMetadataKey).front() >> m;
    static_metadata_.insert(m.begin(), m.end());
}
// PV-level (inside PV loop)
if (pv_cfg.hasChild(kMetadataKey)) {
    std::map<std::string, std::string> m;
    pv_cfg.subConfig(kMetadataKey).front() >> m;
    pv.metadata.insert(m.begin(), m.end());
}
```

Reader merge at push time (PV-level wins):

```cpp
auto merged = config_.staticMetadata();
for (auto& [k, v] : pv_config.metadata)
    merged[k] = v;
batch.metadata = std::move(merged);
```

---

### 3. `MLDPWriter` — column provenance labels

```cpp
// In toDataFrame() / buildRequest(), per-column loop:
auto* col_meta = col.mutable_metadata();
auto* prov     = col_meta->mutable_provenance();
for (auto& [k, v] : item.metadata)
    (*prov->mutable_labels())[k] = v;
```

All columns in one batch share the same static metadata (reader + PV merged).

---

### 4. `HDF5WriterBase` — group attributes

```cpp
// On source group creation only (track via set<string> seen_groups_):
auto group = file_.require_group(batch.root_source);
if (seen_groups_.insert(batch.root_source).second) {
    for (auto& [k, v] : batch.metadata)
        group.createAttribute(k, v);   // verify exact HighFive call before implementing
}
```

Check `src/writer/hdf5/HDF5WriterBase.cpp` for exact HighFive attribute write pattern.

---

### 5. `IQueryable` — factory marker only

```cpp
// include/query/IQueryable.h
namespace mldp_pvxs_driver::query {

class IQueryable {
public:
    virtual ~IQueryable() = default;
    // No virtual API methods — factory marker only.
    // Use QueryableFactory::create<T>() to get a concrete unique_ptr<T> directly.
};

using IQueryableUPtr = std::unique_ptr<IQueryable>;

} // namespace
```

Existing `querySourcesInfo` / `querySourcesData` virtual methods are **removed** from the
interface and become plain (non-virtual) public methods on `MLDPQueryClient` directly.

---

### 6. `QueryableHolder`

`QueryableHolder` is **not** the primary usage pattern. Normal code uses
`QueryableFactory::create<T>()` which returns `unique_ptr<T>` directly.

`QueryableHolder` exists only for **heterogeneous storage** — e.g. a container that holds
mixed `MLDPQueryClient` and `MLDPAnnotationQueryClient` instances without knowing the concrete
type at compile time. In that case `as<T>()` provides a type-safe downcast.

```cpp
// include/query/QueryableHolder.h
namespace mldp_pvxs_driver::query {

class QueryableHolder {
public:
    QueryableHolder() = default;
    explicit QueryableHolder(IQueryableUPtr impl) : impl_(std::move(impl)) {}

    bool valid() const { return impl_ != nullptr; }

    // Returns T* if stored impl is-a T, else nullptr. Never throws.
    template <typename T>
    T* as() const { return dynamic_cast<T*>(impl_.get()); }

private:
    IQueryableUPtr impl_;
};

} // namespace
```

---

### 7. `QueryableFactory` — instance-based, header-only

See full class definition in the **Factory lifecycle** section above. No `.cpp` file.
No static CRTP base. No macro.

`MLDPQueryClient` additions:

```cpp
// header — add Config-ctor; remove 'override' from querySourcesInfo / querySourcesData
explicit MLDPQueryClient(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr);

// .cpp — delegates to existing MLDPGrpcPoolConfig ctor
MLDPQueryClient::MLDPQueryClient(const config::Config& cfg, std::shared_ptr<metrics::Metrics> m)
    : MLDPQueryClient(util::pool::MLDPGrpcPoolConfig(cfg), std::move(m))
{}
```

`querySourcesInfo` / `querySourcesData` become plain public methods — same signatures,
`override` removed, `virtual` removed from base (`IQueryable` has no virtual API now).

---

### 8. `MLDPGrpcPoolConfig` — annotation URL

```cpp
// header — new field + accessor:
std::string annotation_url_;
const std::string& annotationUrl() const { return annotation_url_; }

// .cpp — parse alongside query-url:
annotation_url_ = root.get("annotation-url", "");
```

---

### 9. `MLDPGrpcAnnotationPool` — mirrors `MLDPGrpcQueryPool`

```cpp
// include/pool/MLDPGrpcAnnotationPool.h
class MLDPGrpcAnnotationPool {
public:
    using MLDPGrpcAnnotationPoolShrdPtr = std::shared_ptr<MLDPGrpcAnnotationPool>;

    static MLDPGrpcAnnotationPoolShrdPtr create(const MLDPGrpcPoolConfig&,
                                                std::shared_ptr<metrics::Metrics> = nullptr);

    util::pool::PooledHandle<AnnotationObject> acquire();

    // AnnotationObject: shared_ptr<grpc::Channel> + unique_ptr<DpAnnotationService::Stub>
};
```

Channel from `config.annotationUrl()`. Stub via `DpAnnotationService::NewStub(channel)`.

---

### 10. `MLDPAnnotationQueryClient` — concrete class, inherits `IQueryable` only

```cpp
// include/query/impl/mldp/MLDPAnnotationQueryClient.h
namespace mldp_pvxs_driver::query::impl::mldp {

class MLDPAnnotationQueryClient final : public query::IQueryable {
public:
    explicit MLDPAnnotationQueryClient(const util::pool::MLDPGrpcPoolConfig&,
                                       std::shared_ptr<metrics::Metrics> = nullptr);
    explicit MLDPAnnotationQueryClient(const config::Config&,
                                       std::shared_ptr<metrics::Metrics> = nullptr);

    // All methods are plain public (not virtual, not overrides):

    // PV metadata ("sourceMetadata" in driver parlance)
    std::optional<dp::service::common::PvMetadata>
    getPvMetadata(const std::string& pvNameOrAlias);

    std::pair<std::vector<dp::service::common::PvMetadata>, std::string /*nextPageToken*/>
    queryPvMetadata(const dp::service::annotation::QueryPvMetadataRequest&);

    // Configuration definition
    std::optional<dp::service::common::Configuration>
    getConfiguration(const std::string& configurationName);

    std::pair<std::vector<dp::service::common::Configuration>, std::string>
    queryConfigurations(const dp::service::annotation::QueryConfigurationsRequest&);

    // Configuration activation
    std::optional<dp::service::common::ConfigurationActivation>
    getConfigurationActivation(const dp::service::annotation::GetConfigurationActivationRequest&);

    std::pair<std::vector<dp::service::common::ConfigurationActivation>, std::string>
    queryConfigurationActivations(
        const dp::service::annotation::QueryConfigurationActivationsRequest&);

    std::vector<dp::service::common::ConfigurationActivation>
    getActiveConfigurations(const dp::service::common::Timestamp& at);

    // All methods plain public — not virtual, no macro, no static registration.

private:
    std::shared_ptr<util::log::ILogger>                               logger_;
    util::pool::MLDPGrpcAnnotationPool::MLDPGrpcAnnotationPoolShrdPtr pool_;
};

} // namespace
```

Implementation pattern (acquire handle → call stub → check status → unpack):

```cpp
std::optional<dp::service::common::PvMetadata>
MLDPAnnotationQueryClient::getPvMetadata(const std::string& pvNameOrAlias)
{
    auto handle = pool_->acquire();
    grpc::ClientContext ctx;
    dp::service::annotation::GetPvMetadataRequest req;
    req.set_pvnameoralias(pvNameOrAlias);
    dp::service::annotation::GetPvMetadataResponse resp;
    const auto status = handle->stub->getPvMetadata(&ctx, req, &resp);
    if (!status.ok()) { /* log + return nullopt */ }
    if (resp.has_exceptional_result()) return std::nullopt;
    return resp.get_pv_metadata_result().pv_metadata();
}
```

---

## Migration of existing usages

### `test/writer/mldp/mldp_writer_integration_test.cpp`

Two call sites construct `MLDPQueryClient` directly via `shared_ptr<IQueryable>` and call
virtual methods on the interface pointer. Both must migrate to `QueryableHolder` + `as<T>()`.

**Before (lines 577–617):**
```cpp
std::shared_ptr<mldp_pvxs_driver::query::IQueryable> queryClient =
    std::make_shared<mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient>(
        make_pool_config(1, 1, "query_api_probe_provider", "query api probe provider"));
// ...
const auto infos = queryClient->querySourcesInfo(sources);
// ...
const auto data = queryClient->querySourcesData(sources, options);
```

**After:**
```cpp
// SetUpTestSuite: configure singleton (once per test suite)
QueryableFactory::instance().reset();   // clear any state from prior suites
QueryableFactory::instance().prepare<MLDPQueryClient>(
    make_pool_config_as_config(1, 1, "query_api_probe_provider", "query api probe provider"));

// Per-test: create returns unique_ptr<MLDPQueryClient> directly — no cast needed
auto queryClient = QueryableFactory::instance().create<MLDPQueryClient>();
const auto infos = queryClient->querySourcesInfo(sources);
const auto data  = queryClient->querySourcesData(sources, options);
```

**Before (lines 674–684):**
```cpp
std::shared_ptr<mldp_pvxs_driver::query::IQueryable> queryClient =
    std::make_shared<mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient>(
        make_pool_config(1, 1, "query_data_multi_probe_provider", "query data multi probe provider"));
// ...
const auto data = queryClient->querySourcesData(sources, options);
```

**After:** same pattern — `QueryableFactory::instance().prepare<MLDPQueryClient>(cfg)` in
`SetUpTestSuite`, then `auto q = QueryableFactory::instance().create<MLDPQueryClient>()` per
test — `q` is `unique_ptr<MLDPQueryClient>`, use directly. Call `reset()` in
`TearDownTestSuite` to clean singleton.

**Note:** `make_pool_config` returns `MLDPGrpcPoolConfig` directly; `prepare<T>` takes
`config::Config`. Add `make_pool_config_as_config` helper in the test (builds `Config`
via `makeConfigFromYaml` with keys: `ingestion-url`, `query-url`, `provider-name`,
`provider-description`, `min-connections`, `max-connections`).

---

## Step Order

1. `IDataBus.h` — replace `tags` with `metadata`
2. Both reader config structs `.h` — add `metadata` fields
3. Both reader config `.cpp` — add YAML parsing
4. Both reader impls — populate `batch.metadata`
5. `MLDPWriter.cpp` — forward metadata to column provenance
6. `HDF5WriterBase.cpp` — write metadata as group attributes
7. `IQueryable.h` — strip to virtual-dtor only + `IQueryableUPtr`
8. `MLDPQueryClient.h/.cpp` — remove `override`, add `Config`-ctor
9. `QueryableHolder.h` — new header-only file
10. `QueryableFactory.h` — new header-only file (`prepare<T>` / `create<T>`)
11. `MLDPPVXSControllerConfig.h/.cpp` — add `QueryableEntry`, `queryable_entries_`, parse `queryable:` sequence
    `MLDPPVXSController.cpp` — add `prepareQueryables()` static helper; call in `start()`
12. `mldp_writer_integration_test.cpp` — migrate two call sites: `QueryableFactory::instance().prepare<T>` in `SetUpTestSuite`, `create<T>` returns `unique_ptr<T>` directly per test
12. `MLDPGrpcPoolConfig.h/.cpp` — add `annotation_url_`
13. `MLDPGrpcAnnotationPool.h/.cpp` — new pool
14. `MLDPAnnotationQueryClient.h/.cpp` — new client
15. `CMakeLists.txt` — add sources
16. New annotation queryable tests

---

## Existing utilities to reuse

| Utility | Location | Used for |
|---|---|---|
| `MLDPGrpcPoolConfig(const config::Config&)` | `include/pool/MLDPGrpcPoolConfig.h:98` | Config-ctor delegation in both concrete queryables |
| `Config::operator>>(map<string,string>&)` | `src/config/Config.cpp:237` | Parse metadata YAML maps |
| `Config::hasChild(key)` | `include/config/Config.h` | Existence check (not `hasKey`) |
| `makeConfigFromYaml()` | `test/config/test_config_helpers.h` | Test YAML assembly |
| `MLDPGrpcQueryPool` pattern | `include/pool/MLDPGrpcQueryPool.h` | Template for annotation pool |

---

## Open Questions

1. **HDF5 attribute API** — verify exact HighFive call for string attribute write in
   `HDF5WriterBase.cpp` before implementing step 6.

2. **Column provenance field name** — verify generated protobuf accessor name for
   `ColumnMetadata.provenance.labels` in `common.proto` before step 5.

3. **`annotation-url` required?** — missing key: hard error or soft disable?
   Recommendation: soft — log warning, `QueryableFactory::create("mldp-annotation", cfg)`
   returns an invalid holder (`valid() == false`).

4. **Existing `IQueryable` callers** — any call site that calls `querySourcesInfo` /
   `querySourcesData` through an `IQueryable*` / `IQueryableUPtr` must be updated to use
   `QueryableFactory::create<MLDPQueryClient>()` which returns `unique_ptr<MLDPQueryClient>`
   directly. Audit all usages before step 7.

---

## Verification

1. **Compile** — `cmake --build build` clean with all new files.
2. **Config parsing** — unit tests: YAML with reader-level + PV-level `metadata:`; assert
   `staticMetadata()` + `PVConfig::metadata` populated; PV-level key overrides reader-level.
3. **EventBatch propagation** — log `batch.metadata` before push; run archiver reader in
   `historical_once` mode; verify map populated.
4. **HDF5 output** — `h5dump` output file; check source group attributes match config.
5. **MLDP column provenance** — verify `labels` map populated on each column via log or
   gRPC interceptor.
6. **Factory prepare/create** — unit test: `factory.prepare<MLDPQueryClient>(cfg)`;
   `factory.isPrepared<MLDPQueryClient>()` true; `factory.isPrepared<MLDPAnnotationQueryClient>()`
   false before its `prepare` call.
7. **Unprepared type throws** — `factory.create<MLDPAnnotationQueryClient>()` before
   `prepare<MLDPAnnotationQueryClient>()` throws `std::runtime_error` containing type name.
8. **`create<T>()` type safety** — unit test: `prepare<MLDPQueryClient>` then
   `create<MLDPQueryClient>()` returns non-null `unique_ptr<MLDPQueryClient>`. Unprepared type
   throws. Optionally verify `QueryableHolder::as<T>()` with a manually constructed holder.
9. **Annotation queryable integration** — with live MLDP: `create<MLDPAnnotationQueryClient>()->getPvMetadata(...)` returns populated record; `getActiveConfigurations(now)` returns result.

---

## File cleanup

Delete (superseded by this document):
- `docs/plans/static-metadata-readers.md`
- `docs/plans/queryable-factory-pattern.md`
