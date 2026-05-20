# TODO-08: Controller config + controller startup — parse `queryable:` and call `prepareQueryables()`

## Goal
`MLDPPVXSControllerConfig` parses the `queryable:` YAML sequence into `vector<QueryableEntry>`.
`MLDPPVXSController` calls `prepareQueryables()` at startup before any worker thread starts.

## Depends On
- TODO-06 (`QueryableFactory` exists)
- TODO-07 (`MLDPQueryClient` has Config-ctor)

## Files to Change

### `include/controller/MLDPPVXSControllerConfig.h`
Add:
```cpp
struct QueryableEntry {
    std::string    type;
    config::Config cfg;
};
const std::vector<QueryableEntry>& queryableEntries() const { return queryable_entries_; }
```
Add private member:
```cpp
std::vector<QueryableEntry> queryable_entries_;
```

### `src/controller/MLDPPVXSControllerConfig.cpp`
Add constant at top of file (with other key constants):
```cpp
static constexpr auto QueryableKey = "queryable";
```

In the config parsing function/constructor, after existing section parsing:
```cpp
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

Check how `root.subConfig(QueryableKey)` returns a range — look at how `readerEntries_`
parsing works and mirror that pattern exactly.

### `src/controller/MLDPPVXSController.cpp`
Add includes:
```cpp
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPQueryClient.h>
```

Add static helper function (above or near `start()`):
```cpp
static void prepareQueryables(const MLDPPVXSControllerConfig& cfg,
                               std::shared_ptr<metrics::Metrics> metrics)
{
    using namespace mldp_pvxs_driver::query;
    using namespace mldp_pvxs_driver::query::impl::mldp;

    using PrepFn = std::function<void(const config::Config&,
                                     std::shared_ptr<metrics::Metrics>)>;
    static const std::unordered_map<std::string, PrepFn> kDispatch = {
        {"mldp",
         [](const config::Config& c, std::shared_ptr<metrics::Metrics> m) {
             QueryableFactory::instance().prepare<MLDPQueryClient>(c, std::move(m));
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

Note: `"mldp-annotation"` dispatch entry is added in TODO-10 when `MLDPAnnotationQueryClient` exists.

In `MLDPPVXSController::start()`, call before worker threads start:
```cpp
prepareQueryables(controller_cfg_, metrics_);
```
Find the right insertion point by checking where other startup init occurs.

## YAML example (for integration test or manual test)
```yaml
queryable:
  - type: mldp
    ingestion-url: "localhost:50051"
    query-url:     "localhost:50052"
    provider-name: "my-provider"
    min-connections: 1
    max-connections: 4
```

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Config unit test: parse YAML with queryable section, assert queryableEntries() populated
ctest --test-dir build -R "controller.*config\|config.*controller" -V 2>&1 | tail -20
```

## Commit
```
feat(query): parse queryable: config section and prepare factory at controller startup

MLDPPVXSControllerConfig now parses a queryable: YAML sequence into
QueryableEntry vector. MLDPPVXSController calls prepareQueryables() at
startup, registering each configured queryable type with QueryableFactory
before any worker thread runs.
```
