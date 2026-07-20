# Configuration Reference

> **Related:** [Architecture Overview](../reference/architecture.md) | [Controller](../reference/controller.md) | [Writers](../writers/writers-implementation.md) | [Readers](../readers/readers.md) | [Metrics Export](../metrics/metrics-export-guide.md)

Complete YAML schema reference for the MLDP PVXS Driver. All keys are case-sensitive.

---

## Top-Level Structure

```yaml
enrichers:      # optional — globally named queued-writer payload transformations
  run-metadata:
    type: static-metadata
    metadata: {experiment_id: run-42}

writer:         # required — at least one writer instance
  mldp: [...]
  hdf5: [...]              # requires -DMLDP_PVXS_ENABLE_HDF5=ON build option
  mldp-pv-metadata: [...]  # persists PV metadata via annotation service
  mldp-configuration: [...] # persists configuration objects via annotation service

reader:         # required — at least one reader instance
  - epics-pvxs: [...]
  - epics-base: [...]
  - epics-archiver: [...]
  - epics-ds-metadata: [...]  # fetch PV metadata via EPICS Directory Service RPC
  - slac-calendar: [...]      # fetch beamline schedule events from SLAC calendar HTTP API

routing:        # optional — selective reader-to-writer dispatch
  writer_name:
    from: [reader_1, reader_2]

processors:     # optional — algorithm-backed virtual channel processors (requires BUILD_PYTHON_PROCESSOR=ON for python-processor)
  - type: python-processor
    script-dir: /opt/processors

queryable:      # optional — query client configuration
  mldp: [...]
  mldp-annotation: [...]

metrics:        # optional — Prometheus HTTP endpoint
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 1
```

---

## `writer:` Block

Top-level key. Must contain at least one writer instance across all types.

### Global `enrichers:` and writer chains

`enrichers:` is an optional mapping from an application-unique name to an enricher definition. Each definition is created once when the controller starts. A queued writer can reference an ordered chain with `enrichers: [name, ...]`; the same name on multiple writers deliberately shares its state and is serialized per enricher instance.

```yaml
enrichers:
  run-metadata:
    type: static-metadata
    metadata: {experiment_id: run-42}
  shard-slots:
    type: shard-slot
    num-shards: 6
writer:
  mldp:
    - name: mldp_main
      enrichers: [run-metadata, shard-slots]
```

Writer references must be a sequence of non-empty, unique declared names. Unknown names, duplicate names, missing `type`, and invalid definitions fail startup. Enrichment occurs before writer conversion and queue admission. A filtering enricher accepts but does not queue the batch; an enricher exception rejects it.

Built-in enrichers are `static-metadata` (`metadata` map, overwrites batch keys), `column-attributes` (`column-pattern` glob and `attributes` map), `timestamp-clamp` (limits time-series nanoseconds to `999999999`), and `shard-slot`. `shard-slot` uses `num-shards` from 1 through 65536 (default 6), preserves an existing `shardSlot`, and assigns stable process-lifetime five-digit slots to first-seen columns. Mappings reset on process restart. Changing the shard count can split a PV's historical and new data across MongoDB shards.

`static-metadata` applies to every payload type: time-series, source metadata, configuration, and configuration activation. The other C++ built-ins operate on time-series frames only; they accept the other payload types without changing them.

### `python-enricher`

`python-enricher` is available only when the driver is built with `-DBUILD_PYTHON_PROCESSOR=ON` (the default). Its global definition requires one `script-path`:

```yaml
enrichers:
  classify-payload:
    type: python-enricher
    script-path: /opt/mldp/enrichers/classify.py
writer:
  mldp:
    - name: mldp_main
      enrichers: [classify-payload]
```

