# EpicsBaseReader (Polling-Based)

The `EpicsBaseReader` provides EPICS Channel Access monitoring using a polling-based approach. It uses the legacy EPICS Base client library with a dedicated monitor poller that periodically drains monitor queues.

**Registration Type:** `"epics-base"`

File           | Location
-------------- | -----------------------------------------------
Header         | `include/reader/impl/epics/base/EpicsBaseReader.h`
Implementation | `src/reader/impl/epics/base/EpicsBaseReader.cpp`

## Build Option & Required Libraries

- **Build option:** none (always built)
- **Required libraries/components:**
  - EPICS Base runtime/dev libs (`libCom`, `libca`, `libpvData`, `libpvAccess`, `libpvaClient`, `libpvAccessCA`)
- **Configure-time hints:** `EPICS_BASE`, `EPICS_HOST_ARCH`
- **Optional link mode:** `-DMLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC=ON` for static EPICS/PVXS linking

## Architecture

```mermaid
flowchart TB
    subgraph EpicsBaseReader[\"EpicsBaseReader\"]
        subgraph MonitorPoller[\"EpicsBaseMonitorPoller\"]
            PT1[\"Poll Thread 1\"]
            PT2[\"Poll Thread 2\"]
            PTN[\"Poll Thread N\"]

            PT1 --> MonitorQueues
            PT2 --> MonitorQueues
            PTN --> MonitorQueues

            MonitorQueues[\"Monitor Queues<br/>(per PV)\"]
        end

        MonitorQueues --> DrainQueue

        DrainQueue[\"drainEpicsBaseQueue()<br/>(mutex protected)\"]

        DrainQueue --> ReaderPool

        ReaderPool[\"Reader Thread Pool<br/>(data conversion)\"]
    end
```

## Data Flow

1. EPICS PV updates are captured by pvaClient monitors
2. Updates are stored in per-PV monitor queues
3. Dedicated polling threads periodically drain queues
4. `drainEpicsBaseQueue()` is called (protected by mutex)
5. Events are dispatched to the reader thread pool
6. `processEvent()` converts data and pushes to the bus

## Configuration

```yaml
reader:
  - epics-base:
      - name: my_base_reader
        thread-pool: 1               # Event conversion thread pool size (default 1)
        column-batch-size: 50        # Columns per bus push for NTTable data (default 50)
        monitor-poll-threads: 2      # Number of polling threads (default 2)
        monitor-poll-interval-ms: 5  # Polling interval in ms (default 5)
        metadata:                    # optional static metadata attached to every PV
          facility: LCLS
        pvs:
          - name: MY:PV:NAME
            metadata:                # optional per-PV metadata (merged with reader metadata)
              unit: mm
          - name: ANOTHER:PV
          - name: BSA:TABLE:PV
            option:                  # optional; map form enables SLAC BSAS NTTable mode
              type: slac-bsas-table
              tsSeconds: secondsPastEpoch   # default
              tsNanos: nanoseconds          # default
              column-batch-size: 1          # default 1; 0 = all columns at once
        pvs-files:                   # optional; PV names loaded from text files
          - path: scalars.txt        # relative to the main config file directory
            option: {}               # optional, applied to every PV in the file
            metadata:                # optional, applied to every PV in the file
              source-file: scalars
```

