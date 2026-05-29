# SlacCalendarReader (SLAC Calendar)

The `SlacCalendarReader` fetches beamline experiment schedule events from the SLAC
calendar HTTP API and publishes them as `ConfigurationPayload` and
`ConfigurationActivationPayload` pairs onto the driver bus. Downstream, an
`mldp-configuration` writer persists those payloads to the MLDP annotation service.

**Registration Type:** `"slac-calendar"`

**Status:** Implemented

File           | Location
-------------- | ---------------------------------------------------------------
Header         | `include/reader/impl/slac_calendar/SlacCalendarReader.h`
Implementation | `src/reader/impl/slac_calendar/SlacCalendarReader.cpp`
Config         | `include/reader/impl/slac_calendar/SlacCalendarReaderConfig.h`

## Build Option & Required Libraries

- **Build option:** none (always built)
- **Required libraries/components:**
  - libcurl (HTTP client)
  - nlohmann/json (JSON parsing)
  - libxml2 (HTML inner-text extraction for `details` field)

## Architecture

```mermaid
flowchart TB
    subgraph SlacCalendarReader["SlacCalendarReader"]
        subgraph Worker["Background Worker Thread"]
            Fetch["HTTP GET\n/{experiment}/events.json"]
            Parse["JSON array parser"]
        end

        Worker --> PushCfg["Push ConfigurationPayload\nto IDataBus"]
        Worker --> PushAct["Push ConfigurationActivationPayload\nto IDataBus"]
        PushCfg --> Bus["IDataBus"]
        PushAct --> Bus

        PushAct --> Wait["Sleep rescan-interval-sec\n(interruptible)"]
        Wait -->|loop| Fetch
    end

    subgraph API["SLAC Calendar HTTP API"]
        Endpoint["base-url/{experiment}/events.json\n?start_time=...&end_time=...&limit=..."]
    end

    Fetch -->|HTTP GET| Endpoint
    Endpoint -->|JSON array| Parse
```

## Operating Modes

### One-Shot (Default)

When `rescan-interval-sec` is `0.0` (the default), the reader:

1. Computes `[now - lookback-days, now + lookahead-days]` time window (or uses `start-date` on first run).
2. Issues one HTTP GET per configured experiment.
3. Parses the JSON array — each event becomes one `ConfigurationPayload` + one
   `ConfigurationActivationPayload` pushed to the bus.
4. Worker thread exits; reader stays alive but idle.

```yaml
reader:
  - slac-calendar:
      - name: cal_reader_once
        base-url: https://calendar.slac.stanford.edu
        experiments:
          - lcls
          - facet
        lookahead-days: 30
        lookback-days: 1
```

### Periodic Rescan

When `rescan-interval-sec > 0`, the worker repeats the fetch on that interval.
The sleep between iterations is interruptible so shutdown is prompt.

```yaml
reader:
  - slac-calendar:
      - name: cal_reader_rescan
        base-url: https://calendar.slac.stanford.edu
        experiments:
          - lcls
        lookahead-days: 30
        lookback-days: 1
        rescan-interval-sec: 3600.0   # re-fetch every hour
```

## Configuration

### Required Parameters

Parameter        | Type     | Description
---------------- | -------- | -----------------------------------------------------------
`name`           | string   | **Required.** Unique reader instance name (non-empty).
`base-url`       | string   | **Required.** Base URL of the SLAC calendar API (no trailing slash).
`experiments`    | sequence | **Required.** List of experiment names to fetch (e.g. `lcls`, `facet`).
`lookahead-days` | int      | **Required.** Days into the future to include. Must be > 0.

### Optional Parameters

Parameter              | Type   | Default | Description
---------------------- | ------ | ------- | ---------------------------------------------------
`lookback-days`        | int    | `1`     | Days into the past to include. Must be >= 0.
`start-date`           | string | —       | First-run start date override (`YYYY-MM-DD`). Used only on the first fetch.
`rescan-interval-sec`  | double | `0.0`   | Repeat fetch interval in seconds. `0` = run once.
`connect-timeout-sec`  | int    | `30`    | HTTP connection timeout (seconds).
`total-timeout-sec`    | int    | `60`    | HTTP total request timeout (seconds). Must be >= `connect-timeout-sec`.
`tls-verify-peer`      | bool   | `true`  | Verify TLS peer certificate.
`tls-verify-host`      | bool   | `true`  | Verify TLS hostname against certificate.
`event-limit`          | int    | `1000`  | Maximum events per API request (`limit=` query parameter).

