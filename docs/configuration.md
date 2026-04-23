# Configuration Reference

> **Related:** [Architecture Overview](architecture.md) | [Controller](controller.md) | [Writers](writers-implementation.md) | [Readers](readers.md) | [Metrics Export](metrics-export-guide.md)

Complete YAML schema reference for the MLDP PVXS Driver. All keys are case-sensitive.

---

## Top-Level Structure

```yaml
writer:         # required — at least one writer instance
  grpc: [...]
  hdf5: [...]   # requires MLDP_PVXS_HDF5_ENABLED=ON build flag

reader:         # optional — list of reader groups
  - epics-pvxs: [...]
  - epics-base: [...]
  - epics-archiver: [...]

routing:        # optional — selective reader-to-writer dispatch
  writer_name:
    from: [reader_1, reader_2]

metrics:        # optional — Prometheus HTTP endpoint
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 1
```

---

## `writer:` Block

Top-level key. Must contain at least one writer instance across all types.

### `writer.mldp[]` — MLDP Ingestion Writer

Sequence of MLDP ingestion writer instances. Each element is an independent writer with its own thread pool and connection pool.

```yaml
writer:
  mldp:
    - name: mldp_main              # required — unique instance name
      thread-pool: 4               # optional; default: 1
      stream-max-bytes: 2097152    # optional; default: 2097152 (2 MiB)
      stream-max-age-ms: 200       # optional; default: 200 ms
      mldp-pool:                   # required
        provider-name: pvxs_provider           # required
        provider-description: "My provider"    # optional
        ingestion-url: grpc://ingest:50051      # required
        query-url:     grpc://query:50052       # optional; defaults to ingestion-url
        min-conn: 1                            # optional; default: 1
        max-conn: 4                            # optional; default: 4
        credentials: ssl                       # optional; "none" or "ssl" or map
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `thread-pool` | int | `1` | Worker threads for concurrent gRPC ingestion. |
| `stream-max-bytes` | size_t | `2097152` | Flush gRPC stream when payload exceeds this (bytes). |
| `stream-max-age-ms` | int | `200` | Flush gRPC stream when age exceeds this (milliseconds). |
| `mldp-pool.provider-name` | string | — | **Required.** Provider name registered with the MLDP service. |
| `mldp-pool.provider-description` | string | `""` | Human-readable provider description. |
| `mldp-pool.ingestion-url` | string | — | **Required.** gRPC endpoint for data ingestion. |
| `mldp-pool.query-url` | string | ingestion-url | gRPC endpoint for query operations. |
| `mldp-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-pool.max-conn` | int | `4` | Maximum open connections in the pool. |
| `mldp-pool.credentials` | string\|map | `"none"` | `"none"` (insecure), `"ssl"` (system TLS), or a map with `pem-cert-chain`, `pem-private-key`, `pem-root-certs` file paths. |

**Custom TLS credentials:**

```yaml
mldp-pool:
  credentials:
    pem-cert-chain:  /etc/certs/client.crt   # optional
    pem-private-key: /etc/certs/client.key   # optional
    pem-root-certs:  /etc/certs/ca.crt       # optional
```

→ [Full MLDP Writer Documentation](writers/mldp-writer.md)

---

### `writer.hdf5[]` — HDF5 Storage Writer

> **Build requirement:** Only available when compiled with `-DMLDP_PVXS_HDF5_ENABLED=ON`.

Sequence of HDF5 writer instances. Each instance maintains one open HDF5 file per `root_source`, rotating on age or size thresholds.

```yaml
writer:
  hdf5:
    - name: hdf5_local             # required — unique instance name
      base-path: /data/hdf5        # required — output directory
      max-file-age-s: 3600         # optional; default: 3600 (1 hour)
      max-file-size-mb: 512        # optional; default: 512 MiB
      flush-interval-ms: 1000      # optional; default: 1000 ms
      compression-level: 0         # optional; 0–9; default: 0 (no compression)
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `base-path` | string | — | **Required.** Directory where HDF5 files are created. |
| `max-file-age-s` | int | `3600` | Rotate file after this age in seconds. |
| `max-file-size-mb` | uint64 | `512` | Rotate file after this size in MiB. |
| `flush-interval-ms` | int | `1000` | Flush thread call interval in milliseconds. |
| `compression-level` | int | `0` | DEFLATE compression level 0–9 (0 = off). |

