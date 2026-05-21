# MLDP Configuration Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md) | [Configuration Reference](../guides/configuration.md)

## Overview

`MLDPConfigurationWriter` persists configuration objects and activation windows to the MLDP `DpAnnotationService` via gRPC. It registers as type `"mldp-configuration"` in the writer factory.

Only `ConfigurationPayload` and `ConfigurationActivationPayload` batches are processed; all other payload types are silently ignored (`acceptsPayload()` returns `false` for them).

## Internal Architecture

```
push(ConfigurationPayload | ConfigurationActivationPayload)
                    ↓
              WorkItem queue
                    ↓
           workerLoop (N threads)
                    ↓
      MLDPGrpcAnnotationPool
           /               \
  saveConfiguration   saveConfigurationActivation
```

- **Work queue**: `push()` wraps the active payload variant in a `WorkItem` and enqueues it.
- **Worker threads**: `thread-pool` threads drain the queue; each item dispatches to the appropriate RPC based on the `WorkData` variant.
- **Back-pressure**: `push()` returns `false` when the writer is not running.

## Configuration

Under `writer.mldp-configuration[i]`:

```yaml
writer:
  mldp-configuration:
    - name: cfg_writer              # required — unique instance name
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
| `push(batch)` | If payload is `ConfigurationPayload` or `ConfigurationActivationPayload`: wraps in `WorkItem` and enqueues. All other payload types: no-op. Returns `false` when not running. |
| `stop()` | Sets stop flag; notifies all worker threads; joins them. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp_configuration/MLDPConfigurationWriter.h` | Class definition, `WorkItem`, `WorkData` variant. |
| `include/writer/mldp_configuration/MLDPConfigurationWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp_configuration/MLDPConfigurationWriter.cpp` | `workerLoop()`, `doSaveConfiguration()`, `doSaveConfigurationActivation()`. |
| `src/writer/mldp_configuration/MLDPConfigurationWriterConfig.cpp` | YAML parsing. |
