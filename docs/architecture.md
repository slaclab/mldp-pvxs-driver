# MLDP PVXS Driver Architecture

## Overview

The MLDP PVXS Driver is a high-performance data ingestion system that bridges various data sources with the MLDP (Machine Learning Data Platform) service. It uses a push-based architecture designed for minimal latency and maximum throughput.

The driver implements an **abstract Reader pattern** that allows plugging in different data sources. Currently implemented are EPICS-based readers, with the architecture designed to support future implementations such as EPICS Archiver, HDF5 files, and other data sources.

## High-Level Architecture

```mermaid
flowchart TB
    subgraph DataSources["DATA SOURCES"]
        DS1["EPICS Control System<br/>(PVs/Process Variables)"]
        DS2["EPICS Archiver<br/>(Future)"]
        DS3["HDF5 Files<br/>(Future)"]
        DS4["Others<br/>(Future)"]
    end

    subgraph ReaderLayer["ABSTRACT READER LAYER — all readers inherit IReader"]
        R1["EpicsBaseReader<br/>(Polling)<br/>Monitor Poller · Thread Pool"]
        R2["EpicsPVXSReader<br/>(Event-Driven)<br/>PVXS Subscriptions"]
        R3["ArchiverReader<br/>(Future)<br/>Archiver API"]
        R4["HDF5Reader<br/>(Future)<br/>File Parsing"]
    end

    IDataBus["IDataBus<br/>(Push Interface)"]

    subgraph Controller["MLDPPVXSController"]
        HashPart["Hash-Based Partitioning<br/>(Source Affinity)"]
        subgraph Workers["Worker Queues"]
            W0["Worker 0<br/>Queue"]
            W1["Worker 1<br/>Queue"]
            WN["Worker N<br/>Queue"]
        end
    end

    WriterFactory["WriterFactory<br/>(Static Registration)"]

    subgraph WriterLayer["WRITER LAYER — all writers implement IWriter"]
        WR1["MLDPWriter<br/>(gRPC)<br/>Thread Pool · WorkerChannels"]
        WR2["HDF5Writer<br/>(Disk)<br/>MPSC Queue · Flush Thread"]
    end

    MLDPService["MLDP Ingestion Service<br/>(gRPC Streams)"]
    HDF5Files["HDF5 Files<br/>(Local Disk)"]

    DS1 --> R1
    DS1 --> R2
    DS2 --> R3
    DS3 --> R4
    DS4 --> R4

    R1 --> IDataBus
    R2 --> IDataBus
    R3 --> IDataBus
    R4 --> IDataBus

    IDataBus --> HashPart
    HashPart --> W0
    HashPart --> W1
    HashPart --> WN

    W0 --> WriterFactory
    W1 --> WriterFactory
    WN --> WriterFactory

    WriterFactory --> WR1
    WriterFactory --> WR2

    WR1 --> MLDPService
    WR2 --> HDF5Files
```

## Reader Abstraction

The driver uses a **factory pattern** with abstract readers to support multiple data sources:

Reader Type      | Status      | Description
---------------- | ----------- | ---------------------------------------
`epics-base`     | Implemented | Polling-based EPICS Channel Access
`epics-pvxs`     | Implemented | Event-driven EPICS PVAccess (PVXS)
`epics-archiver` | Future      | Historical data from EPICS Archiver
`hdf5`           | Future      | Data replay from HDF5 files
Others           | Future      | Extensible for new data sources

All readers:

- Inherit from the abstract `Reader` base class
- Register via `REGISTER_READER` macro
- Push events through `IDataBus` interface
- Are decoupled from writer implementation
- Are mirrored on the writer side by `WriterFactory` and `REGISTER_WRITER`

For details on existing readers, see [Reader Types](readers.md). To implement a custom reader, see [Implementing Custom Readers](readers-implementation.md).

## Writer Abstraction

Writers are the **output side** of the pipeline. They consume `IDataBus::EventBatch` objects from worker queues and deliver data to a sink.

Writer Type | Status      | Description
----------- | ----------- | -------------------------------------------
`mldp`      | Implemented | Streams data to MLDP ingestion service (gRPC)
`hdf5`      | Implemented | Writes data to rotated HDF5 files on disk

