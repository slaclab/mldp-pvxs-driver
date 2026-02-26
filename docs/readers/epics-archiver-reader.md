# EpicsArchiverReader (Historical Data Retrieval)

The `EpicsArchiverReader` provides access to historical EPICS data from the EPICS Archiver Appliance. It fetches time-windowed datasets and supports both one-shot historical retrieval and periodic polling modes.

**Registration Type:** `"epics-archiver"`

**Status:** Implemented and actively developed

| File           | Location                                         |
|----------------|--------------------------------------------------|
| Header         | `include/reader/impl/epics/EpicsArchiverReader.h` |
| Implementation | `src/reader/impl/epics/EpicsArchiverReader.cpp`   |
| Config         | `include/reader/impl/epics/EpicsArchiverReaderConfig.h` |

## Architecture

```mermaid
flowchart TB
    subgraph EpicsArchiverReader[\"EpicsArchiverReader\"]
        subgraph Worker[\"Background Worker Thread\"]
            Fetch[\"HTTP Fetch<br/>(via CURL)\"]
            Parse[\"PB/HTTP Stream Parser<br/>(line-by-line)\"]
            Batch[\"Batch Splitter<br/>(by timestamp)\"]
        end

        Worker --> ReaderPool

        subgraph ReaderPool[\"Reader Thread Pool\"]
            Convert[\"Data Conversion<br/>(SCALAR_DOUBLE → protobuf)\"]
        end

        ReaderPool --> Push[\"Push to Event Bus\"]
        Push --> Bus[\"IEventBusPush\"]
    end

    subgraph Modes[\"Fetch Modes\"]
        Historical[\"One-Shot<br/>(at construction)\"]
        Periodic[\"Periodic Tail<br/>(continuous polling)\"]
    end

    Fetch -.->|mode| Modes
```

## Data Flow

### One-Shot Historical Fetch (Default)

1. EpicsArchiverReader constructed with start/end timestamps
2. Background worker thread starts immediately
3. Initiates HTTP GET request to Archiver Appliance `/retrieval/data/getData.raw`
4. Streams PB/HTTP response (PayloadInfo + ScalarDouble samples)
5. Parses protobuf messages line-by-line
6. Batches events by historical sample timestamps (using `batch_duration_sec`)
7. Pushes batches to event bus
8. Completes and thread exits (reader still running but idle)

### Periodic Tail Mode

1. Configured with `mode: periodic_tail`
2. Background worker thread runs continuously
3. Each iteration: generates time window from `now - lookback` to `now`
4. Fetches archiver data for that window
5. Processes and pushes new events
6. Waits `poll_interval_sec` (interruptible via condition_variable)
7. Repeats until reader destruction

## Configuration

### Basic One-Shot Historical Fetch

```yaml
reader:
  - epics-archiver:
      - name: my_archiver_reader
        url: "http://archiver-appliance.example.com"
        start_date: "2024-01-01T00:00:00Z"  # Required
        end_date: "2024-01-02T00:00:00Z"    # Optional
        batch_duration_sec: 1               # Split events by 1-second windows
        thread_pool_size: 2                 # Conversion thread pool
        pvs:
          - name: MY:ARCHIVER:PV
          - name: ANOTHER:HISTORICAL:PV
```

### Periodic Tail Mode

```yaml
reader:
  - epics-archiver:
      - name: continuous_archiver
        url: "http://archiver.example.com"
        mode: periodic_tail             # Enable continuous polling
        poll_interval_sec: 5            # Poll every 5 seconds
        lookback_sec: 60                # Fetch last 60 seconds each time
        batch_duration_sec: 1
        thread_pool_size: 2
        pvs:
          - name: MY:PV:NAME
```

### HTTP and Security Configuration

```yaml
reader:
  - epics-archiver:
      - name: secure_archiver
        url: "https://archiver.example.com"
        start_date: "2024-01-01T00:00:00Z"
        connect_timeout_sec: 30         # Connection timeout (default: 30)
        total_timeout_sec: 300          # Total operation timeout (default: 300)
        tls_verify_peer: true           # Verify SSL peer (default: true)
        tls_verify_host: true           # Verify hostname (default: true)
        pvs:
          - name: MY:PV
```

## Configuration Schema

### Required Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `url` | string | Archiver Appliance base URL (e.g., `http://archiver.example.com`) |
| `pvs` | list | Array of PV names or objects to fetch from archiver |

### One-Shot Mode (Default)

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `start_date` | string | — | **Required** ISO 8601 timestamp for query start |
| `end_date` | string | — | ISO 8601 timestamp for query end (defaults to `start_date` + 1 day) |
| `batch_duration_sec` | float | 1.0 | Split output batches using historical timestamps at this interval |

