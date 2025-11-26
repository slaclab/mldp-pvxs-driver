# CLI Architecture Walkthrough
![Architecture](full-architecture.png)
The diagram above represents the data flow the CLI orchestrates. Use these checkpoints when navigating or extending the pipeline:
- **MLDP connections**: Each configured MLDP target establishes its own gRPC channel and feeds the connection pool. Failures are isolated per connection, so retries or backoffs should be implemented at this edge.
- **Connection pool**: The pool multiplexes active channels and exposes a uniform API to the ingestor. Any throttling, health checking, or reuse policies belong here.
- **MLDP ingestor**: This component owns the `PVXSDPIngestionDriver`; it consumes updates from PVXS, enriches them, and pushes canonical events into the internal queue. It should stay stateless aside from bookkeeping identifiers returned by the service.
- **Queue**: A bounded work queue decouples ingestion from downstream readers. Tune its depth to match the slowest reader you plan to support.
- **Reader abstraction**: The queue fan-outs into the registered readers through the factory described above. Each reader (e.g., `EpicsReader`, synthetic readers, etc.) subscribes to the bus interface and can perform protocol-specific transformation or output.
- **Extending the CLI**: When adding a new external system, keep the connection-specific logic left of the queue and terminate in a reader implementation on the right. This ensures the rest of the CLI keeps the same contract regardless of how many integrations you ship.


## Reader Abstraction
```mermaid
classDiagram
    direction LR

    class IEventBusPush {
        <<interface>>
        +bool push()
    }

    class Reader {
        <<abstract>>
        -shared_ptr~IEventBusPush bus_
        +Reader(shared_ptr~IEventBusPush~ bus)
        +string name()
    }

    Reader --> IEventBusPush : uses

    class Config {
        +ryml::ConstNodeRef node_
        +string get(key, default)
        +int getInt(key, default)
    }

    class ReaderFactory {
        -static unordered_map~string, CreatorFn~ registry
        +static void registerType(string type, CreatorFn fn)
        +static unique_ptr~Reader~ create(string type, shared_ptr~IEventBusPush~, ReaderConfig cfg)
    }

    ReaderFactory --> ReaderConfig
    ReaderFactory --> Reader : creates

    class ReaderRegistrator~T~ {
        +ReaderRegistrator(string typeName)
    }

    ReaderRegistrator~T~ --> ReaderFactory : registers type

    class REGISTER_READER {
        <<macro>>
    }

    note for REGISTER_READER "REGISTER_READER(TYPE, CLASS) -> static inline ReaderRegistrator<CLASS> registrator_{TYPE};"

    class EpicsReader {
        +EpicsReader(shared_ptr~IEventBusPush~, ReaderConfig)
        +string name()
        +void start()
        +void stop()
    }

    EpicsReader --|> Reader : implements
    EpicsReader .. REGISTER_READER : uses macro
```
- **Event bus boundary**: `include/bus/IEventBusPush.h` defines the narrow interface that any reader implementation can use to push decoded events back into the driver. Readers never talk to PVXS or gRPC directly; they receive a shared pointer to `mldp_pvxs_driver::bus::IEventBusPush` in their constructor.
- **Base contract**: `include/reader/Reader.h` provides the `Reader` abstract base class plus a lightweight `ReaderConfig` struct you can extend as the configuration surface grows. Every reader must supply a `name()`, `start()`, and `stop()` implementation and is responsible for owning any worker threads it spins up.
- **Factory & registration**: `include/reader/ReaderFactory.h(.cpp)` exposes `ReaderFactory::registerType` and `ReaderFactory::create`. Implementations register themselves through the `REGISTER_READER("type", ClassName)` macro, which instantiates a static `ReaderRegistrator` that hooks into the factory at load time.
- **Concrete example**: `include/src/reader/impl/epics/EpicsReader.{h,cpp}` shows a skeleton EPICS-backed reader. It stores the bus pointer in the base class, spawns a worker thread in the constructor, and cleans it up in the destructor. Use this file as the starting point for additional reader types (e.g., channel archiver, simulation, file replay).
- **Adding a reader**:
  1. Create `include/reader/impl/<name>/<Name>Reader.h` derived from `Reader`, include the macro, and define any extra configuration knobs.
  2. Implement the behavior in `src/reader/impl/<name>/<Name>Reader.cpp` and make sure the TU is part of `libmldp_pvxs_driver` in `CMakeLists.txt`.
  3. Invoke `ReaderFactory::create("<type>", bus, cfg)` from the orchestration layer to spin up the new reader; the factory throws if the type string is unknown, which protects against misconfigured YAML.


## Tips
- If you hit linker or dependency issues, double-check that `PROTO_PATH` and `PVXS_BASE` correctly point to the same SDK versions referenced in this repository’s Docker/dev container setup.
- Keep configuration changes (e.g., YAML driver configs) separate from code logic in their own commits so reviewers can focus on one area per review pass.
