# EpicsArchiverReader Implementation

This guide describes the `EpicsArchiverReader` implementation, which provides historical data retrieval and tail polling from EPICS Archiver Appliance.

> **Related:** [Reader Implementations](../readers-implementation.md) | [Architecture Overview](../architecture.md)

## Overview

`EpicsArchiverReader` is the history-oriented EPICS reader. It fetches archived data for backfill and can continue tail polling for recent updates.

## Runtime Model

- Supports one-shot historical fetches.
- Supports periodic polling for archiver tailing.
- Pushes batches to `IDataBus` for downstream processing.

## Best Fit

- Data backfill.
- Time-series analysis.
- Replay workflows that need archived source data.

## Implementation Notes

- Uses PB/HTTP streaming for archiver access.
- Configurable timeouts and window settings keep the reader adaptable to different deployments.

## Configuration

Refer to [Reader Implementations](../readers-implementation.md) for the shared reader configuration pattern.