**Validation rules:**

- `name`, `base-url`, and `experiments` are required and must be non-empty.
- `lookahead-days` must be > 0.
- `lookback-days` must be >= 0.
- `total-timeout-sec` must be >= `connect-timeout-sec`.
- `start-date` (if provided) must match `YYYY-MM-DD` format.

## Published Payloads

For each calendar event the reader pushes **two** `EventBatch` items to the bus:

### 1. `ConfigurationPayload`

Field                    | Source in JSON
------------------------ | -----------------------------------------
`configuration_name`     | `program_name`
`category`               | `calendar`
`description`            | `description`
`tags`                   | `tags[]`
`attributes["experiment"]`| experiment name (from config)
`attributes["poc"]`       | `poc` (if present)
`attributes["note"]`      | `note` (if present)
`attributes["config"]`    | `config` (if present)
`attributes["machine"]`   | `machine` (if present)
`attributes["details"]`   | inner text extracted from `details` HTML (if present)
`attributes["hutch_name"]`| `hutch.name` (if present)
`attributes["hutch_color"]`| `hutch.color` (if present)
`attributes["hutch_line"]`| `hutch.line` (if present)

### 2. `ConfigurationActivationPayload`

Field                    | Source in JSON
------------------------ | -----------------------------------------
`client_activation_id`   | `url`
`configuration_name`     | `program_name`
`start_time`             | `start` (ISO 8601 with timezone)
`end_time`               | `end` (ISO 8601 with timezone)
`description`            | `description`
`tags`                   | `tags[]`
`attributes["experiment"]`| experiment name
`attributes["calendar"]` | `calendar`

Both batches carry `root_source = reader_name` and `reader_name = reader_name`.

## Key Features

- **Multi-experiment**: Fetches multiple experiment calendars in a single scan pass.
- **HTML detail extraction**: Strips HTML tags from the `details` field using libxml2 so
  only the plain URL is stored as an attribute.
- **Timezone-aware timestamps**: Parses ISO 8601 timestamps with numeric timezone offsets
  and converts to epoch seconds.
- **Periodic rescan**: Interruptible sleep; no busy-wait.
- **Configurable TLS**: Peer and host verification can be disabled for development
  environments with self-signed certificates.

## Typical Use: Beamline Schedule Pipeline

```yaml
writer:
  mldp-configuration:
    - name: cal_writer
      thread-pool: 2
      deadline-seconds: 10
      mldp-annotation-pool:
        annotation-url: grpc://annotation-host:50053
        min-conn: 1
        max-conn: 4

reader:
  - slac-calendar:
      - name: cal_reader
        base-url: https://calendar.slac.stanford.edu
        experiments:
          - lcls
          - facet
        lookahead-days: 30
        lookback-days: 1
        rescan-interval-sec: 3600.0

routing:
  cal_writer:
    from: [cal_reader]
```

**What happens:**
1. `SlacCalendarReader` queries the calendar API for each experiment.
2. Each event becomes two bus pushes: a configuration + an activation.
3. `MLDPConfigurationWriter` fans each pair into `saveConfiguration` +
   `saveConfigurationActivation` gRPC calls.

## Metrics

This reader does not use the standard `EpicsReaderBase` thread pool and therefore does
not expose per-event metrics. Pipeline health can be observed via the writer-side metrics
on the paired `mldp-configuration` writer instance.

## See Also

- [MLDP Configuration Writer](../writers/mldp-configuration-writer.md) — consumes `ConfigurationPayload` / `ConfigurationActivationPayload`
- [Readers Overview](readers.md)
- [Configuration Reference](../guides/configuration.md#slac-calendar-reader)
