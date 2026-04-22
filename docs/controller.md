# Controller

> **Related:** [Architecture Overview](architecture.md) | [Configuration Reference](configuration.md) | [Readers](readers.md) | [Writers](writers-implementation.md)

The **MLDPPVXSController** is the central orchestrator of the MLDP PVXS Driver. It wires readers, writers, the event bus, metrics, and an optional routing table into a single runtime.

---

## Responsibilities

| Concern | Detail |
|---------|--------|
| **Lifecycle** | Creates readers and writers from YAML config, starts them in order, and performs ordered shutdown. |
| **Event dispatch** | Implements `IDataBus::push()` — the single entry point every reader calls to deliver data. |
| **Fan-out** | Forwards each `EventBatch` to one or more writers in parallel via a shared thread pool. |
| **Routing** | When a `routing:` block is present in the config, selectively dispatches batches based on reader→writer mappings. |
| **Metrics** | Owns the shared `Metrics` collector exposed via Prometheus. |

---

## Lifecycle

```
  ┌──────────┐     ┌───────┐     ┌──────────┐     ┌──────────┐
  │ construct │ ──▶ │ start │ ──▶ │ running  │ ──▶ │  stop    │
  └──────────┘     └───────┘     └──────────┘     └──────────┘
```

### `start()`

1. Validate that at least one writer and one reader are configured.
2. Resize the fan-out thread pool to match the number of writer instances.
3. Build writers via `WriterFactory` and call `start()` on each.
4. Build readers via `ReaderFactory` — readers begin pushing events immediately.
5. Build the **route table** from the optional `routing:` config block.
6. Log warnings for any orphan readers (not feeding any writer) or orphan writers (not receiving from any reader).

### `stop()`

Ordered shutdown (idempotent):

1. Set `running_` to false — new `push()` calls are rejected.
2. Clear readers (stops monitoring/polling).
3. Call `stop()` on each writer (drains pending work).
4. Release resources.

---

## Event Dispatch (`push()`)

When a reader produces an `EventBatch`, it calls `bus_->push(batch)`. The controller:

1. Rejects the batch if the controller is stopped, the `root_source` is empty, or frames are empty (unless it is an end-of-batch-group sentinel).
2. Warns if routing is active but `reader_name` is empty (potential misconfigured reader).
3. For each writer, checks `route_table_.accepts(writer_name, reader_name)`.
4. Submits a copy of the batch to each matching writer via the thread pool (parallel fan-out).
5. Collects futures and logs warnings for any writer that rejects the batch.

### Thread Pool Sizing

The fan-out thread pool is sized to the number of writer instances, so all writers can process a batch concurrently.

---

## Reader-to-Writer Routing

By default, every reader feeds every writer (**all-to-all**). The optional `routing:` configuration block enables selective dispatch.

### Motivation

Some writers should only receive data from specific readers:

- An HDF5 writer stores BSAS table data only (from a specific reader).
- A gRPC writer forwards scalar PVs only (from a different reader).
- A monitoring writer receives everything.

### Configuration

The routing block uses a **writer-centric** model: each writer declares which readers it accepts.

```yaml
routing:
  hdf5_local:
    from: [bsas-reader, epics-archiver-1]
  grpc_forwarder:
    from: [bsas-reader, scalar-reader]
  monitoring:
    from: [all]    # explicit wildcard — accepts every reader
```

When `routing:` is absent or empty, all readers feed all writers (backward compatible).

### Configuration Reference

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `routing` | map | No | Top-level routing block. Keys are writer instance names. |
| `routing.<writer>.from` | sequence of strings | Yes (per entry) | List of reader names this writer accepts. Use `all` to accept every reader. |

### Behavior Details

| Scenario | Result |
|----------|--------|
| No `routing:` block | All-to-all — every reader feeds every writer (default). |
| `routing:` present, writer listed | Writer receives only from listed readers. |
| `routing:` present, writer **not** listed | Writer receives **nothing**. |
| `from: [all]` | Writer accepts batches from any reader. |
| Empty `reader_name` on batch | Warning logged; batch likely dropped by routing (readers must stamp `reader_name`). |

### Route Table Internals