→ [Full HDF5 Writer Documentation](writers/hdf5-writer.md)

---

## `reader:` Block

Optional top-level sequence. Each list entry is a map with exactly one key: the reader type name. The value is a sequence of reader instances of that type.

```yaml
reader:
  - epics-pvxs:
      - name: pvxs_reader_1
        ...
  - epics-base:
      - name: base_reader_1
        ...
  - epics-archiver:
      - name: archiver_reader_1
        ...
```

If `reader:` is absent, no readers are started (the controller will only write data if fed externally or via another mechanism).

### Common Reader Keys (`epics-pvxs` and `epics-base`)

Both EPICS readers share the same base config (`EpicsReaderConfig`):

```yaml
- epics-pvxs:
    - name: pvxs_main              # required
      thread-pool: 2               # optional; default: 2
      column-batch-size: 50        # optional; default: 50
      monitor-poll-threads: 2      # optional; default: 2 (epics-base only)
      monitor-poll-interval-ms: 5  # optional; default: 5 ms (epics-base only)
      pvs:
        - name: MY:PV:1
        - name: MY:NTTABLE:PV
          option:
            type: slac-bsas-table
            tsSeconds: secondsPastEpoch  # optional; default: secondsPastEpoch
            tsNanos: nanoseconds         # optional; default: nanoseconds
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique reader instance name. |
| `thread-pool` | int | `2` | Worker threads for event processing. |
| `column-batch-size` | size_t | `50` | Max columns per `EventBatch` push for NTTable row-ts batches. `0` = unlimited. |
| `monitor-poll-threads` | int | `2` | Monitor queue polling threads *(epics-base only)*. |
| `monitor-poll-interval-ms` | int | `5` | Poll interval when monitor queue is idle *(epics-base only)*. |
| `pvs[].name` | string | — | **Required.** Fully qualified PV name. |
| `pvs[].option.type` | string | — | `slac-bsas-table` activates SLAC BSAS NTTable row-timestamp handling. |
| `pvs[].option.tsSeconds` | string | `secondsPastEpoch` | NTTable column name carrying seconds timestamp. |
| `pvs[].option.tsNanos` | string | `nanoseconds` | NTTable column name carrying nanoseconds timestamp. |

→ [EpicsPVXSReader Implementation](readers/epics-pvxs-reader-implementation.md)
→ [EpicsBaseReader Implementation](readers/epics-base-reader-implementation.md)

---

### `epics-archiver` Reader

Retrieves data from an EPICS Archiver Appliance via PB/HTTP.

Two fetch modes:

| Mode | Key value | Use case |
|------|-----------|----------|
| One-shot historical | `historical_once` (default) | Pull a fixed time window once |
| Continuous tail | `periodic_tail` | Poll the archiver for new data periodically |

```yaml
- epics-archiver:
    - name: archiver_historical       # required
      hostname: archiver.example:11200 # required
      mode: historical_once            # optional; default: historical_once
      start-date: "2026-01-01T00:00:00Z"  # required for historical_once
      end-date:   "2026-01-02T00:00:00Z"  # optional
      connect-timeout-sec: 30          # optional; default: 30
      total-timeout-sec: 300           # optional; default: 300 (0 = infinite)
      batch-duration-sec: 1            # optional; default: 1
      tls-verify-peer: true            # optional; default: true
      tls-verify-host: true            # optional; default: true
      pvs:
        - name: SLAC:GUNB:ELEC:LTU1:630:EPICS_PV

    - name: archiver_tail              # required
      hostname: archiver.example:11200 # required
      mode: periodic_tail
      poll-interval-sec: 5             # required for periodic_tail
      lookback-sec: 5                  # optional; defaults to poll-interval-sec
      pvs:
        - name: FACET:DL1:SBEN:1:BDES
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique reader instance name. |
| `hostname` | string | — | **Required.** Archiver Appliance host and port (e.g., `host:11200`). |
| `mode` | string | `historical_once` | `historical_once` or `periodic_tail`. |
| `start-date` | string | — | **Required for `historical_once`.** ISO 8601 start of time window. |
| `end-date` | string | — | Optional ISO 8601 end of time window. |
| `connect-timeout-sec` | long | `30` | HTTP connection establishment timeout (seconds). |
| `total-timeout-sec` | long | `300` | Total HTTP operation timeout (seconds). `0` = infinite. |
| `batch-duration-sec` | long | `1` | Max archiver sample-time span per output `EventBatch` (seconds). |
| `poll-interval-sec` | long | — | **Required for `periodic_tail`.** Tail poll interval (seconds). |
| `lookback-sec` | long | poll-interval-sec | Tail lookback window (seconds). Must be ≤ `poll-interval-sec`. |
| `tls-verify-peer` | bool | `true` | Verify the server TLS certificate chain. |
| `tls-verify-host` | bool | `true` | Verify the server hostname against the TLS certificate. |
| `pvs[].name` | string | — | **Required.** PV name to retrieve from the archiver. |

