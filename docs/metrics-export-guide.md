# Metrics Export and System Monitoring

## Overview

The MLDP PVXS driver provides comprehensive metrics collection and export capabilities, including:

1. **Prometheus endpoint** for HTTP-based metrics scraping
2. **JSON Lines (JSONL) file exports** for periodic logging
3. **System-level metrics** (CPU, memory, file descriptors, I/O) using the metric-grabber library
4. **Manual dump triggers** via keyboard shortcuts and signals

## Features

### Prometheus Endpoint Configuration

The driver can expose metrics via a Prometheus-compatible HTTP endpoint using the `metrics` configuration block:

```yaml
metrics:
  endpoint: 0.0.0.0:9464
  scan-interval-seconds: 1
```

- **`endpoint`** (required): Host:port where the Prometheus exposer binds
- **`scan-interval-seconds`** (optional, default: 1): Interval between system metrics collection scans

The endpoint serves metrics in Prometheus text exposition format at `http://<endpoint>/metrics`.

### System Metrics Collected

When the Prometheus endpoint is configured, the driver collects:

**Reader metrics** (per PV/source):
- `mldp_pvxs_driver_reader_events_total` - Total events processed
- `mldp_pvxs_driver_reader_events_received_total` - Raw EPICS updates received
- `mldp_pvxs_driver_reader_errors_total` - Reader failures
- `mldp_pvxs_driver_reader_processing_time_ms` - Histogram of conversion time (ms)

- `mldp_pvxs_driver_reader_queue_depth` - Queued PV updates awaiting processing
- `mldp_pvxs_driver_reader_pool_queue_depth` - Queued conversion tasks awaiting processing

**Connection pool metrics**:
- `mldp_pvxs_driver_pool_connections_in_use` - Active connections
- `mldp_pvxs_driver_pool_connections_available` - Idle connections

**Controller metrics**:
- `mldp_pvxs_driver_controller_send_time_seconds` - Histogram of send duration (seconds)
- `mldp_pvxs_driver_controller_queue_depth` - Queued tasks
- `mldp_pvxs_driver_controller_channel_queue_depth` - Per-worker channel queue depth

**Bus metrics**:
- `mldp_pvxs_driver_bus_push_total` - Events pushed to bus
- `mldp_pvxs_driver_bus_failure_total` - Push failures
- `mldp_pvxs_driver_bus_payload_bytes_total` - Total protobuf bytes sent
- `mldp_pvxs_driver_bus_payload_bytes_per_second` - Current throughput
- `mldp_pvxs_driver_bus_stream_rotations_total` - Stream open/close cycles

**System-level metrics** (sampled at configured interval):
  - **Process CPU time (counters):**
    - `mldp_pvxs_driver_process_cpu_user_ticks_total` - Total user CPU time in clock ticks
    - `mldp_pvxs_driver_process_cpu_system_ticks_total` - Total system CPU time in clock ticks
    - `mldp_pvxs_driver_process_cpu_children_user_ticks_total` - Total children user CPU time in clock ticks
    - `mldp_pvxs_driver_process_cpu_children_system_ticks_total` - Total children system CPU time in clock ticks
  - **Memory metrics (gauges):**
    - `mldp_pvxs_driver_process_memory_virtual_bytes` - Virtual memory size in bytes
    - `mldp_pvxs_driver_process_memory_rss_bytes` - Resident set size (physical) in bytes
    - `mldp_pvxs_driver_process_memory_virtual_peak_bytes` - Peak virtual memory size in bytes
    - `mldp_pvxs_driver_process_memory_rss_anon_bytes` - Anonymous RSS in bytes
    - `mldp_pvxs_driver_process_memory_rss_file_bytes` - File-backed RSS in bytes
    - `mldp_pvxs_driver_process_memory_rss_shmem_bytes` - Shared memory RSS in bytes
    - `mldp_pvxs_driver_process_memory_rss_total_bytes` - Total RSS (anon + file + shmem) in bytes
  - **I/O metrics (counters):**
    - `mldp_pvxs_driver_process_io_read_bytes` - Total bytes read
    - `mldp_pvxs_driver_process_io_write_bytes` - Total bytes written
    - `mldp_pvxs_driver_process_io_cancelled_write_bytes` - Total cancelled write bytes
  - **Context switch metrics (counters):**
    - `mldp_pvxs_driver_process_context_switches_voluntary` - Total voluntary context switches
    - `mldp_pvxs_driver_process_context_switches_involuntary` - Total involuntary context switches
  - **Resource metrics (gauges):**
    - `mldp_pvxs_driver_process_fds_open` - Number of open file descriptors
    - `mldp_pvxs_driver_process_threads` - Number of threads in process
    - `mldp_pvxs_driver_process_priority` - Process priority (nice value)
    - `mldp_pvxs_driver_process_nice` - Nice value (0-20, lower = higher priority)

### JSON Lines File Exports

