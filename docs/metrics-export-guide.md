# Metrics Export Implementation

## Overview
The `appendMetricsToFile` function supports periodic metrics collection with improved formatting. Metrics are exported in **JSON Lines (JSONL)** format with timestamps, which is better suited for time-series data than CSV.

## Features

### 1. JSON Lines Format (Recommended over CSV)
- **Why JSONL instead of CSV?**
  - Preserves metric structure and labels hierarchically
  - Each line is a complete, self-contained JSON object
  - Tools like `jq` can easily parse and filter lines
  - Easier to handle numeric types and nested data
  - Standard format for time-series logging

### 2. Automatic Periodic Dumping
A background thread (`PeriodicMetricsDumper`) periodically appends metrics to a specified file at configurable intervals.

### 3. Timestamped Entries
Each metrics dump includes:
- `timestamp_ms`: Milliseconds since epoch (for machine processing)
- `timestamp_iso`: ISO 8601 formatted timestamp (human-readable)
- `metrics`: All collected Prometheus metrics as key-value pairs

## Usage

### Command Line Arguments

```bash
./mldp_pvxs_driver \
  --metrics-output /path/to/metrics.jsonl \
  --metrics-interval 5
```

- `--metrics-output FILE`: Path where metrics will be appended (JSON Lines format)
- `--metrics-interval SECONDS`: Dump interval in seconds (default: 5)

### Manual Metrics Dump
You can still trigger manual dumps without using the periodic feature:
- Press **Ctrl+P** in the foreground terminal
- Or send signals: `kill -USR1 <pid>` or `kill -QUIT <pid>`

These manual dumps print to stdout using `MetricsSnapshot::toString()`.

## Example Output (JSON Lines)

```json
{
  "timestamp_ms": 1704623456123,
  "timestamp_iso": "2024-01-07T15:30:56Z",
  "metrics": {
    "mldp_pvxs_bytes_received_total": 1048576,
    "mldp_pvxs_messages_processed_total": 42,
    "mldp_pvxs_processing_time_seconds": 0.523
  }
}
{
  "timestamp_ms": 1704623516123,
  "timestamp_iso": "2024-01-07T15:31:56Z",
  "metrics": {
    "mldp_pvxs_bytes_received_total": 2097152,
    "mldp_pvxs_messages_processed_total": 85,
    "mldp_pvxs_processing_time_seconds": 1.047
  }
}
```

## Processing Metrics Data

### Using `jq` to analyze JSONL metrics:

```bash
# Get all timestamps
jq .timestamp_iso metrics.jsonl

# Get specific metric values over time
jq '.metrics.mldp_pvxs_messages_processed_total' metrics.jsonl

# Calculate metric deltas
jq -s '.[1:] | map(.metrics.mldp_pvxs_bytes_received_total - .[0].metrics.mldp_pvxs_bytes_received_total)' metrics.jsonl

# Filter metrics from a specific time range
jq 'select(.timestamp_ms > 1704623500000 and .timestamp_ms < 1704623600000)' metrics.jsonl
```

### Convert to CSV (if needed):

```bash
jq -r '[.timestamp_iso, .metrics | to_entries[] | [.key, .value]] | @csv' metrics.jsonl
```

## Implementation Details

### Classes and functions

- **`PeriodicMetricsDumper`**: Background thread manager for periodic metric exports
  - `start()`: Begins periodic dumping
  - `stop()`: Gracefully stops the background thread
  - Thread-safe with mutex protection

- **`appendMetricsToFile()`**: Appends JSONL-formatted metrics with timestamps
- **`serializeMetricsJsonl()`**: Converts Prometheus metrics to structured JSON

### MetricsSnapshot
`MetricsSnapshot` (`include/metrics/MetricsSnapshot.h`, `src/metrics/MetricsSnapshot.cpp`) builds a structured snapshot from the Prometheus registry and formats it for stdout.

- **Snapshot creation**: `getSnapshot(const Metrics& metrics)` parses the Prometheus text exposition to collect per-reader and pool metrics.
- **Formatting**: `toString(const MetricsData& snapshot)` renders a human-readable report for interactive dumps.
- **Usage path**: the CLI triggers `MetricsSnapshot` when Ctrl+P or SIGUSR1/SIGQUIT is received.

Example stdout output:
```text
================================ METRICS DUMP ========================

READER STATISTICS:
─────────────────────────────────────────────────────────────────
PV: pv_1
  Pushes:     42
  Total Data: 1.23 MB
  Rate:       12.34 KB/s

CONNECTION POOL:
─────────────────────────────────────────────────────────────────
  In Use:     1
  Available:  3
  Total:      4
=====================================================================
```

### Architecture

1. **Signal Handling**: Background thread sleeps for the configured interval
2. **Thread Safety**: Mutex protects output path changes during operation
3. **Graceful Shutdown**: Dumper stops cleanly when main thread exits
4. **Error Handling**: Failed writes log errors but don't crash the application

## Configuration Examples

### High-frequency monitoring (every 10 seconds):
```bash
./mldp_pvxs_driver --metrics-output metrics.jsonl --metrics-interval 10
```

### Low-frequency baseline (every 5 minutes):
```bash
./mldp_pvxs_driver --metrics-output metrics.jsonl --metrics-interval 300
```

### Disabled (manual dumps only):
```bash
./mldp_pvxs_driver  # No --metrics-output argument
```

### Runtime controls
- Press **Ctrl+P** to dump metrics to stdout (MetricsSnapshot formatted).
- Press **Ctrl+D** to toggle the periodic metrics dumper on/off.

## Benefits

1. **Time-series analysis**: Easily track metric trends over time
2. **Parseable format**: Standard JSON tools work seamlessly
3. **Low overhead**: Configurable intervals prevent I/O saturation
4. **Non-blocking**: Periodic dumps happen in a background thread
5. **Flexible**: Can be combined with manual dumps via Ctrl+P
