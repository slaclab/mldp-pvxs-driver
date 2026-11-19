# MLDP Configuration Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md) | [Configuration Reference](../guides/configuration.md)

## Overview

`MLDPConfigurationWriter` persists configuration objects and activation windows to the MLDP `DpAnnotationService` via gRPC. It registers as type `"mldp-configuration"` in the writer factory.

Only `ConfigurationPayload` and `ConfigurationActivationPayload` batches are processed; all other payload types are silently ignored (`acceptsPayload()` returns `false` for them).

## Internal Architecture

```
push(ConfigurationPayload | ConfigurationActivationPayload)
                    ↓
     BaseQueuedWriter (bounded MPSC queue, back-pressure)
                    ↓
         processItem(workerIndex, ConfigItem variant)
                    ↓
        MLDPGrpcAnnotationPool
           /               \
  saveConfiguration   saveConfigurationActivation
```

`MLDPConfigurationWriter` inherits `BaseQueuedWriter<ConfigItem>` (where `ConfigItem` is `std::variant<ConfigurationPayload, ConfigurationActivationPayload>`) and supplies two hooks:

- **`toItems()`** — inspects the batch payload; returns a 1-element vector for `ConfigurationPayload` or `ConfigurationActivationPayload`, empty vector for all other types.
- **`processItem()`** — dispatches via `std::visit` to `doSaveConfiguration()` or `doSaveConfigurationActivation()`.

Queue and thread management:

- **Bounded queue**: capacity 1000 items (configurable via `thread-pool` worker count; queue capacity fixed).
- **Worker threads**: `thread-pool` threads (default: 2) drain the queue concurrently.
- **Back-pressure**: `push()` blocks when the queue is full and returns `false` only when the writer is stopping.

## Configuration

Under `writer.mldp-configuration[i]`:

```yaml
writer:
  mldp-configuration:
    - name: cfg_writer              # required — unique instance name
      thread-pool: 2                # optional; default: 2
      deadline-seconds: 10          # optional; default: 10
      max-retries: 3                # optional; default: 3
      retry-backoff-ms: 500         # optional; default: 500
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
| `max-retries` | int | `3` | Retries after the first attempt for transient failures (`>= 0`). |
| `retry-backoff-ms` | int | `500` | Initial retry delay in ms, doubled per retry (500, 1000, 2000). |
| `mldp-annotation-pool.annotation-url` | string | — | **Required.** gRPC endpoint for the annotation service. |
| `mldp-annotation-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-annotation-pool.max-conn` | int | `4` | Maximum open connections in the pool. |

## Error Handling

Each save is classified per attempt:

| Outcome | Cause | Behavior |
|---------|-------|----------|
| Transient | gRPC `UNAVAILABLE`, `DEADLINE_EXCEEDED`, `RESOURCE_EXHAUSTED`, `ABORTED`, `INTERNAL`, `UNKNOWN`; C++ exception (e.g. pool acquire) | `warn` per attempt, retried with exponential backoff up to `max-retries`. |
| Rejected | Other gRPC codes; server `exceptionalResult` (e.g. MLDP `overlapping activation exists ...`) | Not retried. |

When an item finally fails, the writer logs one `error` line and adds 1 to the writer-failures metric (one per item, not per attempt). An activation rejection includes the client id and the start/end times in UTC. An overlap rejection usually means the same event was already stored, from an earlier ingest, with different boundaries or a different id.

Before saving a configuration, the writer looks up the existing record so it can merge it. A transient lookup failure counts as a transient save failure: it is retried, and the configuration is **never** saved blind, which would overwrite existing description/tags/attributes. A non-transient lookup failure (e.g. `UNIMPLEMENTED`) is logged at `warn` and the configuration is saved as new.

Every 10 s (while items are processed) the writer logs a summary:
`MLDPConfigurationWriter '<name>': last 10s saved configurations=X activations=Y failed=Z retried=R`.

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Base class starts thread pool; calls `doStart()` → initializes `MLDPGrpcAnnotationPool`. |
| `push(batch)` | Base `toItems()` hook: returns 1 `ConfigItem` for accepted payloads; empty for others. Enqueued item blocks caller if queue is full; returns `false` only when stopping. |
| `stop()` | Base class drains queue, joins workers; calls `doStop()` → releases pool. |

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/mldp_configuration/MLDPConfigurationWriter.h` | Class definition; `ConfigItem` variant alias; `toItems`/`processItem`/`doStart`/`doStop` overrides. |
| `include/writer/mldp_configuration/MLDPConfigurationWriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `src/writer/mldp_configuration/MLDPConfigurationWriter.cpp` | `toItems()`, `processItem()`, `doSaveConfiguration()`, `doSaveConfigurationActivation()`. |
| `src/writer/mldp_configuration/MLDPConfigurationWriterConfig.cpp` | YAML parsing. |