All writers:

- Implement the `IWriter` pure abstract interface (`include/writer/IWriter.h`)
- Register via `REGISTER_WRITER` macro (static init, before `main`)
- Are instantiated and managed by `WriterFactory`
- Receive `EventBatch` via thread-safe `push()` method

```
IWriter  (pure abstract)
├── MLDPWriter  → gRPC → MLDP ingestion service
└── HDF5Writer  → HDF5 files on local disk
```

### MLDPWriter

- Type key: `"mldp"`
- Owns `MLDPWriter` (connection pool)
- N worker threads, each with own `WorkerChannel` (mutex + deque)
- `push()` distributes frames across workers
- Flushes gRPC stream on `stream-max-bytes` or `stream-max-age-ms`

### HDF5Writer

- Type key: `"hdf5"`
- Requires build flag: `MLDP_PVXS_HDF5_ENABLED`
- One HDF5 file per `root_source`, managed by `HDF5FilePool`
- Bounded MPSC queue (capacity 8192) drained by writer thread
- Dedicated flush thread calls `HDF5FilePool::flushAll()` every `flush-interval-ms`
- Files rotate on age (`max-file-age-s`) or size (`max-file-size-mb`)
- HDF5 layout: `timestamps` dataset (int64, ns-epoch) + one dataset per DataFrame column (unlimited + chunked)

## Push Model Architecture

The driver implements a **high-performance push model** that decouples readers from the ingestion pipeline. This architecture ensures readers can continue monitoring PVs without waiting for gRPC writes to complete.

### Push Flow

1. **Event Detection**: Readers detect PV changes (via subscriptions or polling)
2. **Immediate Push**: Call `bus_->push(EventBatch)` immediately (non-blocking)
3. **Hash Partitioning**: Controller partitions events by source name hash
4. **Queue Distribution**: Events are enqueued to per-worker channels
5. **Async Batching**: Workers asynchronously batch and flush to MLDP

### Source-Affinity Hash Partitioning

The controller uses hash-based partitioning to ensure efficient stream utilization:

```cpp
auto idx = std::hash<std::string>{}(src_name) % channels_.size();
per_channel[idx].emplace_back(src_name, std::move(events));
```

**Benefits:**

- Same source always routes to same worker (stream coherence)
- Different sources can use different workers (parallelism)
- Hash distribution provides automatic load balancing
- Stream affinity enables efficient batching

### Per-Worker Architecture

Each worker maintains its own queue and gRPC stream:

```
WorkerChannel {
    mutex              // Protects queue access
    condition_variable // Signals queue has items
    deque<QueueItem>   // Batched work items
    shutdown flag      // Graceful stop signal
}
```

**Worker Loop Lifecycle:**

1. Block on condition variable with timeout (enables idle detection)
2. Dequeue item (source + columns)
3. Build single gRPC `IngestDataRequest`
4. Write to stream (client-streaming RPC)
5. Manage stream rotation based on thresholds

### Stream Rotation

Streams are rotated based on:

- **max_bytes**: Stream reached byte threshold (default: ~2MB)
- **max_age**: Stream exceeded age limit (default: 200ms)
- **write_failed**: gRPC write error occurred
- **idle**: No activity for max_age duration
- **shutdown**: Controller stopping

## Multithreading Model

### Three-Tier Thread Pool Architecture

```mermaid
flowchart TB
    subgraph ReaderPools["Reader Thread Pools"]
        subgraph R1Pool["Reader 1 Pool<br/>(2 threads)"]
            R1Conv["PV Conversion"]
        end
        subgraph R2Pool["Reader 2 Pool<br/>(2 threads)"]
            R2Conv["PV Conversion"]
        end
        subgraph RNPool["Reader N Pool<br/>(2 threads)"]
            RNConv["PV Conversion"]
        end
    end

    subgraph ControllerPool["Controller Worker Thread Pool"]
        subgraph W0["Worker 0"]
            W0Stream["gRPC Stream"]
        end
        subgraph W1["Worker 1"]
            W1Stream["gRPC Stream"]
        end
        subgraph W2["Worker 2"]
            W2Stream["gRPC Stream"]
        end
        subgraph WN["Worker N"]
            WNStream["gRPC Stream"]
        end
    end

    subgraph MonitorPolling["EPICS Base Monitor Polling Threads<br/>(Only used by EpicsBaseReader - polling mode)"]
        subgraph PT1["Poll Thread 1"]
            PT1Drain["drain queues"]
        end
        subgraph PT2["Poll Thread 2"]
            PT2Drain["drain queues"]
        end
    end

    ReaderPools --> ControllerPool
    ControllerPool --> MonitorPolling
```

