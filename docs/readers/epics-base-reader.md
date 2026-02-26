# EpicsBaseReader (Polling-Based)

The `EpicsBaseReader` provides EPICS Channel Access monitoring using a polling-based approach. It uses the legacy EPICS Base client library with a dedicated monitor poller that periodically drains monitor queues.

**Registration Type:** `"epics-base"`

| File           | Location                                         |
|----------------|--------------------------------------------------|
| Header         | `include/reader/impl/epics/EpicsBaseReader.h`    |
| Implementation | `src/reader/impl/epics/EpicsBaseReader.cpp`      |

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
        thread_pool_size: 2          # Conversion thread pool size
        monitor_poll_threads: 2      # Number of polling threads
        monitor_poll_interval_ms: 5  # Polling interval in ms
        pvs:
          - name: MY:PV:NAME
          - name: ANOTHER:PV
```

## Key Features

- **Polling Interval Control**: Configurable polling frequency
- **Multiple Poll Threads**: Parallel queue draining
- **Mutex Protection**: Thread-safe queue access via `epics_base_drain_mutex_`
- **Legacy Compatibility**: Works with traditional EPICS Channel Access

## Use Cases

- Legacy EPICS installations without PVAccess support
- Environments requiring Channel Access protocol
- Systems where polling is preferred over event-driven updates
