# MLDP PV Metadata Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md) | [Configuration Reference](../guides/configuration.md)

## Overview

`MLDPPVMetadataWriter` persists PV source metadata to the MLDP `DpAnnotationService` via gRPC. It registers as type `"mldp-pv-metadata"` in the writer factory.

Only `SourceMetadataPayload` batches are processed; all other payload types are silently ignored (`acceptsPayload()` returns `false` for them).

## Internal Architecture

```
push(SourceMetadataPayload) → toItems() fan-out → one WorkItem per source entry
                                                             ↓
                                          BaseQueuedWriter (bounded MPSC queue, back-pressure)
                                                             ↓
                                             processItem(workerIndex, WorkItem)
                                                             ↓
                                                  MLDPGrpcAnnotationPool
                                                             ↓
                                                   savePvMetadata RPC
```

`MLDPPVMetadataWriter` inherits `BaseQueuedWriter<WorkItem>` (where `WorkItem` is `std::pair<std::string, SourceMetadataEntry>`) and supplies two hooks:

- **`toItems()`** — extracts `SourceMetadataPayload`; fans out each `{source → SourceMetadataEntry}` pair into an individual `WorkItem`; returns empty vector for other payload types.
- **`processItem()`** — calls `saveSourceMetadata(source_name, entry)` to issue the `savePvMetadata` gRPC RPC.

Queue and thread management:

- **Bounded queue**: capacity 1000 items.
- **Worker threads**: `thread-pool` threads (default: 2) drain the queue concurrently.
- **Back-pressure**: `push()` blocks when the queue is full and returns `false` only when the writer is stopping.

## Configuration

Under `writer.mldp-pv-metadata[i]`:

```yaml
writer:
  mldp-pv-metadata:
    - name: pv_metadata_main          # required — unique instance name
      thread-pool: 2                  # optional; default: 2
      deadline-seconds: 10            # optional; default: 10
      mldp-pv-metadata-pool:          # required
        annotation-url: grpc://annotation-host:50053  # required
        min-conn: 1                   # optional; default: 1
        max-conn: 4                   # optional; default: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `thread-pool` | int | `2` | Worker threads draining the internal work queue. |
| `deadline-seconds` | int | `10` | Per-RPC deadline in seconds. |
| `mldp-pv-metadata-pool.annotation-url` | string | — | **Required.** gRPC endpoint for the annotation service. |
| `mldp-pv-metadata-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-pv-metadata-pool.max-conn` | int | `4` | Maximum open connections in the pool. |

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Base class starts thread pool; calls `doStart()` → initializes `MLDPGrpcAnnotationPool`. |
| `push(batch)` | Base `toItems()` hook: fans out source entries into `WorkItem`s; empty for non-metadata payloads. Enqueued items block caller if queue is full; returns `false` only when stopping. |
| `stop()` | Base class drains queue, joins workers; calls `doStop()` → releases pool. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp_pv_metadata/MLDPPVMetadataWriter.h` | Class definition; `WorkItem` alias; `toItems`/`processItem`/`doStart`/`doStop` overrides. |
| `include/writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp_pv_metadata/MLDPPVMetadataWriter.cpp` | `toItems()`, `processItem()`, `saveSourceMetadata()`. |
| `src/writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.cpp` | YAML parsing. |
