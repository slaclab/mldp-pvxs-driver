# SLAC BSAS NTTable Structure

## Overview

BSAS (Beam Synchronous Acquisition System) is SLAC's mechanism for synchronizing
acquisitions across multiple process variables to individual beam pulses.  Each
BSAS update delivers a **batch of rows** where every row carries its own
timestamp, representing one beam-pulse acquisition.

The driver represents this as a PVXS `NTTable` (Named-Type Table) with a fixed
two-column layout for per-row timestamps plus one column **per PV name** being
acquired.  The column name is exactly the EPICS PV name (e.g.
`XCOR:LI28:202:BACT`), making the table's column set the live device list for
that acquisition group.

## Structure Layout

A BSAS NTTable has the standard `epics:nt/NTTable:1.0` type with one extra
convention: the per-row timestamp columns live *inside* `value.*` as ordinary
array columns.  They are **not** the NTTable-level `timeStamp` scalar, which
records when the whole update was produced, not when individual rows were
sampled.

```Text
epics:nt/NTTable:1.0
├── labels                               # string[] — column names in value.* order
├── value                                # Struct: one array field per column
│   ├── <PV:NAME:A>      : T[]           # Float64[], Int32[], String[], etc.
│   ├── <PV:NAME:B>      : T[]           # one column per acquired PV
│   │   …
│   ├── secondsPastEpoch : uint32[]      # per-row epoch seconds (type varies)
│   └── nanoseconds      : uint32[]      # per-row nanoseconds  (type varies)
└── timeStamp                            # whole-update metadata (scalar)
    ├── secondsPastEpoch : int64
    └── nanoseconds      : int32
```

- The **PV-name columns** (`<PV:NAME:A>`, …) carry the sampled values — one
  element per beam pulse.  Any number of PVs may appear; the column name is
  exactly the EPICS PV name.
- The **two timestamp columns** (`secondsPastEpoch`, `nanoseconds`) index each
  row in time.  Supported integer types: `UInt32`, `UInt64`, `Int32`, `Int64`.
- All arrays share the same length (one element per row / beam pulse).

## Concrete Example

From [`test/mock/sioc.cpp`](../../test/mock/sioc.cpp), a three-row BSAS table
update looks like this in tabular form, those three rows map to:

PV_NAME_A_DOUBLE_VALUE | PV_NAME_B_STRING_VALUE | secondsPastEpoch | nanoseconds
---------------------- | ---------------------- | ---------------- | -----------
1.0                    | `"OK"`                 | T                | T + 0
2.0                    | `"WARNING"`            | T                | T + 1
3.0                    | `"FAULT"`              | T                | T + 2

## Timestamp Resolution

The driver looks for the per-row timestamp arrays in two locations (first
match wins):

1. **Top-level fields** — `epicsValue[tsSecondsField]` / `epicsValue[tsNanosField]`
2. **Inside `value.*`** — `epicsValue["value"][tsSecondsField]` / `epicsValue["value"][tsNanosField]`

