# TODO: IQueryable Abstraction

## Goal

Extract a pure interface from `MLDPQueryClient` so query consumers depend on an
abstraction, not a concrete gRPC implementation. Enables alternative backends
(mock, REST, archiver-based) without touching existing consumers.

## Motivation

`MLDPQueryClient` is currently the only query implementation and is injected
directly by concrete type. Adding a second query backend would require forking
call sites. An interface breaks that coupling.

## Plan

### 1. Define `IQueryable` interface

**New file:** `include/query/IQueryable.h`

```cpp
namespace mldp_pvxs_driver::query {

class IQueryable {
public:
    virtual ~IQueryable() = default;

    virtual std::vector<util::bus::IDataBus::SourceInfo>
    querySourcesInfo(const std::set<std::string>& source_names) = 0;

    virtual std::optional<
        std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>
    querySourcesData(const std::set<std::string>&   source_names,
                     const QuerySourcesDataOptions& options) = 0;
};

} // namespace mldp_pvxs_driver::query
```

### 2. Refactor `MLDPQueryClient`

- `include/query/MLDPQueryClient.h`: inherit `IQueryable`, mark overrides
- `src/query/MLDPQueryClient.cpp`: no signature changes expected

### 3. Update call sites

- Find all places that hold or construct `MLDPQueryClient` directly
- Replace concrete type with `std::shared_ptr<IQueryable>` (or `unique_ptr`)
- Inject via constructor / factory

### 4. (Optional) Add factory

Consider `QueryClientFactory` mirroring `WriterFactory`/`ReaderFactory` if
multiple backends need runtime selection via config.

## Files to Touch

| File | Change |
|------|--------|
| `include/query/IQueryable.h` | NEW — pure interface |
| `include/query/MLDPQueryClient.h` | inherit `IQueryable` |
| `src/query/MLDPQueryClient.cpp` | add `override` keywords |
| Call sites (TBD via grep) | swap concrete → interface pointer |

## Acceptance Criteria

- `MLDPQueryClient` compiles as `IQueryable` implementation
- All consumers accept `IQueryable&` or `shared_ptr<IQueryable>`
- No call site includes `MLDPQueryClient.h` directly (only via factory/injection)
- A mock `IQueryable` can be constructed in tests without gRPC
