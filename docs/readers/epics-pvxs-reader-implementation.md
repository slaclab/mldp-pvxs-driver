# EpicsPVXSReader Implementation

This guide describes the `EpicsPVXSReader` implementation, which provides event-driven EPICS PVAccess monitoring.

> **Related:** [Reader Implementations](../readers-implementation.md) | [Architecture Overview](../architecture.md)

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

`EpicsPVXSReader` includes SLAC BSAS NTTable support.

See:

- [SLAC BSAS NTTable Gen 1](slac-bsas-table-gen1.md)
- [SLAC BSAS NTTable Gen 2](slac-bsas-table-gen2.md)

## Implementation Notes

- Shares common EPICS conversion helpers with `EpicsReaderBase`.
- Makes thread-pool usage conditional so single-threaded deployments avoid unnecessary overhead.

## Configuration

Refer to [Reader Implementations](../readers-implementation.md) for the shared reader configuration pattern.