### Thread Pool Types

Pool                 | Location             | Purpose                        | Default Size
-------------------- | -------------------- | ------------------------------ | ------------
Reader Pool          | Per-Reader           | Convert EPICS data to protobuf | 2 threads
Controller Pool      | MLDPPVXSController   | Process batches, write to gRPC | 2 threads
Monitor Poll Threads | EpicsBaseReader only | Poll EPICS Base queues         | 2 threads

### Conditional Parallelization

The PVXS reader implements smart threading decisions:

```cpp
// Bypass thread pool overhead for single-threaded scenarios
reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr
```

- When thread count is 1: bypass thread pool (direct execution)
- When thread count > 1: use thread pool for parallel conversion

## Event Processing Pipeline

```mermaid
flowchart TB
    RawEvent["Raw EPICS Event"]

    RawEvent --> EPICSBasePath
    RawEvent --> PVXSPath

    subgraph EPICSBasePath["EPICS Base Path"]
        MonitorPoller["Monitor Poller Thread"]
        DrainQueue["Drain Queue<br/>(with mutex protection)"]
        MonitorPoller --> DrainQueue
    end

    subgraph PVXSPath["PVXS Path"]
        SubCallback["Subscription Callback"]
    end

    DrainQueue --> ReaderPool
    SubCallback --> ReaderPool

    ReaderPool["Reader Thread Pool<br/>(detach_task)"]

    ReaderPool --> ProcessEvent

    subgraph ProcessEvent["processEvent()"]
        Timestamp["Timestamp extraction"]
        DataConv["Data conversion<br/>(convertPVToProtoValue)"]
        AlarmMap["Alarm/Status mapping"]
    end

    ProcessEvent --> EventBusPush["IDataBus::push(EventBatch)"]

    EventBusPush --> HashPart

    subgraph HashPart["Controller Hash Partitioning"]
        HashFunc["hash(source_name) % channels.size()"]
    end

    HashPart --> WorkerQueue["Per-Worker Queue<br/>(source affinity)"]

    WorkerQueue --> WorkerLoop

    subgraph WorkerLoop["Worker Loop (Controller Thread Pool)"]
        Dequeue["Dequeue items with timeout"]
        BuildReq["Build gRPC IngestDataRequest"]
        WriteStream["Write to stream<br/>(client-streaming RPC)"]
        Flush["Flush on: max_bytes,<br/>max_age, or shutdown"]
        Dequeue --> BuildReq --> WriteStream --> Flush
    end

    WorkerLoop --> MLDPService["MLDP Ingestion Service"]
```

## Key Design Patterns

### Factory Pattern (ReaderFactory / WriterFactory)

- Runtime reader type selection via YAML configuration
- Static registration via `REGISTER_READER` macro
- Extensible for new reader backends
- Writer selection follows the same pattern via `WriterFactory` and `REGISTER_WRITER`

### Template Method Pattern (EpicsReaderBase)

- Common threading/configuration logic in base class
- Subclasses implement `addPV()` and `processEvent()`

### RAII (PooledHandle)

- Automatic gRPC connection release on handle destruction
- Prevents connection leaks

### Producer-Consumer (Event Bus)