### Periodic Tail Mode

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mode` | string | `one_shot` | Set to `periodic_tail` to enable continuous polling |
| `poll_interval_sec` | float | 5.0 | Polling interval in seconds |
| `lookback_sec` | float | (poll_interval) | Seconds of history to fetch per poll (defaults to poll_interval) |
| `batch_duration_sec` | float | 1.0 | Batch split interval |

### HTTP and TLS Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `connect_timeout_sec` | float | 30.0 | Connection establishment timeout |
| `total_timeout_sec` | float | 300.0 | Total operation timeout |
| `tls_verify_peer` | bool | true | Verify SSL peer certificate |
| `tls_verify_host` | bool | true | Verify hostname matches certificate |

## Key Features

### Historical Data Retrieval

- **Time-Windowed Queries**: Query archiver data by start/end timestamps
- **Efficient Streaming**: Uses PB/HTTP streaming to minimize memory footprint
- **Timestamp-Based Batching**: Groups events by historical sample time (not wall-clock time)

### Continuous Polling (Periodic Tail)

- **Automated Tail Polling**: Continuously fetch new archiver data at configurable intervals
- **Lookback Window**: Configurable history depth to handle clock skew and backfill
- **Interruptible Wait**: Condition variable allows prompt shutdown without waiting for poll interval
- **Graceful Completion**: Stops cleanly on reader destruction

### HTTP & Security

- **Secure Defaults**: TLS verification enabled by default
- **Configurable Timeouts**: Separate connection and total operation timeouts
- **Chunked Transfer**: Efficient streaming via HTTP chunked encoding
- **Keep-Alive**: TCP keep-alive and compression support enabled

### Performance & Reliability

- **Background Worker Thread**: One-shot fetch or periodic polling runs off main reader construction
- **Thread Pool Processing**: Configurable conversion parallelism
- **Graceful Shutdown**: HTTP client cancellation on reader destruction prevents hanging requests
- **Metrics**: Prometheus metrics for events received, processed, and errors

## Data Types Supported

Currently supports:

- **SCALAR_DOUBLE**: Numeric scalar values from archiver

Future support planned for:

- SCALAR_INT
- SCALAR_STRING
- ARRAY_DOUBLE
- WAVEFORM data types

## Use Cases

- **Historical Data Backfill**: Ingest historical archiver data into MLDP pipeline
- **Data Migration**: Retrieve archived data for offline analysis or reprocessing
- **Continuous Archiver Monitoring**: Tail archiver for latest recorded values
- **Time-Series Analysis**: Combine with other readers for multi-source temporal analysis

## Common Patterns

### Backfill Yesterday's Data

```yaml
reader:
  - epics-archiver:
      - name: yesterday_backfill
        url: "http://archiver.example.com"
        start_date: "2024-01-09T00:00:00Z"
        end_date: "2024-01-10T00:00:00Z"
        pvs:
          - name: MY:SCALAR:PV
```

### Continuous Tail (Last Hour)

```yaml
reader:
  - epics-archiver:
      - name: tail_reader
        url: "http://archiver.example.com"
        mode: periodic_tail
        poll_interval_sec: 10
        lookback_sec: 3600            # Keep 1 hour of history per poll
        pvs:
          - name: MY:PV
```

### Fine-Grained Batching

```yaml
reader:
  - epics-archiver:
      - name: high_freq_archiver
        url: "http://archiver.example.com"
        start_date: "2024-01-01T00:00:00Z"
        batch_duration_sec: 0.1       # 100ms batches for high-frequency analysis
        pvs:
          - name: FAST:PV
```

## Metrics

The reader exposes standard MLDP reader metrics:

| Metric | Description |
|--------|-------------|
| `mldp_pvxs_driver_reader_events_received_total` | Total archiver samples received |
| `mldp_pvxs_driver_reader_events_total` | Successfully processed events |
| `mldp_pvxs_driver_reader_errors_total` | Conversion or HTTP errors |
| `mldp_pvxs_driver_reader_processing_time_ms` | Event processing latency (histogram) |
| `mldp_pvxs_driver_reader_pool_queue_depth` | Thread pool queue depth |

## Implementation Details

### Batch Splitting by Historical Timestamp

Batches are created by grouping events that fall within the same `batch_duration_sec` window using the **archiver sample time**, not the reader's wall-clock processing time. This preserves the original temporal structure of the data.

```cpp
// Example: batch_duration_sec: 1.0
// Event from 2024-01-01T12:34:56.500Z  -> Batch [12:34:56, 12:34:57)
// Event from 2024-01-01T12:34:57.200Z  -> Batch [12:34:57, 12:34:58)
```

### Background Worker Thread Lifecycle

- **One-Shot Mode**: Worker thread starts, fetches and processes, exits automatically
- **Periodic Mode**: Worker thread runs until reader destruction
- **Shutdown**: HTTP request is cancelled immediately; thread joins before destructor completes

### Configuration Validation

- `total_timeout_sec >= connect_timeout_sec` (enforced)
- All timeout values must be positive (enforced)
- URL must be valid and accessible
- At least one PV required
- Start date required for one-shot mode
