# EpicsArchiverReader (Historical Data Retrieval)

The `EpicsArchiverReader` provides access to historical EPICS data from the EPICS Archiver Appliance. It fetches time-windowed datasets and supports both one-shot historical retrieval and periodic polling modes.

**Registration Type:** `"epics-archiver"`

**Status:** Implemented and actively developed

File           | Location
-------------- | -------------------------------------------------------
Header         | `include/reader/impl/epics_archiver/EpicsArchiverReader.h`
Implementation | `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp`
Config         | `include/reader/impl/epics_archiver/EpicsArchiverReaderConfig.h`

## Build Option & Required Libraries

- **Build option:** none (always built)
- **Required libraries/components:**
  - libcurl (`CURL::libcurl`) for HTTP transport
  - Protobuf (`protobuf::libprotobuf`) + epicsarchiverap protobuf payload definitions
- **Configure-time hints:** standard CMake package discovery for CURL/Protobuf

## Architecture

```mermaid
flowchart TB
    subgraph EpicsArchiverReader["EpicsArchiverReader"]
        subgraph Worker["Background Worker Thread"]
            Fetch["HTTP Fetch<br/>(via CURL)"]
            Parse["PB/HTTP Stream Parser<br/>(line-by-line)"]
            Batch["Batch Splitter<br/>(by timestamp)"]
        end

        Worker --> ReaderPool

        subgraph ReaderPool["Reader Thread Pool"]
            Convert["Data Conversion<br/>(Origin type → protobuf)"]
        end

        ReaderPool --> Push["Push to Event Bus"]
        Push --> Bus["IDataBus"]
    end

    subgraph Modes["Fetch Modes"]
        Historical["One-Shot<br/>(at construction)"]
        Periodic["Periodic Tail<br/>(continuous polling)"]
    end

    Fetch -.->|mode| Modes
```

## Operating Modes

### One-Shot Historical Fetch (Default)