- `IDataBus` interface decouples readers from controller
- `MLDPQueryClient` handles out-of-band metadata/data queries instead of `IDataBus`
- Async event delivery via thread pools
- Workers dispatch `EventBatch` to registered `IWriter` instances (`MLDPWriter`, `HDF5Writer`)
- Optional **reader-to-writer routing** selectively dispatches batches based on config — see [Controller Documentation](controller.md#reader-to-writer-routing)

## Cross-Cutting Utilities

### Logging Abstraction

The driver uses a logging abstraction layer (`util::log`) so library code is not coupled to a specific backend. The executable can install a concrete logger implementation (for example the spdlog-backed adapter).

- Detailed guide: [Logging Abstraction Guide](logging.md)
- Logging interface and helpers: `include/util/log/ILog.h`, `include/util/log/Logger.h`
- Default/simple logger implementation: `include/util/log/CoutLogger.h`, `src/util/log/CoutLogger.cpp`
- spdlog adapter used by the executable: `include/SpdlogLogger.h`, `src/cli/SpdlogLogger.cpp`

### HTTP Transport Provider (`util/http`)

HTTP-based readers can use the shared `util/http` transport abstraction instead of managing raw `libcurl` directly. This centralizes TLS defaults, timeouts, header handling, and streaming callback plumbing.

- Detailed documentation: [HTTP Transport Provider](http-provider.md)

## Configuration

### Controller Settings

The controller config (`MLDPPVXSControllerConfig`) holds five top-level keys — no thread-pool or stream knobs at this level; those live in each writer's config.

```yaml
name: my_controller   # optional; default: "default"; used as Prometheus label 'controller'

writer:           # required; at least one writer instance must be present or controller fails to start
  mldp:
      ...

reader:           # list of reader instances (by type)
  - epics-pvxs:
      ...

routing:          # optional; selective reader-to-writer dispatch
  writer_name:
    from: [reader_1, reader_2]

metrics:          # optional Prometheus / metrics config
  ...
```

> **Note:** `name` scopes all controller-emitted Prometheus metrics under a `controller` label. Run multiple controller instances with distinct names to avoid metric collisions.

→ [Full Controller Documentation](controller.md)

### Reader Settings

```yaml
reader:
  - epics-pvxs:
      - name: my_reader
        thread-pool-size: 2
        pvs:
          - name: PV_NAME
```

### Writer Settings

```yaml
writer:
  mldp:
    - name: mldp_main                  # required, unique instance name
      thread-pool: 4                   # worker threads (default: 1)
      stream-max-bytes: 2097152        # gRPC stream flush threshold (~2MB)
      stream-max-age-ms: 200           # gRPC stream age flush (ms)
      mldp-pool:
        provider-name: my_provider
        ingestion-url: grpc://host:50051
        query-url: grpc://host:50052
        min-conn: 1
        max-conn: 4

  hdf5:                                # requires MLDP_PVXS_HDF5_ENABLED build flag
    - name: hdf5_local                 # required, unique instance name
      base-path: /data/hdf5            # required, output directory
      max-file-age-s: 3600             # rotate after N seconds (default: 3600)
      max-file-size-mb: 512            # rotate at N MiB (default: 512)
      flush-interval-ms: 1000          # flush thread period ms (default: 1000)
      compression-level: 0             # DEFLATE 0–9; 0 = off (default: 0)
```

## Metrics & Observability

The driver exposes Prometheus metrics for monitoring:

### Reader Metrics

- `mldp_pvxs_driver_reader_events_received_total`
- `mldp_pvxs_driver_reader_events_total`
- `mldp_pvxs_driver_reader_errors_total`
- `mldp_pvxs_driver_reader_processing_time_ms`
- `mldp_pvxs_driver_reader_pool_queue_depth`

### Bus Metrics

- `mldp_pvxs_driver_bus_push_total`
- `mldp_pvxs_driver_bus_failure_total`
- `mldp_pvxs_driver_bus_payload_bytes_total`
- `mldp_pvxs_driver_bus_stream_rotations_total`

### Controller Metrics

- `mldp_pvxs_driver_controller_send_time_seconds`
- `mldp_pvxs_driver_controller_queue_depth`
- `mldp_pvxs_driver_controller_channel_queue_depth`

### Pool Metrics

- `mldp_pvxs_driver_pool_connections_in_use`
- `mldp_pvxs_driver_pool_connections_available`
