# EpicsPVXSReader Implementation

This guide describes the `EpicsPVXSReader` implementation, which provides event-driven EPICS PVAccess monitoring.

> **Related:** [Reader Implementations](readers-implementation.md) | [Architecture Overview](../reference/architecture.md)

## Overview

`EpicsPVXSReader` is the PVXS-based reader used for low-latency event-driven updates. It subscribes to PV changes and converts them directly into MLDP payloads.

## Runtime Model

- Uses subscription callbacks rather than polling.
- Can delegate conversion work to a thread pool when configured.
- Pushes batches to `IDataBus` for downstream processing.

## Best Fit

- High-frequency updates.
- Modern EPICS deployments with PVAccess support.
- Low-latency ingestion pipelines.

## BSAS Support

`EpicsPVXSReader` (`epics-pvxs`) is scalar-only. SLAC BSAS NTTables are handled by
subclasses that reuse its PVXS context, subscription and metrics plumbing:

- `EpicsPVXSBsasTableReaderBase` — overrides the protected virtual `processEvent()`,
  converts the table through `BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch()`
  (optionally with a `ColumnFilter` applied before typed conversion), groups columns
  into frames and pushes tabular batches plus the `end_of_batch_group` marker.
- `EpicsPVXSBsasGen1Reader` (`epics-pvxs-bsas-gen1`) — no filter.
- `EpicsPVXSBsasGen2Reader` (`epics-pvxs-bsas-gen2`) — keeps only `<PV>.Val` columns.

Subclasses use the protected constructor, which does not start monitoring; the
most-derived constructor calls `addPV(pvNames())` once the object is fully built,
and the table base destructor drains the pool (`stopProcessing()`) before its
members are destroyed.

See:

- [SLAC BSAS NTTable Gen 1](slac-bsas-table-gen1.md)
- [SLAC BSAS NTTable Gen 2](slac-bsas-table-gen2.md)

## Implementation Notes

- Shares common EPICS conversion helpers with `EpicsReaderBase`.
- Makes thread-pool usage conditional so single-threaded deployments avoid unnecessary overhead.

## Configuration

Refer to [Reader Implementations](readers-implementation.md) for the shared reader configuration pattern.
