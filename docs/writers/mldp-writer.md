# MLDP Ingestion Writer

> **Related:** [Writers Overview](../writers-implementation.md) | [Architecture](../architecture.md) | [MLDP Query Client](../query-client.md)

## Overview

`MLDPWriter` forwards event batches to the MLDP ingestion service over gRPC. It registers as type `"mldp"` in the writer factory.

## Internal Architecture

```
push() → round-robin → WorkerChannel[i].deque
                              ↓
                       workerLoop(i)
                              ↓
                   MLDPGrpcIngestionePool
                              ↓
                       gRPC IngestDataRequest
```

- **Worker channels**: each worker owns a `WorkerChannel` (mutex + CV + deque). `push()` selects a channel via atomic round-robin.
- **Thread pool**: `BS::light_thread_pool` with `thread-pool` threads (default: 1).
- **Stream flushing**: each worker flushes the gRPC stream when payload exceeds `stream-max-bytes` or age exceeds `stream-max-age-ms`.
- **Back-pressure**: `push()` returns `false` when the queue is at capacity.

## Configuration

Under `writer.mldp[i]`:

```yaml
writer:
  mldp:
    - name: mldp_main             # required — unique instance name
      thread-pool: 4              # optional; default: 1
      stream-max-bytes: 2097152   # optional; default: 2 MiB
      stream-max-age-ms: 200      # optional; default: 200 ms
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://ingest-host:50051
        query-url:     grpc://query-host:50052
        min-conn: 1
        max-conn: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | Required. Unique writer instance name. |
| `thread-pool` | int | `1` | Concurrent ingestion worker threads. |
| `stream-max-bytes` | size_t | `2097152` | Flush gRPC stream after this payload size (bytes). |
| `stream-max-age-ms` | int | `200` | Flush gRPC stream after this age (milliseconds). |
| `mldp-pool.*` | object | — | Connection pool settings (see `MLDPGrpcPoolConfig`). |

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Registers provider with MLDP service; spawns worker threads. |
| `push(batch)` | Fans out `DataFrames` across worker channels; returns `false` on overflow. |
| `stop()` | Sets shutdown flag on all channels; drains queues; joins thread pool. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp/MLDPWriter.h` | Class definition, `WorkerChannel`, `QueueItem`. |
| `include/writer/mldp/MLDPWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp/MLDPWriter.cpp` | `workerLoop()`, `buildRequest()`, metrics. |

## Metrics

`MLDPWriter` updates queue-depth gauges via `updateQueueDepthMetric()` after each `push()` and worker drain. Metrics are injected as `std::shared_ptr<metrics::Metrics>` at construction.
