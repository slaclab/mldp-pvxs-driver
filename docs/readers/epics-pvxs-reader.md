# EpicsPVXSReader (Event-Driven)

The `EpicsPVXSReader` provides modern EPICS PVAccess monitoring using an event-driven subscription model. It uses the PVXS client library for direct PV access with immediate event callbacks.

**Registration Type:** `"epics-pvxs"`

| File           | Location                                         |
|----------------|--------------------------------------------------|
| Header         | `include/reader/impl/epics/EpicsPVXSReader.h`    |
| Implementation | `src/reader/impl/epics/EpicsPVXSReader.cpp`      |

## Architecture

```mermaid
flowchart TB
    subgraph EpicsPVXSReader[\"EpicsPVXSReader\"]
        subgraph Context[\"pvxs::client::Context\"]
            subgraph Subscriptions[\"PV Subscriptions\"]
                M1[\"monitor(pv1) --> callback\"]
                M2[\"monitor(pv2) --> callback\"]
                MN[\"monitor(pvN) --> callback\"]
            end
        end

        Subscriptions -->|immediate event| ReaderPool

        subgraph ReaderPool[\"Reader Thread Pool<br/>(conditional usage)\"]
            Condition[\"if thread_count > 1 → use pool<br/>else → direct execution\"]
        end

        ReaderPool --> ProcessEvent

        ProcessEvent[\"processEvent()<br/>(data conversion + push)\"]
    end
```

## Data Flow

1. PVXS context establishes subscriptions via `pva_context_.monitor(pv)`
2. Subscription callbacks fire immediately on PV value changes
3. Events are dispatched to the reader thread pool (or direct if single-threaded)
4. `processEvent()` converts PVXS Value to protobuf
5. Event batch is pushed to the bus

## Configuration

```yaml
reader:
  - epics-pvxs:
      - name: my_pvxs_reader
        thread_pool_size: 2           # Conversion thread pool size
        column_batch_size: 50         # NTTable column batch size
        pvs:
          - name: MY:PV:NAME
          - name: BSA:TABLE:PV
            option:                   # For SLAC BSAS NTTable with row timestamps
              type: slac-bsas-table
              tsSeconds: secondsPastEpoch
              tsNanos: nanoseconds
```

## Key Features

- **Event-Driven**: Immediate response to PV changes (no polling overhead)
- **Smart Threading**: Conditional thread pool usage based on thread count
- **NTTable Support**: Special handling for tabular data with row timestamps
- **PVXS Options**: Support for custom channel options

## Conditional Parallelization

The reader implements smart thread pool decisions to avoid overhead:

```cpp
// Line 132 in EpicsPVXSReader.cpp
reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr
```

- **Single thread (= 1)**: Bypass thread pool, execute directly
- **Multiple threads (> 1)**: Use thread pool for parallel conversion

## SLAC BSAS NTTable Handling

For PVs that return NTTable structures with per-row timestamps (SLAC BSAS format):

```yaml
pvs:
  - name: BSA:TABLE:PV
    option:
      type: slac-bsas-table
      tsSeconds: secondsPastEpoch    # Column containing epoch seconds
      tsNanos: nanoseconds           # Column containing nanoseconds
```

- Each table column becomes a separate source in the event batch
- Timestamps are extracted per-row from the specified fields
- Source name equals the column field name
- Conversion handled by `BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch()`

## Use Cases

- Modern EPICS installations with PVAccess support
- High-frequency PV updates requiring minimal latency
- Applications needing immediate event notification
- Systems with NTTable data structures
