# EpicsDSMetadataReader (EPICS Directory Service Metadata)

The `EpicsDSMetadataReader` fetches PV metadata from an EPICS Directory Service (DS)
via a PVA RPC call and publishes the results as `SourceMetadataPayload` onto the driver
bus. Downstream, an `mldp-pv-metadata` writer persists that payload to the MLDP
annotation service.

**Registration Type:** `"epics-ds-metadata"`

**Status:** Implemented

File           | Location
-------------- | ---------------------------------------------------------------
Header         | `include/reader/impl/epics_ds/EpicsDSMetadataReader.h`
Implementation | `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp`
Config         | `include/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h`

## Build Option & Required Libraries

- **Build option:** none (always built)
- **Required libraries/components:**
  - PVXS (`libpvxs`) for the PVA client and RPC support
  - EPICS Base core libraries

## Architecture

```mermaid
flowchart TB
    subgraph EpicsDSMetadataReader["EpicsDSMetadataReader"]
        subgraph Worker["Background Worker Thread"]
            RPC["NTURI RPC call\n(pvxs::client::Context)"]
            Parse["NTTable parser\n(source-name-column, tags-column)"]
        end

        Worker --> Push["Push SourceMetadataPayload\nto IDataBus"]
        Push --> Bus["IDataBus"]

        Push --> Wait["Sleep rescan-interval-sec\n(interruptible)"]
        Wait -->|loop| RPC
    end

    subgraph DS["EPICS Directory Service"]
        PVA["PVA RPC endpoint\n(service name)"]
    end

    RPC -->|NTURI query| PVA
    PVA -->|NTTable response| Parse
```

## Operating Modes

### One-Shot (Default)

When `rescan-interval-sec` is `0.0` (the default), the reader:

1. Constructs an NTURI request with the configured `query` pattern.
2. Issues an RPC call to the PVA service named by `service`.
3. Parses the NTTable response — extracts source names from `source-name-column` and
   optionally comma-separated tags from `tags-column`.
4. Pushes the resulting `SourceMetadataPayload` to the bus once.
5. Worker thread exits; reader stays alive but idle.

```yaml
reader:
  - epics-ds-metadata:
      - name: ds_metadata_once
        service: ds
        query: "%"
        timeout-sec: 5.0
        source-name-column: channelName
        tags-column: tags
```

### Periodic Rescan

When `rescan-interval-sec` is greater than `0`, the worker repeats the fetch on that
interval until the reader is destroyed. The sleep between iterations is interruptible via
a condition variable so shutdown is prompt.

```yaml
reader:
  - epics-ds-metadata:
      - name: ds_metadata_rescan
        service: ds
        query: "%"
        timeout-sec: 5.0
        source-name-column: channelName
        tags-column: tags
        rescan-interval-sec: 300.0   # re-fetch every 5 minutes
```

## Configuration

### Required Parameters

Parameter | Type   | Description
--------- | ------ | -----------------------------------------------------------
`name`    | string | **Required.** Unique reader instance name (non-empty).

### Optional Parameters

Parameter              | Type   | Default         | Description
---------------------- | ------ | --------------- | ---------------------------------------------------
`service`              | string | `"ds"`          | PVA service name to call via RPC.
`query`                | string | `"%"`           | Query pattern sent in the NTURI `query.name` field.
`timeout-sec`          | double | `5.0`           | RPC call timeout in seconds. Must be positive.
`source-name-column`   | string | `"channelName"` | NTTable column that carries the PV / source name.
`tags-column`          | string | `""`            | NTTable column for comma-separated tags. Empty = disabled.
`rescan-interval-sec`  | double | `0.0`           | Repeat fetch interval in seconds. `0` = run once.

**Validation rules:**

- `name` is required and must be non-empty.
- `timeout-sec` must be strictly positive.
- `rescan-interval-sec` must be `>= 0`.

## Key Features

- **RPC-based fetch**: Uses `pvxs::client::Context` to issue a synchronous NTURI call
  against any PVA Directory Service endpoint.
- **NTTable parsing**: Extracts source names and optional tags from the configured column
  names; other NTTable columns are ignored.
- **Periodic rescan**: Optional background loop with an interruptible sleep; no busy-wait.
- **Graceful shutdown**: Condition variable wakeup on destruction; worker thread joins
  before the destructor returns.
- **No PV list required**: The query pattern (`%` by default) returns whatever the DS
  exposes — no per-PV configuration needed.

## Typical Use: PV Metadata Pipeline

The most common deployment pairs this reader with an `mldp-pv-metadata` writer:

```yaml
writer:
  mldp-pv-metadata:
    - name: pv_metadata_writer
      thread-pool: 2
      deadline-seconds: 10
      mldp-pv-metadata-pool:
        annotation-url: grpc://annotation-host:50053

reader:
  - epics-ds-metadata:
      - name: ds_metadata
        service: ds
        query: "%"
        timeout-sec: 5.0
        source-name-column: channelName
        tags-column: tags
        rescan-interval-sec: 300.0

routing:
  pv_metadata_writer:
    from: [ds_metadata]
```

**What happens:**
1. `EpicsDSMetadataReader` calls the DS RPC and receives an NTTable.
2. Each row becomes a `SourceMetadataEntry`; all rows are bundled into a
   `SourceMetadataPayload` and pushed to the bus.
3. `MLDPPVMetadataWriter` fans the payload into individual `savePvMetadata` RPCs.

## Metrics

This reader does not use the standard `EpicsReaderBase` thread pool and therefore does
not expose per-event metrics. Pipeline health can be observed via the writer-side metrics
on the paired `mldp-pv-metadata` writer instance.

## See Also

- [MLDP PV Metadata Writer](../writers/mldp-pv-metadata-writer.md) — consumes `SourceMetadataPayload`
- [Readers Overview](readers.md)
- [Configuration Reference](../guides/configuration.md#epics-ds-metadata-reader)