The route table is an **immutable** `std::unordered_map<writer_name, WriterRoute>` built once at startup. No mutex is needed during `push()` — the table is never modified after `start()` returns.

Lookup cost: **O(1)** average per writer per batch (hash map + hash set).

When no routing is configured, the route table is in **all-to-all mode** and `accepts()` returns `true` immediately with zero overhead.

### Startup Validation

At startup, the controller validates the routing configuration:

1. Every writer name in `routing:` must match an existing writer instance — unknown names cause a startup failure (`std::runtime_error`).
2. Every reader name in a `from:` list must match an existing reader instance (except `all`) — unknown names cause a startup failure.
3. Orphan warnings are logged:
   - **Orphan reader**: a reader not mentioned in any route — it will not feed any writer.
   - **Orphan writer**: a writer not mentioned in routing — it will receive no data.

### Example: Mixed Writer Setup

```yaml
name: my_controller

writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: dp-ingestion:50051
        query-url: dp-query:50052
        min-conn: 1
        max-conn: 4

  hdf5:
    - name: hdf5_bsas
      base-path: /data/hdf5
      max-file-age-s: 3600

reader:
  - epics-pvxs:
      - name: scalar_reader
        pvs:
          - name: MY:SCALAR:PV

  - epics-pvxs:
      - name: bsas_reader
        pvs:
          - name: MY:BSAS:TABLE
            option:
              type: slac-bsas-table

routing:
  mldp_main:
    from: [scalar_reader, bsas_reader]   # gRPC gets everything
  hdf5_bsas:
    from: [bsas_reader]                  # HDF5 gets BSAS only
```

In this configuration:
- `mldp_main` receives batches from both `scalar_reader` and `bsas_reader`.
- `hdf5_bsas` receives batches only from `bsas_reader`.
- Scalar PV data never reaches the HDF5 writer.

---

## Metrics

The controller emits the following Prometheus metrics (all labeled with `source`):

| Metric | Type | Description |
|--------|------|-------------|
| `mldp_pvxs_driver_controller_send_time_seconds` | Histogram | End-to-end time sending a batch to writers. |
| `mldp_pvxs_driver_controller_queue_depth` | Gauge | Aggregate queued items across all worker channels. |
| `mldp_pvxs_driver_controller_channel_queue_depth` | Gauge | Per-worker channel queue depth (labeled `worker=INDEX`). |

---

## Controller Configuration

```yaml
name: my_controller    # optional; default: "default"

writer:                # required — at least one writer instance
  mldp: [...]
  hdf5: [...]

reader:                # required — at least one reader instance
  - epics-pvxs: [...]
  - epics-base: [...]
  - epics-archiver: [...]

routing:               # optional — selective reader→writer dispatch
  writer_name:
    from: [reader_1, reader_2]

metrics:               # optional — Prometheus HTTP endpoint
  endpoint: "0.0.0.0:9464"
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | `"default"` | Controller instance name. Used as Prometheus label `controller`. |
| `writer` | map | — | **Required.** Writer instances. See [Configuration Reference](configuration.md#writer-block). |
| `reader` | sequence | — | **Required.** Reader instances. See [Configuration Reference](configuration.md#reader-block). |
| `routing` | map | — | Optional. Selective reader→writer routing. See [Routing](#reader-to-writer-routing) above. |
| `metrics` | map | — | Optional. Prometheus metrics endpoint. See [Configuration Reference](configuration.md#metrics-block). |

---

## Related Files

| File | Role |
|------|------|
| `include/controller/MLDPPVXSController.h` | Controller class declaration |
| `src/controller/MLDPPVXSController.cpp` | Lifecycle, dispatch, and routing integration |
| `include/controller/MLDPPVXSControllerConfig.h` | Typed config with routing entry parsing |
| `src/controller/MLDPPVXSControllerConfig.cpp` | YAML parsing (writers, readers, routing, metrics) |
| `include/controller/RouteTable.h` | Immutable route table class |
| `src/controller/RouteTable.cpp` | Route table build, accepts, orphan detection |
| `include/util/bus/IDataBus.h` | `EventBatchStruct` with `reader_name` field |