The field names are configurable per PV (see [Configuration](#configuration));
the defaults match the SLAC convention.

## How the Driver Processes a BSAS Update

```Text
Incoming NTTable update
        │
        ▼
┌─────────────────────────────────────────┐
│ 1. Validate: compound value + "value"   │
│    struct must be present               │
└─────────────┬───────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ 2. Locate timestamp arrays              │
│    (top-level → value.* fallback)       │
│    Normalise both arrays to uint64      │
│    Row count = min(seconds, nanos) len  │
└─────────────┬───────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ 3. For each non-timestamp column:       │
│    • Convert typed array to DataFrame   │
│    • Attach per-row TimestampList       │
│    • Emit DataFrame under column name   │
│    • Flush batch when column limit hit  │
└─────────────────────────────────────────┘
```

Each non-timestamp column becomes its own `DataFrame` in `EventBatch.frames`,
with source identity carried by the column name. Timestamp columns are consumed
for row indexing and never forwarded as sources themselves. Every emitted frame
must carry `datatimestamps.timestamplist`; untimestamped frames are dropped.

## Configuration

Live Gen1 tables are read over PVAccess by the dedicated **`epics-pvxs-bsas-gen1`**
reader (the plain `epics-pvxs` reader is scalar-only and rejects
`option.type: slac-bsas-table`):

```yaml
reader:
  - epics-pvxs-bsas-gen1:
      - name: bsas_gen1
        rows-per-data-batch: 120      # rows per DataBatch; 0 = all rows
        pvs-per-data-batch: 1         # PV columns per DataBatch; 0 = all
        data-batches-per-push: 50     # DataBatches per bus EventBatch; 0 = all
        tsSeconds: secondsPastEpoch   # omit to use this default
        tsNanos: nanoseconds          # omit to use this default
        pvs:
          - name: "BSAS:GEN1:TABLE"
            option:                   # optional per-PV overrides
              pvs-per-data-batch: 0
```

Key                     | Level            | Default            | Description
----------------------- | ---------------- | ------------------ | ---------------------------------------------------------
`rows-per-data-batch`   | reader, `option` | `120`              | Rows per `DataBatch` (values per column); `0` = all rows
`pvs-per-data-batch`    | reader, `option` | `1`                | PV columns per `DataBatch` (they share the row timestamps); `0` = all
`data-batches-per-push` | reader, `option` | `50`               | `DataBatch`es per bus `EventBatch` push; `0` = all in one push
`tsSeconds`             | reader, `option` | `secondsPastEpoch` | Name of the per-row epoch-seconds column
`tsNanos`               | reader, `option` | `nanoseconds`      | Name of the per-row nanoseconds column

`option.type` is rejected: the table generation is implied by the reader type.
`column-batch-size` is rejected: use `data-batches-per-push`.
The other `epics-pvxs` reader keys (`thread-pool`, `metadata`, `environment`, ...) are
also accepted.

Example — 1000 PVs × 250 rows with the defaults: each PV is cut into 3 DataBatches
(120 + 120 + 10 rows), giving 3000 DataBatches. They are pushed in time order (rows
0-119 of every PV first) in 60 pushes of 50, then the marker.

### Bus output

Each NTTable update produces the same shape as the HDF5 `hdf5-bsas-gen1` reader:

- one or more tabular `EventBatch`es — `TimeSeriesPayload{root_source_name = <table PV>, is_tabular = true}`,
  metadata = reader/PV `metadata` + `source: <table PV>`; each holds up to
  `data-batches-per-push` `DataBatch`es of up to `rows-per-data-batch` rows ×
  `pvs-per-data-batch` PV columns, ordered by row chunk then column;
- then one `end_of_batch_group` marker (also pushed when the update produced no data).

All-NaN columns are discarded. Updates without timestamp columns produce only the
marker and count as a reader error. A bus push that is rejected is logged and
counted as a reader error; the update is dropped (a PVXS stream cannot be paused).

The `epics-base` reader keeps supporting `option.type: slac-bsas-table`
(with the per-PV `column-batch-size` key), see [EPICS Base Reader](epics-base-reader.md).

## Implementation References

Component                   | File
--------------------------- | -----------------------------------------------------------
PVXS conversion class       | `include/reader/impl/epics/pvxs/BSASEpicsDataBatchConversion.h`
PVXS conversion impl        | `src/reader/impl/epics/pvxs/BSASEpicsDataBatchConversion.cpp`
EPICS Base conversion       | `include/reader/impl/epics/base/EpicsPVDataBatchConversion.h`
PVXS table reader base      | `src/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.cpp`
PVXS Gen1 reader            | `src/reader/impl/epics/pvxs/EpicsPVXSBsasGen1Reader.cpp`
PVXS table reader config    | `src/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.cpp`
EPICS Base reader dispatch  | `src/reader/impl/epics/base/EpicsBaseReader.cpp`
Mock IOC (concrete example) | `test/mock/sioc.cpp`, `test/mock/BsasTablePV.cpp`
Tests                       | `test/reader/impl/epics/pvxs/epics_pvxs_bsas_gen1_reader_test.cpp`