Key                                 | Level      | Default            | Description
----------------------------------- | ---------- | ------------------ | ---------------------------------------------------------
`name`                              | reader     | — (required)       | Reader instance name; must be non-empty
`thread-pool`                       | reader     | `1`                | Event conversion thread pool size
`column-batch-size`                 | reader     | `50`               | Columns per bus push for NTTable data
`monitor-poll-threads`              | reader     | `2`                | Number of monitor polling threads
`monitor-poll-interval-ms`          | reader     | `5`                | Monitor queue polling interval (ms)
`metadata`                          | reader     | none               | Static string map attached to every PV of this reader
`pvs`                               | reader     | none               | Sequence of PV entries (with `pvs-files`, optional; PV names must be unique)
`pvs[].name`                        | `pvs[]`    | — (required)       | EPICS PV name
`pvs[].option`                      | `pvs[]`    | none               | Scalar option string, or a map (see `option.*`)
`pvs[].metadata`                    | `pvs[]`    | none               | Static string map for this PV
`pvs-files[].path`                  | `pvs-files[]` | — (required)    | PV list file (one PV per line, `#` comments); relative → config file directory
`pvs-files[].option`                | `pvs-files[]` | none            | Same as `pvs[].option`, applied to every PV in the file
`pvs-files[].metadata`              | `pvs-files[]` | none            | Same as `pvs[].metadata`, applied to every PV in the file
`option.type`                       | `option`   | none               | `slac-bsas-table` enables BSAS NTTable mode; other values are ignored
`option.tsSeconds`                  | `option`   | `secondsPastEpoch` | Per-row epoch-seconds column (BSAS mode, must not be empty)
`option.tsNanos`                    | `option`   | `nanoseconds`      | Per-row nanoseconds column (BSAS mode, must not be empty)
`option.column-batch-size`          | `option`   | `1`                | Columns per batch push in BSAS mode; `0` = all at once

`backend` is rejected (choose the `epics-pvxs` or `epics-base` reader type).
`option.sourceName` is rejected with `type: slac-bsas-table`: the source name always
equals the column field name.

### Loading PVs from files

PV names can also be loaded from plain text files (one PV per line, blank lines and
`#` comment lines ignored) with `pvs-files`. Each entry's `option` and `metadata`
apply to every PV in that file and have the same semantics as on an inline `pvs[]`
entry. Relative paths resolve against the directory of the main config file.
See [Configuration Reference](../guides/configuration.md#common-reader-keys-epics-pvxs-and-epics-base).

```yaml
reader:
  - epics-base:
      - name: bsas_reader
        pvs-files:
          - path: /etc/mldp-pvxs/gen1-tables.txt   # SLAC BSAS NTTable gen1
            option:
              type: slac-bsas-table
              tsSeconds: secondsPastEpoch
              tsNanos: nanoseconds
              column-batch-size: 4
          - path: scalars.txt                      # relative → config file directory
            metadata:                              # optional, applied to every PV in the file
              group: scalars
```

```text
# gen1-tables.txt
BSAS:TABLE:ONE
BSAS:TABLE:TWO
```

## Key Features

- **Polling Interval Control**: Configurable polling frequency
- **Multiple Poll Threads**: Parallel queue draining
- **Mutex Protection**: Thread-safe queue access via `epics_base_drain_mutex_`
- **Legacy Compatibility**: Works with traditional EPICS Channel Access

## SLAC BSAS NTTable Handling

`EpicsBaseReader` supports the same SLAC BSAS NTTable mode as `EpicsPVXSReader`.
Each NTTable column (PV name) becomes a separate source in the event batch;
the two per-row timestamp columns are consumed for row indexing and are not
forwarded as sources.

```yaml
pvs:
  - name: BSA:TABLE:PV
    option:
      type: slac-bsas-table
      tsSeconds: secondsPastEpoch   # column holding per-row epoch seconds
      tsNanos: nanoseconds          # column holding per-row nanoseconds
      column-batch-size: 1          # columns per batch push (0 = all at once)
```

Conversion is handled by `EpicsPVDataBatchConversion::tryBuildNtTableRowTsBatch()`.

For a full description of the BSAS NTTable structure, field layout, and a
concrete annotated example see
[SLAC BSAS NTTable Gen 1](slac-bsas-table-gen1.md) and [Gen 2](slac-bsas-table-gen2.md).

## Use Cases

- Legacy EPICS installations without PVAccess support
- Environments requiring Channel Access protocol
- Systems where polling is preferred over event-driven updates
