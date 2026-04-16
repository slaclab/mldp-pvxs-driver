# TODO: Factory Base-Class Unification

## Goal

`ReaderFactory` and `WriterFactory` are intentional mirrors that share identical
boilerplate (static registry map, `registerType`, `registeredTypes`, `create`).
Extract a single `Factory<T>` / `FactoryRegistrator<T>` template so future factory
types require zero boilerplate and the registry logic lives in one place.

---

## Tasks

### 1. Design `include/util/factory/Factory.h`

```cpp
namespace mldp_pvxs_driver::util::factory {

template <typename ProductT, typename... CtorArgs>
class Factory {
public:
    using UPtr      = std::unique_ptr<ProductT>;
    using CreatorFn = std::function<UPtr(CtorArgs...)>;

    static void registerType(const std::string& name, CreatorFn fn);
    static UPtr create(const std::string& name, CtorArgs... args);
    static std::vector<std::string> registeredTypes();

private:
    static std::unordered_map<std::string, CreatorFn>& registry();
};

template <typename FactoryT, typename ConcreteT>
class FactoryRegistrator {
public:
    explicit FactoryRegistrator(const char* name) {
        FactoryT::registerType(name,
            [](auto&&... args) {
                return std::make_unique<ConcreteT>(std::forward<decltype(args)>(args)...);
            });
    }
};

} // namespace
```

Decide on variadic vs. fixed `CtorArgs` — fixed `(const config::Config&, shared_ptr<Metrics>)`
is simpler and covers both readers and writers; document this constraint.

### 2. Refactor `ReaderFactory` to thin wrapper
- File: `include/reader/ReaderFactory.h`, `src/reader/ReaderFactory.cpp`
- Replace the internal map with a specialisation of `Factory<IReader, ...>`
- Keep the public API (`ReaderFactory::create`, `ReaderFactory::registerType`,
  `REGISTER_READER` macro) identical so no call sites change

### 3. Refactor `WriterFactory` to thin wrapper
- File: `include/writer/WriterFactory.h`, `src/writer/WriterFactory.cpp`
- Same treatment as `ReaderFactory`

### 4. Update `REGISTER_READER` / `REGISTER_WRITER` macros
- Both macros should delegate to the shared `FactoryRegistrator` template
- Keep the macro names unchanged for backward compat

### 5. Add `src/util/factory/Factory.cpp` if any out-of-line code is needed
- If the template is fully header-only, no `.cpp` is needed; note this in
  CMakeLists comment

### 6. Tests
- File: `test/util/factory/factory_test.cpp`
- Register two dummy types; verify `create` dispatches correctly
- Verify `create` throws `std::runtime_error` for unknown type
- Verify `registeredTypes()` lists exactly the registered names

---

## Key files

| File | Role |
|---|---|
| `include/reader/ReaderFactory.h` | Current reader factory (to become thin wrapper) |
| `include/writer/WriterFactory.h` | Current writer factory (to become thin wrapper) |
| `src/reader/ReaderFactory.cpp` | Reader factory impl |
| `src/writer/WriterFactory.cpp` | Writer factory impl |
| `include/util/factory/Factory.h` | New shared template (to create) |

---

## Notes

- Do this **after** the HDF5 tests and documentation tasks are complete; the public
  API must not change so existing tests remain green with zero modifications.
- The unification is pure refactor — no behavioral change.
