# Writers — Implementation Guide

> **Related:** [Architecture Overview](architecture.md) | [Reader Implementations](readers-implementation.md) | [MLDP Query Client](query-client.md)

## Overview

Writers are the sink-side components of the MLDP PVXS Driver. They receive `EventBatch` objects from the bus and forward them to a backend. The driver uses an **abstract Writer pattern** with a factory-based registration system; new writer types can be added without modifying the controller's dispatch logic.

## Writer Interface

All writers implement `IWriter`:

```cpp
class IWriter {
public:
    virtual ~IWriter() = default;
    virtual std::string name() const = 0;
    virtual void start() = 0;
    virtual bool push(util::bus::IDataBus::EventBatch batch) noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual bool isHealthy() const noexcept { return true; }
};
```

### Lifecycle

1. Construct from configuration.
2. `start()` — allocate runtime resources (threads, connections).
3. `push()` — called from controller/bus threads as batches arrive.
4. `stop()` — drain work and release resources.

### Threading Contract

- `push()` must be safe to call concurrently.
- `start()` and `stop()` are single-owner lifecycle operations.
- `push()` returns `false` when applying back-pressure or dropping a batch.

## Registration System

Writers register at static-initialization time via `REGISTER_WRITER`:

```cpp
class MyWriter final : public IWriter {
    REGISTER_WRITER("my-type", MyWriter)
    ...
};
```

The macro inserts a `static inline WriterRegistrator<T>` member. Before `main()` runs, `WriterRegistrator<MyWriter>` calls `WriterFactory::registerType(...)`.

`WriterFactory::create(type, node, metrics)` then dispatches to the registered constructor.

### Registration Flow

1. `REGISTER_WRITER("my-type", MyWriter)` in class body.
2. `WriterRegistrator<MyWriter>` runs at static init → calls `WriterFactory::registerType(...)`.
3. Controller calls `WriterFactory::create("my-type", node, metrics)` → concrete writer constructed.

## Controller Wiring

`MLDPPVXSControllerConfig::writerEntries()` returns `(type, config-node)` pairs. The controller iterates them:

```cpp
for (const auto& [type, writerNode] : config_.writerEntries()) {
    auto writer = WriterFactory::create(type, writerNode, metrics_);
    ...
}
```

No `#ifdef` branches needed. YAML configuration determines which writers are active.

## Configuration Layout

Writers are configured under the top-level `writer:` block. Each type owns a YAML sequence of instances:

```yaml
writer:
  grpc:
    - name: grpc_main
      mldp-pool:
        provider-name: pvxs_provider
  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
```

`WriterConfig` validates the top-level structure; each concrete writer config parses its own sub-block.

## Existing Writers

| Type | Doc | Header |
|------|-----|--------|
| `grpc` | [gRPC Writer](writers/grpc-writer.md) | `include/writer/grpc/MLDPGrpcWriter.h` |
| `hdf5` | [HDF5 Writer](writers/hdf5-writer.md) | `include/writer/hdf5/HDF5Writer.h` |

---

## Implementing a New Writer

### 1 — Create the config struct

Add `src/writer/my_type/MyWriterConfig.h`:

```cpp
struct MyWriterConfig {
    class Error : public std::runtime_error { using std::runtime_error::runtime_error; };

    std::string name;         // required
    // … type-specific fields …

    static MyWriterConfig parse(const config::Config& node);
};
```

`parse()` must throw `MyWriterConfig::Error` on validation failure.

### 2 — Implement `IWriter`

Add `include/writer/my_type/MyWriter.h`:

```cpp
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/my_type/MyWriterConfig.h>

class MyWriter final : public IWriter {
    REGISTER_WRITER("my-type", MyWriter)
public:
    // Factory constructor — called by WriterFactory
    explicit MyWriter(const config::Config& root,
                      std::shared_ptr<metrics::Metrics> metrics = nullptr);

    // Typed constructor — for unit tests
    explicit MyWriter(MyWriterConfig config,
                      std::shared_ptr<metrics::Metrics> metrics = nullptr);

    std::string name() const override { return config_.name; }
    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;
    bool isHealthy() const noexcept override;

private:
    MyWriterConfig config_;
    // … threads, queues, connections …
};
```

Key rules:
- `push()` must be **noexcept** and **thread-safe**.
- `push()` returns `false` on back-pressure or drop.
- `start()` and `stop()` are called from a single owner; they may throw or log but must not leave dangling threads.

### 3 — Register with CMake

Add the new `.cpp` files to `CMakeLists.txt`:

```cmake
target_sources(mldp_pvxs_driver PRIVATE
    src/writer/my_type/MyWriter.cpp
    src/writer/my_type/MyWriterConfig.cpp
)
```

If the writer is optional (e.g. depends on an external library), guard with a CMake option and `#ifdef`:

```cmake
option(MLDP_PVXS_MYTYPE_ENABLED "Enable MyType writer" OFF)
if(MLDP_PVXS_MYTYPE_ENABLED)
    target_compile_definitions(mldp_pvxs_driver PRIVATE MLDP_PVXS_MYTYPE_ENABLED)
    target_sources(mldp_pvxs_driver PRIVATE src/writer/my_type/MyWriter.cpp)
    target_link_libraries(mldp_pvxs_driver PRIVATE my_type_lib)
endif()
```

Wrap the class body and `REGISTER_WRITER` inside the same `#ifdef MLDP_PVXS_MYTYPE_ENABLED` guard so the factory type is absent when the option is off.

### 4 — Extend YAML parsing

In `WriterConfig` and `MLDPPVXSControllerConfig`, add a sub-block for the new type under `writer:`:

```yaml
writer:
  my-type:
    - name: my_instance
      # … type-specific keys …
```

Update `WriterConfig::parse()` to recognise `"my-type"` and forward the node to `MyWriterConfig::parse()`.

### 5 — Add a doc page

Create `docs/writers/my-type-writer.md` following the pattern in [grpc-writer.md](writers/grpc-writer.md):
- Overview / architecture diagram
- Config table with all YAML keys, types, defaults
- Lifecycle table
- Threading notes
- Key files table

Link it from the table in the **Existing Writers** section above.

### Checklist

- [ ] `IWriter` fully implemented (`name`, `start`, `push`, `stop`, `isHealthy`)
- [ ] `push()` is `noexcept` and thread-safe
- [ ] Factory constructor signature matches `(const config::Config&, std::shared_ptr<metrics::Metrics>)`
- [ ] `REGISTER_WRITER("my-type", MyWriter)` inside class body
- [ ] Sources added to `CMakeLists.txt`
- [ ] YAML parsing updated in `WriterConfig` / `MLDPPVXSControllerConfig`
- [ ] Unit tests for config parsing and lifecycle
- [ ] Doc page added and linked here
