# MLDP Annotation Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md) | [Configuration Reference](../guides/configuration.md)

## Overview

`MLDPAnnotationWriter` persists PV source metadata to the MLDP `DpAnnotationService` via gRPC. It registers as type `"mldp-annotation"` in the writer factory.

Only `SourceMetadataPayload` batches are processed; all other payload types are silently ignored (`acceptsPayload()` returns `false` for them).

## Internal Architecture

```
push(SourceMetadataPayload) → expand source entries → WorkItem queue
                                                             ↓
                                                      workerLoop (N threads)
                                                             ↓
                                                  MLDPGrpcAnnotationPool
                                                             ↓
                                                   savePvMetadata RPC
```

- **Work queue**: `push()` fans each source-metadata entry from the payload map into individual `WorkItem`s on an internal `std::queue`.
- **Worker threads**: `thread-pool` threads drain the queue concurrently, each calling `savePvMetadata` on the annotation endpoint.
- **Back-pressure**: `push()` returns `false` when the writer is not running.

## Configuration

Under `writer.mldp-annotation[i]`:

```yaml
writer:
  mldp-annotation:
    - name: annotation_main         # required — unique instance name
      thread-pool: 2                # optional; default: 2
      deadline-seconds: 10          # optional; default: 10
      mldp-annotation-pool:         # required
        annotation-url: grpc://annotation-host:50053  # required
        min-conn: 1                 # optional; default: 1
        max-conn: 4                 # optional; default: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `thread-pool` | int | `2` | Worker threads draining the internal work queue. |
| `deadline-seconds` | int | `10` | Per-RPC deadline in seconds. |
| `mldp-annotation-pool.annotation-url` | string | — | **Required.** gRPC endpoint for the annotation service. |
| `mldp-annotation-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-annotation-pool.max-conn` | int | `4` | Maximum open connections in the pool. |

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Initializes `MLDPGrpcAnnotationPool`; spawns `thread-pool` worker threads. |
| `push(batch)` | If payload is `SourceMetadataPayload`: expands each `{source → SourceMetadataEntry}` pair into a `WorkItem` and enqueues it. All other payload types: no-op. Returns `false` when not running. |
| `stop()` | Sets stop flag; notifies all worker threads; joins them. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp_annotation/MLDPAnnotationWriter.h` | Class definition, `WorkItem`. |
| `include/writer/mldp_annotation/MLDPAnnotationWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp_annotation/MLDPAnnotationWriter.cpp` | `workerLoop()`, `saveSourceMetadata()`. |
| `src/writer/mldp_annotation/MLDPAnnotationWriterConfig.cpp` | YAML parsing. |
