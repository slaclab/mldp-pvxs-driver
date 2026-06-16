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
        thread-pool: 2               # Event conversion thread pool size
        column-batch-size: 50        # NTTable column batch size
        monitor-poll-threads: 2      # Number of polling threads
        monitor-poll-interval-ms: 5  # Polling interval in ms
        pvs:
          - name: MY:PV:NAME
          - name: ANOTHER:PV
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
