# TODO-06: Add QueryableHolder and QueryableFactory (header-only, no breaking changes)

## Goal
Create two new header-only files. No existing code changes yet — pure additions.
This sets up the factory infrastructure before IQueryable is stripped in TODO-07.

## Depends On
Nothing. Pure additions.

## Files to Create

### `include/query/QueryableHolder.h` (NEW)
```cpp
//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// ...license header...
//////////////////////////////////////////////////////////////////////////////
#pragma once
#include <query/IQueryable.h>
#include <memory>

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

} // namespace mldp_pvxs_driver::query
```

Note: `IQueryableUPtr` alias must exist on `IQueryable.h` before this compiles.
Add `using IQueryableUPtr = std::unique_ptr<IQueryable>;` to `IQueryable.h` now
(this does NOT break anything — it's an additive change).

### `include/query/QueryableFactory.h` (NEW)
```cpp
//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// ...license header...
//////////////////////////////////////////////////////////////////////////////
#pragma once
#include <query/IQueryable.h>
#include <config/Config.h>
#include <metrics/Metrics.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace mldp_pvxs_driver::query {

class QueryableFactory {
public:
    static QueryableFactory& instance() {
        static QueryableFactory inst;
        return inst;
    }

    // Startup: bind type T to its config. Must complete before any create<T>() call.
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

    // Runtime: create instance of T. Returns unique_ptr<T> directly.
    // static_cast is safe: the closure stored by prepare<T> always produces a T.
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

    template <typename T>
    bool isPrepared() const {
        std::shared_lock lock(mutex_);
        return creators_.count(std::type_index(typeid(T))) > 0;
    }

    // Test helper: clear all registered creators.
    void reset() {
        std::unique_lock lock(mutex_);
        creators_.clear();
    }

private:
    QueryableFactory() = default;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::type_index, std::function<IQueryableUPtr()>> creators_;
};

} // namespace mldp_pvxs_driver::query
```

### `include/query/IQueryable.h` (additive change only)
Add `IQueryableUPtr` alias after the class definition:
```cpp
using IQueryableUPtr = std::unique_ptr<IQueryable>;
```
Do NOT remove existing virtual methods yet — that is TODO-07.

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# All existing tests still pass (no behavior change)
ctest --test-dir build -V 2>&1 | tail -20
```

## Commit
```
feat(query): add QueryableFactory and QueryableHolder headers

Introduce header-only QueryableFactory (Meyer's singleton, prepare/create
pattern) and QueryableHolder (typed downcast wrapper). IQueryable gains the
IQueryableUPtr alias. No existing behavior changed.
```
