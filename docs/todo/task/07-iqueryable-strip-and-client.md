# TODO-07: Strip IQueryable to factory marker + update MLDPQueryClient + migrate integration test

## Goal
Remove virtual API methods from `IQueryable` (factory marker only), update
`MLDPQueryClient` accordingly (remove `override`, add `Config`-ctor), and
migrate the integration test to use `QueryableFactory`.

This is a coordinated breaking change — all three parts must land in one commit.

## Depends On
- TODO-06 (`QueryableFactory` and `IQueryableUPtr` exist)

## Files to Change

### `include/query/IQueryable.h`
Remove:
- `#include <common.pb.h>`
- `#include <util/bus/IDataBus.h>`
- `#include <optional>`, `<set>`, `<unordered_map>`, `<vector>` (no longer needed here)
- `querySourcesInfo()` pure virtual method
- `querySourcesData()` pure virtual method

Keep:
- `virtual ~IQueryable() = default;`
- `using IQueryableUPtr = std::unique_ptr<IQueryable>;` (added in TODO-06)

Add doc comment: `// Factory marker only. Use QueryableFactory::create<T>() for concrete access.`

### `include/query/impl/mldp/MLDPQueryClient.h`
- Remove `override` keyword from `querySourcesInfo` and `querySourcesData` declarations
- Remove `virtual` if present on those methods
- Add new constructor declaration:
  ```cpp
  explicit MLDPQueryClient(const config::Config&,
                           std::shared_ptr<metrics::Metrics> = nullptr);
  ```
- Existing pool-config ctor stays as-is

### `src/query/MLDPQueryClient.cpp`
- Add delegating `Config`-ctor implementation:
  ```cpp
  MLDPQueryClient::MLDPQueryClient(const config::Config& cfg,
                                   std::shared_ptr<metrics::Metrics> m)
      : MLDPQueryClient(util::pool::MLDPGrpcPoolConfig(cfg), std::move(m))
  {}
  ```
- `querySourcesInfo` and `querySourcesData` implementations stay unchanged
  (they are no longer virtual/override but still public methods)

### `test/writer/mldp/mldp_writer_integration_test.cpp`
Migrate two call sites. Add includes:
```cpp
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPQueryClient.h>
```

**Call site 1 (around line 577):**
Before (to remove):
```cpp
std::shared_ptr<mldp_pvxs_driver::query::IQueryable> queryClient =
    std::make_shared<mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient>(
        make_pool_config(1, 1, "query_api_probe_provider", "query api probe provider"));
```
After:
- In `SetUpTestSuite()` (create one if absent):
  ```cpp
  QueryableFactory::instance().reset();
  QueryableFactory::instance().prepare<MLDPQueryClient>(
      make_pool_config_as_config(1, 1, "query_api_probe_provider", "query api probe provider"));
  ```
- Where `queryClient` is used:
  ```cpp
  auto queryClient = QueryableFactory::instance().create<MLDPQueryClient>();
  ```
  `queryClient` is now `unique_ptr<MLDPQueryClient>` — use directly, no cast needed.
- In `TearDownTestSuite()`: `QueryableFactory::instance().reset();`

**Call site 2 (around line 674):**
Same pattern with config `"query_data_multi_probe_provider"`.
If both call sites use different provider names, prepare may need to be called twice
or use the same factory slot (only one `MLDPQueryClient` type can be prepared at once).
Check if they can share — if not, call `reset()` + `prepare()` before each test that
needs a different config, or use the concrete constructor directly for the second site
since `QueryableFactory` is mainly needed for the factory pattern demonstration.

**Add `make_pool_config_as_config` helper** in the test file:
```cpp
static config::Config make_pool_config_as_config(
    int min_conn, int max_conn,
    const std::string& provider_name,
    const std::string& provider_desc)
{
    // Build YAML string and parse via makeConfigFromYaml (see test/config/test_config_helpers.h)
    std::string yaml = fmt::format(
        "ingestion-url: localhost:50051\n"
        "query-url: localhost:50052\n"
        "provider-name: {}\n"
        "provider-description: {}\n"
        "min-connections: {}\n"
        "max-connections: {}\n",
        provider_name, provider_desc, min_conn, max_conn);
    return makeConfigFromYaml(yaml);
}
```
Adjust host/port to match existing test setup.

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Confirm no remaining IQueryable virtual method usage
grep -rn "querySourcesInfo\|querySourcesData" src/ include/ --include="*.cpp" --include="*.h" | grep "virtual\|override\|= 0"
# Run integration tests
ctest --test-dir build -R "mldp_writer_integration" -V 2>&1 | tail -30
```

## Commit
```
refactor(query): strip IQueryable to factory marker; migrate MLDPQueryClient and tests

IQueryable is now a virtual-dtor-only factory marker. querySourcesInfo and
querySourcesData are plain public methods on MLDPQueryClient. MLDPQueryClient
gains a Config-based constructor for use with QueryableFactory. The MLDP writer
integration test is migrated to QueryableFactory::prepare/create.
```