→ [EpicsArchiverReader Implementation](readers/epics-archiver-reader-implementation.md)

---

## `routing:` Block

Optional. Controls which readers feed which writers. When absent, all readers feed all writers (backward compatible default).

The routing model is **writer-centric**: each writer declares which readers it accepts.

```yaml
routing:
  mldp_main:
    from: [scalar_reader, bsas_reader]
  hdf5_bsas:
    from: [bsas_reader]
  monitoring:
    from: [all]         # accepts batches from every reader
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `routing` | map | No | Top-level routing block. Each key is a writer instance name. |
| `routing.<writer>.from` | sequence | Yes (per entry) | Reader names this writer accepts. Use `all` to accept every reader. |

### Behavior

| Scenario | Result |
|----------|--------|
| No `routing:` block | All-to-all dispatch (every reader feeds every writer). |
| Writer listed in `routing:` | Writer receives only from its listed readers. |
| Writer **not** listed in `routing:` | Writer receives **nothing** — a startup warning is logged. |
| `from: [all]` | Writer accepts batches from any reader. |

### Startup Validation

- Every writer name in `routing:` must match a configured writer instance. Unknown names cause a startup failure.
- Every reader name in `from:` must match a configured reader instance (except `all`). Unknown names cause a startup failure.
- Orphan warnings are logged for readers/writers not mentioned in any route.

→ [Full Controller Documentation](controller.md#reader-to-writer-routing)

---

## `metrics:` Block

Optional. Exposes a Prometheus HTTP endpoint for internal driver metrics.

```yaml
metrics:
  endpoint: "0.0.0.0:9464"     # required when block is present
  scan-interval-seconds: 1      # optional; default: 1
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `endpoint` | string | — | **Required.** `host:port` for the Prometheus exposer to bind. |
| `scan-interval-seconds` | uint32 | `1` | Interval between system metrics collection scans. |

When `metrics:` is absent the Prometheus exposer is not started.

→ [Metrics Export Guide](metrics-export-guide.md)

---

## Example Configurations

| File | Description |
|------|-------------|
| [`docs/examples/config-mldp-only.yaml`](examples/config-mldp-only.yaml) | Minimal single gRPC writer + PVXS reader |
| [`docs/examples/config-mldp-and-hdf5.yaml`](examples/config-mldp-and-hdf5.yaml) | Dual writer (gRPC + HDF5) |
| [`docs/examples/config-epics-archiver.yaml`](examples/config-epics-archiver.yaml) | Archiver reader with gRPC writer |