1. EpicsArchiverReader constructed with start/end timestamps
2. Background worker thread starts immediately
3. Initiates HTTP GET request to Archiver Appliance `/retrieval/data/getData.raw`
4. Streams PB/HTTP response (PayloadInfo + ScalarDouble samples)
5. Parses protobuf messages line-by-line
6. Pushes batches to event bus
8. Completes and thread exits (reader still running but idle)
9. Signals `signalCompleted()` to notify the controller — enables [auto-close](readers.md#reader-lifecycle--auto-close) when all readers are one-shot

**Worker thread lifecycle:** Starts, fetches and processes, exits automatically. Calls `signalCompleted()` on exit to support [controller auto-close](readers.md#reader-lifecycle--auto-close). Shutdown cancels the HTTP request immediately; thread joins before destructor completes.

```yaml
reader:
  - epics-archiver:
      - name: my_archiver_reader
        hostname: "archiver-appliance.example.com:11200"
        start-date: "2024-01-01T00:00:00Z"  # Required
        end-date: "2024-01-02T00:00:00Z"    # Optional
        thread-pool: 2                      # Event conversion thread pool size
        pvs:
          - name: MY:ARCHIVER:PV
          - name: ANOTHER:HISTORICAL:PV
```

### Periodic Tail Mode

1. Configured with `mode: periodic_tail`
2. Background worker thread runs continuously
3. Each iteration: generates time window from `now - lookback` to `now`
4. Fetches archiver data for that window
5. Processes and pushes new events
6. Waits `poll-interval-sec` (interruptible via condition variable)
7. Repeats until reader destruction

**Worker thread lifecycle:** Runs until reader destruction. Condition variable allows prompt shutdown without waiting for poll interval to expire. Does **not** call `signalCompleted()` — periodic readers never trigger [auto-close](readers.md#reader-lifecycle--auto-close).

```yaml
reader:
  - epics-archiver:
      - name: continuous_archiver
        hostname: "archiver.example.com:11200"
        mode: periodic_tail             # Enable continuous polling
        poll-interval-sec: 5            # Poll every 5 seconds
        lookback-sec: 60                # Fetch last 60 seconds each time
        thread-pool: 2
        pvs:
          - name: MY:PV:NAME
```

## Configuration

### Required Parameters

Parameter | Type   | Description
--------- | ------ | ------------------------------------------------------------------
`hostname` | string | Archiver Appliance host:port (e.g., `archiver.example.com:11200`)
`pvs`     | list   | Array of PV names or objects to fetch from archiver

### One-Shot Mode Parameters

Parameter            | Type   | Default | Description
-------------------- | ------ | ------- | -------------------------------------------------------------------
`start-date`         | string | —       | **Required** ISO 8601 timestamp for query start
`end-date`           | string | —       | ISO 8601 timestamp for query end (defaults to `start-date` + 1 day)

### Periodic Tail Mode Parameters

Parameter            | Type   | Default         | Description
-------------------- | ------ | --------------- | -----------------------------------------------------------------
`mode`               | string | `one_shot`      | Set to `periodic_tail` to enable continuous polling
`poll-interval-sec`  | float  | 5.0             | Polling interval in seconds
`lookback-sec`       | float  | (poll_interval) | Seconds of history to fetch per poll (defaults to poll_interval)

### Sample-Count Batching Parameters

These parameters apply to both `historical_once` and `periodic_tail` modes.

Parameter                 | Type | Default | Description
------------------------- | ---- | ------- | -------------------------------------------------------------------
`pv-samples-per-batch`    | int  | 0       | Flush a pending PV batch after accumulating this many samples. `0` = disabled (submit immediately via timestamp splitting only).
`batch-flush-interval-ms` | int  | 0       | Flush any incomplete pending batch after this many milliseconds. `0` = disabled; incomplete batches are dropped unless this is set. Also enables shutdown-time flush of remaining pending samples.

**Interaction rules:**

- When `pv-samples-per-batch > 0`, samples are accumulated per PV in memory. A batch is submitted only when the count reaches `pv-samples-per-batch`.
- When `batch-flush-interval-ms > 0`, an additional time-based flush fires after each `fetchConfiguredPVs` call for batches whose age exceeds the interval. On reader shutdown, all remaining pending batches are also flushed.
- When `pv-samples-per-batch > 0` and `batch-flush-interval-ms = 0`, incomplete batches that have not yet reached the sample limit are **discarded** at shutdown.

```yaml
reader:
  epics-archiver:
    - name: batched_archiver
      hostname: "archiver.example.com:11200"
      start-date: "2024-01-01T00:00:00Z"
      pv-samples-per-batch: 10        # submit after 10 samples per PV
      batch-flush-interval-ms: 5000   # also flush after 5 seconds if batch not full
      pvs:
        - name: MY:PV
```

### HTTP and TLS Parameters

Parameter             | Type  | Default | Description
--------------------- | ----- | ------- | ----------------------------------
`connect-timeout-sec` | float | 30.0    | Connection establishment timeout
`total-timeout-sec`   | float | 300.0   | Total operation timeout
`tls-verify-peer`     | bool  | true    | Verify SSL peer certificate
`tls-verify-host`     | bool  | true    | Verify hostname matches certificate

```yaml
reader:
  - epics-archiver:
      - name: secure_archiver
        hostname: "archiver.example.com:11200"
        start-date: "2024-01-01T00:00:00Z"
        connect-timeout-sec: 30         # Connection timeout (default: 30)
        total-timeout-sec: 300          # Total operation timeout (default: 300)
        tls-verify-peer: true           # Verify SSL peer (default: true)
        tls-verify-host: true           # Verify hostname (default: true)
        pvs:
          - name: MY:PV
```

**Validation rules:**

- `total-timeout-sec >= connect-timeout-sec` (enforced)
- All timeout values must be positive (enforced)
- Hostname must be valid and reachable
- At least one PV required
- Start date required for one-shot mode
- `pv-samples-per-batch` must be `> 0` when specified (enforced)
- `batch-flush-interval-ms` must be `> 0` when specified (enforced)

## Key Features

- **Time-Windowed Queries**: Query archiver data by start/end timestamps with efficient PB/HTTP streaming
- **Sample-Count Batching**: Accumulate up to `pv-samples-per-batch` samples per PV before submitting; each PV maintains independent pending state
- **Timed Flush**: `batch-flush-interval-ms` ensures incomplete batches are not held indefinitely; also drains all pending batches on shutdown when set
- **Automated Tail Polling**: Continuously fetch new archiver data at configurable intervals with a configurable lookback window to handle clock skew and backfill
- **Background Worker Thread**: One-shot fetch or periodic polling runs off main reader construction
- **Thread Pool Processing**: Configurable conversion parallelism
- **Secure Defaults**: TLS verification enabled by default; configurable per-instance timeouts
- **Graceful Shutdown**: HTTP request cancellation on reader destruction; thread joins before destructor completes
- **Metrics**: Prometheus metrics for events received, processed, and errors

## Data Types Supported

All standard EPICS Archiver Appliance PB/HTTP payload types are supported:

### Scalars

EPICS Type      | DataFrame Column Type
--------------- | ---------------------
`SCALAR_DOUBLE` | `doublecolumns`
`SCALAR_FLOAT`  | `floatcolumns`
`SCALAR_INT`    | `int32columns`
`SCALAR_SHORT`  | `int32columns`
`SCALAR_ENUM`   | `int32columns`
`SCALAR_STRING` | `stringcolumns`
`SCALAR_BYTE`   | `stringcolumns` (raw bytes)

### Waveforms

EPICS Type        | DataFrame Column Type
----------------- | ---------------------
`WAVEFORM_DOUBLE` | `doublecolumns`
`WAVEFORM_FLOAT`  | `floatcolumns`
`WAVEFORM_INT`    | `int32columns`
`WAVEFORM_SHORT`  | `int32columns`
`WAVEFORM_ENUM`   | `int32columns`
`WAVEFORM_STRING` | `stringcolumns`
`WAVEFORM_BYTE`   | `stringcolumns` (raw bytes)

### Other

EPICS Type         | DataFrame Column Type
------------------ | ---------------------
`V4_GENERIC_BYTES` | `stringcolumns` (raw bytes)

## Use Cases and Patterns

### Backfill Historical Data

```yaml
reader:
  - epics-archiver:
      - name: yesterday_backfill
        hostname: "archiver.example.com:11200"
        start-date: "2024-01-09T00:00:00Z"
        end-date: "2024-01-10T00:00:00Z"
        pvs:
          - name: MY:SCALAR:PV
```

### Continuous Tail (Last Hour)

```yaml
reader:
  - epics-archiver:
      - name: tail_reader
        hostname: "archiver.example.com:11200"
        mode: periodic_tail
        poll-interval-sec: 10
        lookback-sec: 3600            # Keep 1 hour of history per poll
        pvs:
          - name: MY:PV
```

## Metrics

Metric                                            | Description
------------------------------------------------- | -------------------------------------
`mldp_pvxs_driver_reader_events_received_total`   | Total archiver samples received
`mldp_pvxs_driver_reader_events_total`            | Successfully processed events
`mldp_pvxs_driver_reader_errors_total`            | Conversion or HTTP errors
`mldp_pvxs_driver_reader_processing_time_ms`      | Event processing latency (histogram)
`mldp_pvxs_driver_reader_pool_queue_depth`        | Thread pool queue depth

## Implementation Details

### Sample-Count Batching

When `pv-samples-per-batch > 0`, samples are accumulated in `PendingPvBatch` entries keyed by PV name. Each PV tracks its own pending frames and the wall-clock time when the first sample arrived (`created_at`). Submission fires when:

1. The frame count reaches `pv-samples-per-batch` (size trigger), or
2. `batch-flush-interval-ms > 0` and `now - created_at >= interval` after each `fetchConfiguredPVs` call (time trigger).

When `batch-flush-interval-ms = 0`, incomplete batches that have not reached the sample limit are discarded on reader shutdown. Set `batch-flush-interval-ms` to enable shutdown-time drain of remaining pending samples.

Samples accumulate in the pending buffer until the size or time trigger fires.

### HTTP Transport

- **Chunked Transfer**: Efficient streaming via HTTP chunked encoding
- **Keep-Alive**: TCP keep-alive and compression support enabled
- **Cancellation**: HTTP client cancellation on reader destruction prevents hanging requests
