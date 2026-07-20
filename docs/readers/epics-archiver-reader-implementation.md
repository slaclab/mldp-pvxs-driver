# EpicsArchiverReader Implementation

This guide describes the `EpicsArchiverReader` implementation, which provides historical data retrieval and tail polling from EPICS Archiver Appliance.

> **Related:** [Reader Implementations](readers-implementation.md) | [Architecture Overview](../reference/architecture.md)

## Overview

`EpicsArchiverReader` is the history-oriented EPICS reader. It fetches archived data for backfill and can continue tail polling for recent updates.

## Runtime Model

- Supports one-shot historical fetches.
- Supports periodic polling for archiver tailing.
- Configurable `fetch-threads` parallelizes PV fetching across multiple worker threads, each with its own HTTP client. PVs are distributed via a shared work queue (dynamic load balancing).
- Pushes batches to `IDataBus` for downstream processing. Push uses blocking backpressure — worker HTTP clients disable stall detection to avoid false timeouts during backpressure.

## Best Fit

- Data backfill.
- Time-series analysis.
- Replay workflows that need archived source data.

## Implementation Notes

- Uses PB/HTTP streaming for archiver access.
- Configurable timeouts and window settings keep the reader adaptable to different deployments.
- Multi-threaded architecture: each worker owns a `WorkerContext` (HTTP client + per-PV state). Shared `PVWorkQueue` distributes work dynamically.
- In periodic_tail mode, worker 0 coordinates time window computation and queue population; other workers wait on a condition variable before each cycle.
- Shutdown: `running_=false` + cancel all HTTP clients + notify all CVs. Workers exit immediately, dropping buffered samples (no data loss during normal operation — `bus_->push()` blocks until success).

## Configuration

Refer to [Reader Implementations](readers-implementation.md) for the shared reader configuration pattern.
