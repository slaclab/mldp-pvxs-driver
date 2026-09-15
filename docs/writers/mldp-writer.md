# MLDP Ingestion Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md) | [MLDP Query Client](../dev/query-client.md)

## Overview

`MLDPWriter` forwards event batches to the MLDP ingestion service over gRPC. It registers as type `"mldp"` in the writer factory.

## Build Option & Required Libraries

- **Build option:** none (always built)
- **Required libraries/components:**
  - gRPC (`gRPC::grpc++`)
  - Protobuf (`protobuf::libprotobuf`)
  - OpenSSL (`OpenSSL::SSL`, `OpenSSL::Crypto`) for TLS credentials
  - dp-grpc protobuf definitions (fetched at configure time)

## Internal Architecture

```
push() ──► BaseQueuedWriter (bounded MPSC queue, back-pressure)
                   │  round-robin across worker channels
                   ▼
           processItem(workerIndex, MLDPQueueItem)
                   │
                   ▼
        MLDPGrpcIngestionePool
                   │
                   ▼
        gRPC IngestDataRequest (bidi stream)
```

`MLDPWriter` inherits `BaseQueuedWriter<MLDPQueueItem>` and supplies two hooks:

- **`toItems()`** — extracts `TimeSeriesPayload` frames from the batch; returns one `MLDPQueueItem` per frame (source name + metadata + `DataBatch`).
- **`processItem()`** — per-worker handler; opens/reuses a per-worker gRPC bidi-stream via `ensureStream()`, builds the protobuf request, writes it, and flushes when `stream-max-bytes` or `stream-max-age-ms` is exceeded.

Queue and thread management:

- **Worker channels**: the base class owns per-worker channels (mutex + CV + deque). `push()` selects a channel via atomic round-robin.
- **Thread pool**: `BS::light_thread_pool` with `thread-pool` threads (default: 1).
- **Back-pressure**: `push()` blocks indefinitely when the total queued items across all worker channels reaches `queue-capacity`. It only returns `false` when the writer is stopping (`stop()` or `forceStop()` called).

## Configuration

Under `writer.mldp[i]`:

```yaml
writer:
  mldp:
    - name: mldp_main             # required — unique instance name
      thread-pool: 4              # optional; default: 1
      stream-max-bytes: 2097152   # optional; default: 2 MiB
      stream-max-age-ms: 200      # optional; default: 200 ms
      queue-capacity: 10000       # optional; default: 10000
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://ingest-host:50051
        # query-url:   grpc://query-host:50052   # optional; not used by ingestion writer
        min-conn: 1
        max-conn: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | Required. Unique writer instance name. |
| `thread-pool` | int | `1` | Concurrent ingestion worker threads. |
| `stream-max-bytes` | size_t | `2097152` | Flush gRPC stream after this payload size (bytes). |
| `stream-max-age-ms` | int | `200` | Flush gRPC stream after this age (milliseconds). |
| `queue-capacity` | size_t | `10000` | Max queued items across all worker channels before push blocks indefinitely. |
| `mldp-pool.*` | object | — | Connection pool settings (see `MLDPGrpcPoolConfig`). |

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Registers provider with MLDP service; spawns worker threads. |
| `push(batch)` | Extracts `TimeSeriesPayload` frames; fans out across worker channels via round-robin. Blocks indefinitely when queue at capacity; returns `false` only when `stop()` or `forceStop()` is called. Batch `metadata` map is forwarded to `buildRequest()` and stamped on each `ColumnProvenance.source` label in the gRPC request. |
| `stop()` | Sets `running_=false`, wakes blocked producers (push returns `false`), workers drain remaining queued items, then thread pool joins. |
| `forceStop()` | Sets `forceQuit_=true`, wakes blocked producers and workers. Workers break immediately without draining — queue contents are discarded. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp/MLDPWriter.h` | Class definition, `WorkerChannel`, `QueueItem`. |
| `include/writer/mldp/MLDPWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp/MLDPWriter.cpp` | `toItems()`, `processItem()`, `buildRequest()`, metrics. |

## Connection Pool and Kubernetes Load Balancing

Each `MLDPGrpcIngestionePool` object opens its own TCP connection to the ingest server. gRPC HTTP/2 would normally multiplex all streams over a single shared subchannel when multiple channels point at the same target URL, causing all traffic to land on the same backend pod behind a Layer-4 load balancer (MetalLB, kube-proxy IPVS/iptables). The pool prevents this by setting `GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL=1` and a unique `GRPC_ARG_CHANNEL_ID` on every channel it creates. Each pool object therefore opens a distinct TCP connection, and kube-proxy hashes the unique `(src_ip, src_port, dst_ip, dst_port)` tuple to a different backend pod.

In practice: with `max-conn: 8` and a multi-replica MLDP deployment behind a `LoadBalancer` service, all 8 workers spread across replicas instead of saturating one.

## Metrics

`MLDPWriter` updates queue-depth gauges via `updateQueueDepthMetric()` after each `push()` and worker drain. Throughput metrics (`data_bytes_per_second`, `payload_bytes_per_second`) use a wall-clock windowed accumulator per source: bytes are summed and flushed every ≥1 s of wall time, giving accurate real-time ingest throughput regardless of the acquisition timestamp spacing in the data. Metrics are injected as `std::shared_ptr<metrics::Metrics>` at construction.