For periodic file-based logging, use the `PeriodicMetricsDumper` background thread:

```bash
./mldp_pvxs_driver \
  --metrics-output /path/to/metrics.jsonl \
  --metrics-interval 5
```

- **`--metrics-output FILE`**: Path for JSONL exports (default: `metrics.jsonl`)
- **`--metrics-interval SECONDS`**: Dump interval (default: 60)

### Manual Metrics Dumps

Trigger manual dumps without periodic features:
- Press **Ctrl+P** in the foreground terminal (outputs `MetricsSnapshot` format to stdout)
- Or send signals: `kill -USR1 <pid>` or `kill -QUIT <pid>`

### Runtime Controls

- **Ctrl+D**: Toggle the periodic metrics dumper on/off (while the driver is running)

## Output Formats

### Prometheus Text Exposition (Endpoint)

When configured via YAML, metrics are available via HTTP:

```bash
curl http://localhost:9464/metrics
```

### JSON Lines Format (File)

Each line is a complete JSON object:

```json
{
  "timestamp_ms": 1704623456123,
  "timestamp_iso": "2024-01-07T15:30:56Z",
  "metrics": {
    "mldp_pvxs_driver_bus_push_total": [
      {
        "source": "QUAD:IN20:361:BACT",
        "value": 42
      }
    ],
    "mldp_pvxs_driver_reader_processing_time_ms": [
      {
        "source": "QUAD:IN20:361:BACT",
        "histogram": "bucket",
        "le": "0.5",
        "value": 10
      },
      {
        "source": "QUAD:IN20:361:BACT",
        "histogram": "sum",
        "value": 125.5
      },
      {
        "source": "QUAD:IN20:361:BACT",
        "histogram": "count",
        "value": 42
      }
    ]
  }
}
```

**Metric structure**:
- Tagged metrics include label fields (e.g., `"source"`, `"pv"`) plus `"value"`
- Histograms include a `"histogram"` field (`"bucket"`, `"sum"`, or `"count"`)
- Untagged metrics have only `"value"` field

## Example Output (JSON Lines)

See the format specification above. Histogram samples are grouped under the base metric name with a `"histogram"` label indicating the sample type.

## Implementation Details

### Classes and functions

- **`MetricsConfig`**: Parses YAML configuration for Prometheus endpoint and scan interval
- **`Metrics`**: Prometheus registry and metric family accessors
- **`PeriodicMetricsDumper`**: Background thread manager for periodic file exports
  - `start()`: Begins periodic dumping
  - `stop()`: Gracefully stops the background thread
  - Thread-safe with mutex protection

- **`appendMetricsToFile()`**: Appends JSONL-formatted metrics with timestamps
- **`serializeMetricsJsonl()`**: Converts Prometheus metrics to structured JSON with:
  - Metrics grouped by name as arrays
  - Histogram samples tagged with `"histogram"` (bucket/sum/count)
- **`MetricsSnapshot`** (`include/metrics/MetricsSnapshot.h`, `src/metrics/MetricsSnapshot.cpp`): Builds snapshots from the Prometheus registry and formats for stdout
  - **Snapshot creation**: `getSnapshot(const Metrics& metrics)` parses the Prometheus text exposition to collect per-reader and pool metrics.
  - **Formatting**: `toString(const MetricsData& snapshot)` renders a human-readable report for interactive dumps.
  - **Usage path**: the CLI triggers `MetricsSnapshot` when Ctrl+P or SIGUSR1/SIGQUIT is received.

### Architecture

1. **Signal Handling**: Background thread sleeps for the configured interval
2. **Thread Safety**: Mutex protects output path changes during operation
3. **Graceful Shutdown**: Dumper stops cleanly when main thread exits
4. **Error Handling**: Failed writes log errors but don't crash the application

## Configuration Examples

1. **Prometheus endpoint with 2-second system scan**:
   ```yaml
   metrics:
     endpoint: 0.0.0.0:9464
     scan-interval-seconds: 2
   ```

2. **JSONL exports every 5 seconds**:
   ```bash
   ./mldp_pvxs_driver --metrics-output metrics.jsonl --metrics-interval 5
   ```

3. **Manual dumps only**:
   ```bash
   ./mldp_pvxs_driver  # No metrics block or --metrics-output
   ```

4. **Combined endpoint and file exports**:
   Configure both the YAML `metrics` block and use `--metrics-output` together.

## Benefits

1. **Flexible export**: Prometheus HTTP endpoint or JSONL file logging
2. **System visibility**: CPU, I/O, and resource metrics alongside application metrics
3. **Time-series analysis**: Track trends over time with periodic JSONL dumps
4. **Parseable format**: Standard JSON tools (jq, Python json module) for analysis
5. **Low overhead**: Configurable scan intervals prevent I/O saturation
6. **Non-blocking**: Background threads for file exports don't affect main processing
7. **Operational controls**: Runtime toggle with Ctrl+D, manual dumps with Ctrl+P
