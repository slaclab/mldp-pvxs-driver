# Plan: QueryableFactory — self-registering IQueryable implementations

## Context

`IQueryable` exists as a pure interface in `include/query/IQueryable.h` but has no factory.
The only implementation (`MLDPQueryClient`) is constructed directly — callers must know the
concrete type and `MLDPGrpcPoolConfig`. The reader/writer subsystems already use a proven
CRTP factory pattern with static-init registration (`REGISTER_READER` / `REGISTER_WRITER`).
Goal: replicate that pattern for queryable backends so callers only hold `IQueryableUPtr`
and select an implementation by name string.

---

## Files to change

| File | Action |
|---|---|
| `include/query/IQueryable.h` | Add `using IQueryableUPtr = std::unique_ptr<IQueryable>` after class |
| `include/query/QueryableFactory.h` | **NEW** — factory + registrator + macro |
| `src/query/QueryableFactory.cpp` | **NEW** — thin `create` wrapper |
| `include/query/impl/mldp/MLDPQueryClient.h` | Add config ctor + `REGISTER_QUERYABLE` |
| `src/query/MLDPQueryClient.cpp` | Implement new config ctor |
| `CMakeLists.txt` | Add `src/query/QueryableFactory.cpp` to `libmldp_pvxs_driver` |

---

## Step-by-step

### 1. `include/query/IQueryable.h`

After the closing `};` of `class IQueryable`, add:
```cpp
using IQueryableUPtr = std::unique_ptr<IQueryable>;
```
Mirrors `IWriterUPtr` in `include/writer/IWriter.h`.

---

### 2. `include/query/QueryableFactory.h` (new file)

Mirrors `WriterFactory.h` exactly:

```cpp
#pragma once

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <query/IQueryable.h>
#include <util/factory/Factory.h>

#include <memory>
#include <string>

namespace mldp_pvxs_driver::query {

class QueryableFactory
    : public util::factory::Factory<
          QueryableFactory,
          IQueryable,
          const config::Config&,
          std::shared_ptr<metrics::Metrics>>
{
public:
    static constexpr std::string_view kTypeName = "queryable";

    static IQueryableUPtr create(
        const std::string&                type,
        const config::Config&             cfg,
        std::shared_ptr<metrics::Metrics> metrics = nullptr);
};

template <typename QueryableT>
class QueryableRegistrator
{
public:
    explicit QueryableRegistrator(const char* typeName)
    {
        QueryableFactory::registerType(
            typeName,
            [](const config::Config&             cfg,
               std::shared_ptr<metrics::Metrics> metrics)
            {
                return std::make_unique<QueryableT>(cfg, std::move(metrics));
            });
    }
};

#define REGISTER_QUERYABLE(TYPE_STRING, CLASSNAME) \
    static inline ::mldp_pvxs_driver::query::QueryableRegistrator<CLASSNAME> registrator_{TYPE_STRING};

} // namespace mldp_pvxs_driver::query
```

---

### 3. `src/query/QueryableFactory.cpp` (new file)

```cpp
#include <query/QueryableFactory.h>

using namespace mldp_pvxs_driver::query;

IQueryableUPtr QueryableFactory::create(
    const std::string&                type,
    const config::Config&             cfg,
    std::shared_ptr<metrics::Metrics> metrics)
{
    return Factory::create(type, cfg, std::move(metrics));
}
```

---

### 4. `include/query/impl/mldp/MLDPQueryClient.h`

Add include:
```cpp
#include <query/QueryableFactory.h>
```

Add constructor (before destructor):
```cpp
// Factory-compatible constructor — parses MLDPGrpcPoolConfig from cfg.
explicit MLDPQueryClient(const config::Config&             cfg,
                         std::shared_ptr<metrics::Metrics> metrics = nullptr);
```

Add `REGISTER_QUERYABLE` inside the class body (after existing members):
```cpp
    REGISTER_QUERYABLE("mldp", MLDPQueryClient)
```

---

### 5. `src/query/MLDPQueryClient.cpp`

Add delegating constructor (delegates to existing `MLDPGrpcPoolConfig` ctor):
```cpp
MLDPQueryClient::MLDPQueryClient(const config::Config&             cfg,
                                 std::shared_ptr<metrics::Metrics> metrics)
    : MLDPQueryClient(util::pool::MLDPGrpcPoolConfig(cfg), std::move(metrics))
{}
```

`MLDPGrpcPoolConfig` already has `explicit MLDPGrpcPoolConfig(const config::Config& root)`
so no other changes needed in the pool layer.

---

### 6. `CMakeLists.txt`

In the `libmldp_pvxs_driver` source list, add alongside the other query sources:
```cmake
src/query/QueryableFactory.cpp
```

---

## Key existing functions reused

- `util::factory::Factory<>` — `include/util/factory/Factory.h` (generic CRTP, no changes)
- `MLDPGrpcPoolConfig(const config::Config&)` — `include/pool/MLDPGrpcPoolConfig.h:98`
- Existing `MLDPQueryClient(const MLDPGrpcPoolConfig&, ...)` — kept; new ctor delegates to it

---

## What is NOT changed

- Existing direct-construction call sites (`make_shared<MLDPQueryClient>(pool_cfg)`) stay valid.
- `MLDPGrpcPoolConfig` is unchanged.
- Tests in `test/writer/mldp/mldp_writer_integration_test.cpp` still compile (they include
  the header directly and construct with `MLDPGrpcPoolConfig`).

---

## Verification

1. **Compile**: `cmake --build build` — expect clean build with new factory files.
2. **Existing tests pass**: `ctest --test-dir build` — no regressions.
3. **Factory registration**: confirm `QueryableFactory::registeredTypes()` returns `{"mldp"}`.
4. **Unknown type throws**: `QueryableFactory::create("bad-type", cfg)` must throw
   `std::runtime_error("Unknown queryable type: bad-type")`.