The script must export `enrich(batch)`. It receives a dictionary with `reader_name`, `payload_type`, and `metadata`. `payload_type` is one of `time-series`, `source-metadata`, `configuration`, or `configuration-activation`. Return `None` to accept but drop the batch, or return a dictionary containing an optional string-to-string `metadata` mapping to merge into the batch metadata. A non-dictionary result is rejected; Python exceptions are logged and rejected without stopping the writer. Python calls acquire the CPython GIL, so one shared Python enricher runs serially even when several writers reference it. See the [Payload Enricher Guide](../enrichers/enrichers.md#python-enricher) for the complete contract.

### `writer.mldp[]` — MLDP Ingestion Writer

Sequence of MLDP ingestion writer instances. Each element is an independent writer with its own thread pool and connection pool.

```yaml
writer:
  mldp:
    - name: mldp_main              # required — unique instance name
      thread-pool: 4               # optional; default: 1
      stream-max-bytes: 2097152    # optional; default: 2097152 (2 MiB)
      stream-max-age-ms: 200       # optional; default: 200 ms
      queue-capacity: 10000        # optional; default: 10000
      push-timeout-ms: 5000        # optional; default: 5000 (0 = drop immediately)
      mldp-pool:                   # required
        provider-name: pvxs_provider           # required
        provider-description: "My provider"    # optional
        ingestion-url: grpc://ingest:50051      # required
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
| `queue-capacity` | size_t | `10000` | Max queued items across all worker channels before push blocks. |
| `push-timeout-ms` | int | `5000` | How long `push()` blocks waiting for space (ms). 0 = drop immediately without waiting. |
| `mldp-pool.provider-name` | string | — | **Required.** Provider name registered with the MLDP service. |
| `mldp-pool.provider-description` | string | `""` | Human-readable provider description. |
| `mldp-pool.ingestion-url` | string | — | **Required.** gRPC endpoint for data ingestion. |
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

→ [Full MLDP Writer Documentation](../writers/mldp-writer.md)

---

### `writer.hdf5[]` — HDF5 Storage Writer

> **Build requirement:** Only available when compiled with `-DMLDP_PVXS_ENABLE_HDF5=ON`.

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
      queue-capacity: 8192         # optional; default: 8192
      merge-root-sources: false    # optional; default: false — set true to merge all sources into one file
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `base-path` | string | — | **Required.** Directory where HDF5 files are created. |
| `max-file-age-s` | int | `3600` | Rotate file after this age in seconds. |
| `max-file-size-mb` | uint64 | `512` | Rotate file after this size in MiB. |
| `flush-interval-ms` | int | `1000` | Flush thread call interval in milliseconds. |
| `compression-level` | int | `0` | DEFLATE compression level 0–9 (0 = off). |
| `queue-capacity` | size_t | `8192` | Max queued batches before `push()` blocks. Push blocks indefinitely — **never drops data**. Only unblocks on shutdown (double Ctrl+C). |
| `merge-root-sources` | bool | `false` | Opt-in merge mode; all root-sources share one output file with one HDF5 group per source. See [hdf5-writer.md](../writers/hdf5-writer.md#merge-mode). |

→ [Full HDF5 Writer Documentation](../writers/hdf5-writer.md)

---

### `writer.mldp-pv-metadata[]` — MLDP PV Metadata Writer

Sequence of PV metadata writer instances. Each element persists `SourceMetadataPayload` batches to the `DpAnnotationService` gRPC endpoint via `savePvMetadata` RPCs.

```yaml
writer:
  mldp-pv-metadata:
    - name: pv_metadata_main        # required — unique instance name
      thread-pool: 2                # optional; default: 2
      deadline-seconds: 10          # optional; default: 10
      mldp-pv-metadata-pool:        # required
        annotation-url: grpc://annotation-host:50053  # required
        min-conn: 1                 # optional; default: 1
        max-conn: 4                 # optional; default: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique writer instance name. |
| `thread-pool` | int | `2` | Worker threads for concurrent annotation RPCs. |
| `deadline-seconds` | int | `10` | Per-RPC deadline in seconds. |
| `mldp-pv-metadata-pool.annotation-url` | string | — | **Required.** gRPC endpoint for the annotation service. |
| `mldp-pv-metadata-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-pv-metadata-pool.max-conn` | int | `4` | Maximum open connections in the pool. |

→ [Full MLDP PV Metadata Writer Documentation](../writers/mldp-pv-metadata-writer.md)

---

### `writer.mldp-configuration[]` — MLDP Configuration Writer

Sequence of configuration writer instances. Each element persists `ConfigurationPayload` and `ConfigurationActivationPayload` batches to the annotation gRPC endpoint via `saveConfiguration` / `saveConfigurationActivation` RPCs.

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
| `thread-pool` | int | `2` | Worker threads for concurrent annotation RPCs. |
| `deadline-seconds` | int | `10` | Per-RPC deadline in seconds. |
| `mldp-annotation-pool.annotation-url` | string | — | **Required.** gRPC endpoint for the annotation service. |
| `mldp-annotation-pool.min-conn` | int | `1` | Minimum open connections in the pool. |
| `mldp-annotation-pool.max-conn` | int | `4` | Maximum open connections in the pool. |

→ [Full MLDP Configuration Writer Documentation](../writers/mldp-configuration-writer.md)

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
      static-metadata:             # optional — reader-level key/value metadata
        facility: lcls
        subsystem: bpms
      pvs:
        - name: MY:PV:1
          metadata:                # optional — per-PV overrides (merged over static-metadata)
            signal_type: scalar
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
| `static-metadata` | map | `{}` | Reader-level key/value metadata stamped on every `EventBatch.metadata`. |
| `pvs[].name` | string | — | **Required.** Fully qualified PV name. |
| `pvs[].metadata` | map | `{}` | Per-PV metadata key/value pairs. Merged over `static-metadata`; PV-level keys win on conflict. |
| `pvs[].option.type` | string | — | `slac-bsas-table` activates SLAC BSAS NTTable row-timestamp handling. |
| `pvs[].option.tsSeconds` | string | `secondsPastEpoch` | NTTable column name carrying seconds timestamp. |
| `pvs[].option.tsNanos` | string | `nanoseconds` | NTTable column name carrying nanoseconds timestamp. |

**Metadata merge semantics**: `EventBatch.metadata` = `static-metadata` merged with the matching PV's `metadata` block. Per-PV keys override reader-level keys on conflict. The merged map is forwarded to writers and stamped as `ColumnProvenance.source` labels in gRPC ingestion requests.

→ [EpicsPVXSReader Implementation](../readers/epics-pvxs-reader-implementation.md)
→ [EpicsBaseReader Implementation](../readers/epics-base-reader-implementation.md)

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
      fetch-threads: 4                 # optional; default: 1 (parallel PV fetch workers)
      connect-timeout-sec: 30          # optional; default: 30
      total-timeout-sec: 300           # optional; default: 300 (0 = infinite)
      pv-samples-per-batch: 0          # optional; default: 0 (disabled)
      batch-flush-interval-ms: 0       # optional; default: 0 (disabled)
      tls-verify-peer: true            # optional; default: true
      tls-verify-host: true            # optional; default: true
      static-metadata:                 # optional — reader-level key/value metadata
        source: archiver
      pvs:
        - name: SLAC:GUNB:ELEC:LTU1:630:EPICS_PV
          metadata:                    # optional — per-PV overrides
            signal_type: waveform

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
| `fetch-threads` | long | `1` | Number of parallel worker threads fetching PVs. Each gets its own HTTP client. |
| `pv-samples-per-batch` | long | `0` | Accumulate this many samples per PV before submitting. `0` = disabled. |
| `batch-flush-interval-ms` | long | `0` | Flush incomplete PV batches after this many ms. `0` = disabled; when disabled, incomplete batches are discarded at shutdown. |
| `poll-interval-sec` | long | — | **Required for `periodic_tail`.** Tail poll interval (seconds). |
| `lookback-sec` | long | poll-interval-sec | Tail lookback window (seconds). Must be ≤ `poll-interval-sec`. |
| `tls-verify-peer` | bool | `true` | Verify the server TLS certificate chain. |
| `tls-verify-host` | bool | `true` | Verify the server hostname against the TLS certificate. |
| `static-metadata` | map | `{}` | Reader-level key/value metadata stamped on every `EventBatch.metadata`. |
| `pvs[].name` | string | — | **Required.** PV name to retrieve from the archiver. |
| `pvs[].metadata` | map | `{}` | Per-PV metadata key/value pairs. Merged over `static-metadata`. |

→ [EpicsArchiverReader Implementation](../readers/epics-archiver-reader-implementation.md)

---

### `epics-ds-metadata` Reader {#epics-ds-metadata-reader}

Fetches PV metadata from an EPICS Directory Service endpoint via PVA RPC and publishes
a `SourceMetadataPayload` onto the bus. Pair with an `mldp-pv-metadata` writer to
persist the metadata to the MLDP annotation service.

```yaml
- epics-ds-metadata:
    - name: ds_metadata                       # required
      service: ds                             # optional; default: "ds"
      query: "%"                              # optional; default: "%"
      timeout-sec: 5.0                        # optional; default: 5.0
      source-name-column: channelName         # optional; default: "channelName"
      tags-column: tags                       # optional; default: "" (disabled)
      show-columns: "channelName,hostName"    # optional; default: "" (all columns)
      rescan-interval-sec: 300.0              # optional; default: 0.0 (run once)
      worker-thread-count: 2                  # optional; default: 1
      max-queue-depth: 16                     # optional; default: 16
      pv-show-columns: "dname,ename,etype"    # optional; default: dname,ename,etype,lname,ioc,scheme,z
      pvs:                                    # required; must contain at least one entry
        - name: BPMS:LI20:2445:X
          metadata:
            system: bpm
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique reader instance name. |
| `service` | string | `"ds"` | PVA service name to call via RPC. |
| `query` | string | `"%"` | Query pattern sent in the NTURI `query.name` field. |
| `timeout-sec` | double | `5.0` | RPC call timeout in seconds. Must be positive. |
| `source-name-column` | string | `"channelName"` | NTTable column carrying the PV / source name. |
| `tags-column` | string | `""` | NTTable column for comma-separated tags. Empty = disabled. |
| `show-columns` | string | `""` | Comma-separated columns passed as `show=` in the wildcard NTURI query. Empty = server returns all columns. |
| `rescan-interval-sec` | double | `0.0` | Repeat fetch interval in seconds. `0` = run once. |
| `worker-thread-count` | int | `1` | `1` = single-thread inline; `N > 1` = 1 producer + N-1 consumers. Range: `1..64`. |
| `max-queue-depth` | int | `16` | Bounded queue depth in producer/consumer mode. Ignored when `worker-thread-count` is `1`. Range: `1..1024`. |
| `pvs` | list | — | **Required.** Per-PV enrichment entries for targeted DS lookups. Must contain at least one entry. Each entry requires `name`; `metadata` map is optional. |
| `pv-show-columns` | string | `"dname,ename,etype,lname,ioc,scheme,z"` | DS `show=` columns fetched per PV in PV-list mode. Duplicate values are rejected. |

**Validation rules:**

- `name` is required and must be non-empty.
- `timeout-sec` must be strictly positive.
- `rescan-interval-sec` must be `>= 0`.
- `worker-thread-count` must be in range `1..64`.
- `max-queue-depth` must be in range `1..1024`.
- `pvs` is required and must contain at least one entry.
- `pv-show-columns` must not contain duplicate column names.

→ [EpicsDSMetadataReader Documentation](../readers/epics-ds-metadata-reader.md)

---

### `slac-calendar` Reader {#slac-calendar-reader}

Fetches beamline experiment schedule events from the SLAC calendar HTTP API and publishes
`ConfigurationPayload` + `ConfigurationActivationPayload` pairs onto the bus. Pair with an
`mldp-configuration` writer to persist schedule data to the MLDP annotation service.

```yaml
- slac-calendar:
    - name: cal_reader                  # required
      base-url: https://calendar.slac.stanford.edu  # required
      experiments:                      # required
        - lcls
        - facet
      lookahead-days: 30                # required — must be > 0
      lookback-days: 1                  # optional; default: 1
      rescan-interval-sec: 3600.0       # optional; default: 0.0 (run once)
      connect-timeout-sec: 30           # optional; default: 30
      total-timeout-sec: 60             # optional; default: 60
      tls-verify-peer: true             # optional; default: true
      tls-verify-host: true             # optional; default: true
      event-limit: 1000                 # optional; default: 1000
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Unique reader instance name. |
| `base-url` | string | — | **Required.** Base URL of the SLAC calendar API (no trailing slash). |
| `experiments` | sequence | — | **Required.** Experiment names to fetch (e.g. `lcls`, `facet`). |
| `lookahead-days` | int | — | **Required.** Days into the future to include. Must be > 0. |
| `lookback-days` | int | `1` | Days into the past to include. Must be >= 0. |
| `start-date` | string | — | First-run start override (`YYYY-MM-DD`). Used only on the first fetch. |
| `rescan-interval-sec` | double | `0.0` | Repeat fetch interval in seconds. `0` = run once. |
| `connect-timeout-sec` | int | `30` | HTTP connection timeout (seconds). |
| `total-timeout-sec` | int | `60` | HTTP total request timeout. Must be >= `connect-timeout-sec`. |
| `tls-verify-peer` | bool | `true` | Verify TLS peer certificate. |
| `tls-verify-host` | bool | `true` | Verify TLS hostname against certificate. |
| `event-limit` | int | `1000` | Maximum events per API request. |

→ [SlacCalendarReader Documentation](../readers/slac-calendar-reader.md)

---

## `queryable:` Block

Optional. Configures query client factories used by the driver for out-of-band metadata and data inspection. Each entry registers a client type with `QueryableFactory` at startup. Components that need a query client call `QueryableFactory::instance().create<T>()` at runtime.

```yaml
queryable:
  mldp:
    mldp-pool:
      ingestion-url: grpc://ingest:50051
      query-url:     grpc://query:50052   # required here — used by MLDPQueryClient
      min-conn: 1
      max-conn: 2
  mldp-annotation:
    mldp-annotation-pool:
      annotation-url: grpc://annotation-host:50053
      min-conn: 1
      max-conn: 2
```

| Type key | Client class | Description |
|----------|-------------|-------------|
| `mldp` | `MLDPQueryClient` | Query source metadata and historical data from MLDP. |
| `mldp-annotation` | `MLDPAnnotationQueryClient` | Query annotation service metadata. |

> **Note:** `queryable:` is optional. When absent, no query clients are registered. Writers and readers that require a query client will fail at runtime if the corresponding type was not prepared.

→ [Query Client Documentation](../dev/query-client.md)

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
| `routing.<writer>.include` | sequence | No | Glob patterns for `root_source`; batch is accepted only if at least one pattern matches. Empty or absent = accept all. |
| `routing.<writer>.exclude` | sequence | No | Glob patterns for `root_source`; batch is dropped if any pattern matches. Applied after `include`. Empty or absent = exclude none. |

### PV Filter Routing (`include` / `exclude`)

Each routing entry can optionally filter batches by their `root_source` field (the PV name that originated the batch) using glob patterns evaluated with `fnmatch(3)`.

**Filter precedence** (applied in order):
1. **`include`** — if non-empty, the batch must match at least one pattern to proceed. If empty or absent, all batches pass this step.
2. **`exclude`** — if non-empty, the batch is dropped if it matches any pattern. Applied after `include`, so `exclude` always wins.

**Glob syntax notes:**
- Patterns follow POSIX `fnmatch(3)` rules (`*` and `?` wildcards are supported).
- Matching is **case-sensitive**.
- `FNM_PATHNAME` is **not** set, so `*` matches `:` (the standard EPICS PV separator). For example, `FACET:*` matches `FACET:LI20:XCOR:01`.

**Example — two readers, one merged HDF5 writer with filters:**

```yaml
reader:
  - epics-pvxs:
      - name: bsas_reader
        pvs:
          - name: FACET:LI20:XCOR:01
          - name: FACET:LI20:XCOR:02
  - epics-archiver:
      - name: archiver_reader
        hostname: archiver.example:11200
        mode: historical_once
        start-date: "2026-01-01T00:00:00Z"
        pvs:
          - name: FACET:LI20:XCOR:01
          - name: SYS:DIAG:TEMP:01

writer:
  hdf5:
    - name: hdf5_facet
      base-path: /data/hdf5/facet
      merge-root-sources: true   # all sources in one file, one HDF5 group per source

routing:
  hdf5_facet:
    from: [bsas_reader, archiver_reader]
    include:
      - "FACET:*"       # only FACET PVs pass through
    exclude:
      - "FACET:*:TEMP:*"  # drop any FACET temperature PVs
```

In this example:
- `FACET:LI20:XCOR:01` — **passes** (matches `include`, does not match `exclude`).
- `SYS:DIAG:TEMP:01` — **dropped** (no match in `include`).
- `FACET:LI20:TEMP:99` — **dropped** (matches `include` but also matches `exclude`; exclude wins).
- `merge-root-sources: true` causes all accepted sources to be written into a single HDF5 file, with one HDF5 group per `root_source`.

### Behavior

| Scenario | Result |
|----------|--------|
| No `routing:` block | All-to-all dispatch (every reader feeds every writer). |
| Writer listed in `routing:` | Writer receives only from its listed readers. |
| Writer **not** listed in `routing:` | Writer receives **nothing** — a startup warning is logged. |
| `from: [all]` | Writer accepts batches from any reader. |
| No `include` / `exclude` | All batches from listed readers are forwarded (backward compatible). |
| `include` non-empty, `exclude` absent | Only batches whose `root_source` matches an include pattern are forwarded. |
| `exclude` non-empty, `include` absent | All batches forwarded except those whose `root_source` matches an exclude pattern. |
| Both `include` and `exclude` non-empty | Batch must match `include` **and** must not match `exclude`. |

### Startup Validation

- Every writer name in `routing:` must match a configured writer instance. Unknown names cause a startup failure.
- Every reader name in `from:` must match a configured reader instance (except `all`). Unknown names cause a startup failure.
- Orphan warnings are logged for readers/writers not mentioned in any route.

→ [Full Controller Documentation](../reference/controller.md#reader-to-writer-routing)

---

## `processors:` Block {#processors-block}

Optional. Declares channel processors — algorithm-backed virtual channel engines that consume real source batches, run a compute function, and publish virtual output sources back onto the bus. The existing sequence form remains supported. A mapping form additionally supports `algorithms-plugin-path` and named Python algorithms; the path defaults to `./algorithms` relative to the process working directory.

Processors integrate with the `routing:` system the same way writers do: they appear as named targets in `routing:` to control which readers feed them, and their virtual output sources can be used as `from:` origins to feed downstream writers.

> **Build requirement:** `python-processor` requires `-DBUILD_PYTHON_PROCESSOR=ON` (CMake default: ON). When disabled the type is not registered and startup fails if any entry uses it.

```yaml
processors:
  - type: python-processor
    script-dir: /opt/scripts/processors
```

To load one script by logical type, use the mapping form:

```yaml
processors:
  algorithms-plugin-path: /opt/mldp/algorithms
  beam-calculation:
    type: beam_calculation  # loads /opt/mldp/algorithms/beam_calculation.py
```

If an unregistered processor definition has `script-path`, that explicit file takes precedence over `algorithms-plugin-path`.

### `processors[].type: python-processor`

Scans `script-dir` for `.py` files and creates one `ChannelProcessor` per valid script. Invalid or mis-configured scripts are skipped with a warning — they do not abort the load.

```yaml
processors:
  - type: python-processor
    script-dir: /opt/scripts/processors   # required
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `type` | string | Yes | `"python-processor"` |
| `script-dir` | string | Yes | Path to directory containing `.py` processor scripts. |

Each Python script must export:

| Symbol | Type | Description |
|--------|------|-------------|
| `config` | `dict` | Processor metadata. Must contain `name`, `sources`, and `output_source` or `output_sources`. |
| `compute` | callable | Algorithm entry point. Receives a `dict` snapshot and returns one or more `mldp` payload objects. |

**Minimal script:**

```python
import mldp

config = {
    "name": "my-processor",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "VIRTUAL:MY:OUT",
}

def compute(snapshot):
    value = snapshot.get("SRC:A", 0.0)
    return mldp.timeseries("VIRTUAL:MY:OUT", {"value": value * 2.0})
```

**`config` keys:**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | **Required.** Processor instance name. |
| `sources` | list[str] | — | **Required.** Input source names consumed by this processor. |
| `alignment` | string | `latest-value` | `latest-value` or `interpolate`. |
| `trigger` | string | `any-update` | `any-update`, `all-updated`, or `interval`. |
| `trigger-interval-sec` | float | — | **Required when `trigger` is `interval`.** Fire interval in seconds. |
| `max-buffer-depth` | int | `0` | Maximum samples retained per input source. `0` = unlimited; when the depth is exceeded, oldest samples are dropped before the next compute. |
| `output_source` | string | — | Single virtual output source (convenience alias for `output_sources: [name]`). |
| `output_sources` | list[str] | — | One or more virtual output source names emitted by `compute()`. |

**Wiring with routing:**

```yaml
processors:
  - type: python-processor
    script-dir: /opt/scripts/processors

routing:
  my-processor:              # processor name matches script config["name"]
    from: [pvxs_reader]
    include:
      - "SRC:*"
  mldp_main:
    from: [my-processor]     # processor's virtual output feeds the writer
```

→ [Full Python Processor Documentation](../processors/python-processor.md)

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

→ [Metrics Export Guide](../metrics/metrics-export-guide.md)

---

## Example Configurations

| File | Description |
|------|-------------|
| [`docs/examples/config-mldp-only.yaml`](../examples/config-mldp-only.yaml) | Minimal single gRPC writer + PVXS reader |
| [`docs/examples/config-mldp-and-hdf5.yaml`](../examples/config-mldp-and-hdf5.yaml) | Dual writer (gRPC + HDF5) |
| [`docs/examples/config-epics-archiver.yaml`](../examples/config-epics-archiver.yaml) | Archiver reader with gRPC writer |
