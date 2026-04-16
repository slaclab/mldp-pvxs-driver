# EpicsBaseReader Implementation

This guide describes the `EpicsBaseReader` implementation, which provides polling-based EPICS Channel Access monitoring for legacy systems.

> **Related:** [Reader Implementations](../readers-implementation.md) | [Architecture Overview](../architecture.md)

## Overview

`EpicsBaseReader` is the polling-oriented EPICS reader. It watches PVs, drains monitor queues, and converts incoming updates into MLDP frames for downstream publishing.

## Runtime Model

- Uses polling rather than event-driven subscriptions.
- Supports multiple polling threads.
- Drains monitor queues under mutex protection.
- Pushes batches to `IDataBus` for controller handoff.

## Best Fit

- Legacy EPICS installations without PVAccess.
- Deployments that need predictable polling intervals.
- Systems that benefit from queue-drain control over subscription callbacks.

## BSAS Support

`EpicsBaseReader` supports SLAC BSAS NTTable mode with per-row timestamps.

See:

- [SLAC BSAS NTTable Gen 1](slac-bsas-table-gen1.md)
- [SLAC BSAS NTTable Gen 2](slac-bsas-table-gen2.md)

## Implementation Notes

- Shares EPICS-specific threading and conversion helpers with `EpicsReaderBase`.
- Keeps logging and metrics aligned with the rest of the driver.

## Configuration

Refer to [Reader Implementations](../readers-implementation.md) for the shared reader configuration pattern.
