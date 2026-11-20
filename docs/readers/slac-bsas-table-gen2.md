# SLAC BSAS NTTable Gen 2 Structure

## Overview

BSAS Gen 2 (Beam Synchronous Acquisition System, second generation) extends the Gen 1
format by delivering **per-pulse statistical summaries** instead of raw sample arrays.
Each row corresponds to a single **Pulse ID (PID)** and each acquired PV contributes
multiple statistics columns (value, count, RMS, average, min, max) rather than a single
raw value column.

This format is suited for use cases where the IOC pre-aggregates data over a pulse window
and the downstream consumer needs statistical metadata alongside the measurement.

> **See also:** [BSAS Gen 1](slac-bsas-table-gen1.md) for the raw-sample (per-beam-pulse)
> variant where each column is a single PV value array.

## Structure Layout

A BSAS Gen 2 NTTable follows the standard `epics:nt/NTTable:1.0` type.  The row index
is the **Pulse ID** (`pid` column) and each acquired PV `xxx` expands into six
statistic sub-columns inside `value.*`.

```Text
epics:nt/NTTable:1.0
├── labels                                   # string[] — column names in value.* order
├── value                                    # Struct: one array field per column
│   ├── pid                  : uint64[]      # Pulse ID — one element per row
│   ├── <PV:NAME>.Val        : Float64[]     # Instantaneous / last sample value
│   ├── <PV:NAME>.CNT        : uint32[]      # Sample count in the pulse window
│   ├── <PV:NAME>.RMS        : Float64[]     # Root-mean-square over the window
│   ├── <PV:NAME>.AVG        : Float64[]     # Mean over the window
│   ├── <PV:NAME>.MIN        : Float64[]     # Minimum sample in the window
│   ├── <PV:NAME>.MAX        : Float64[]     # Maximum sample in the window
│   │   … (repeated for every acquired PV)
│   ├── secondsPastEpoch     : uint64[]      # per-row epoch seconds
│   └── nanoseconds          : uint32[]      # per-row nanoseconds
└── timeStamp                                # whole-update metadata (scalar)
    ├── secondsPastEpoch     : int64
    └── nanoseconds          : int32
```

- The **`pid` column** uniquely identifies each row as a beam-pulse ID.  PIDs are
  monotonically increasing integers (e.g. 1999, 2999, …, 1000999).
- Each acquired PV `xxx` is represented by **six sub-columns** sharing the PV base name
  as prefix separated by `.`: `.Val`, `.CNT`, `.RMS`, `.AVG`, `.MIN`, `.MAX`.
- The **two timestamp columns** (`secondsPastEpoch`, `nanoseconds`) provide the wall-clock
  time for each row.
- All arrays share the same length (one element per row / pulse ID).
- The NTTable-level `timeStamp` scalar records when the whole update was produced.

## Concrete Example

A two-row BSAS Gen 2 table update for two acquired PVs (`XCOR:LI28:202:BACT` and
`BPMS:LI28:202:X`) would look like in tabular form:

pid  | XCOR…BACT.Val | XCOR…BACT.CNT | XCOR…BACT.AVG | … | secondsPastEpoch | nanoseconds
---- | ------------- | ------------- | ------------- | - | ---------------- | -----------
1999 | 0.12          | 10            | 0.119         | … | T                | T_ns + 0
2999 | 0.15          | 10            | 0.148         | … | T                | T_ns + 1

## Comparison with Gen 1

Aspect            | Gen 1 (`epics-pvxs-bsas-gen1`)    | Gen 2 (`epics-pvxs-bsas-gen2`)
----------------- | --------------------------------- | ---------------------------------------------
Row index         | Implicit (array position)         | Explicit `pid` column (Pulse ID)
PV representation | One column per PV (raw value)     | Six columns per PV (Val/CNT/RMS/AVG/MIN/MAX)
Timestamp columns | `secondsPastEpoch`, `nanoseconds` | `secondsPastEpoch`, `nanoseconds` (same)
Typical use case  | Raw sample forwarding             | Pre-aggregated statistical summaries

## How the Driver Processes a BSAS Gen 2 Update

Live Gen2 tables are read over PVAccess by the **`epics-pvxs-bsas-gen2`** reader.

> **Phase 1:** the reader currently ingests the same data as Gen1 — the row
> timestamps and each PV's `.Val` column. The pulse ID column and the `CNT`, `RMS`,
> `AVG`, `MIN`, `MAX` statistics are **omitted** until MLDP can store them
> (see `todo/TODO-bsas-gen2-reader.md`).

```Text
Incoming NTTable update
        │
        ▼
┌─────────────────────────────────────────────┐
│ 1. Validate: compound value + "value"       │
│    struct must be present                   │
└─────────────┬───────────────────────────────┘
              ▼
┌─────────────────────────────────────────────┐
│ 2. Locate timestamp arrays                  │
│    (top-level → value.* fallback)           │
└─────────────┬───────────────────────────────┘
              ▼
┌─────────────────────────────────────────────┐
│ 3. Filter columns by label, BEFORE any      │
│    conversion: skip pid + timestamps; keep  │
│    only "<PV>.Val"; output name = "<PV>"    │
└─────────────┬───────────────────────────────┘
              ▼
┌─────────────────────────────────────────────┐
│ 4. Convert kept columns, group into frames, │
│    push tabular EventBatches + marker       │
│    (same shape as Gen1)                     │
└─────────────────────────────────────────────┘
```

Column naming: the filter reads the NTTable `labels` entry for each `value.*` field
(real PV names such as `XCOR:LI28:202:BACT.Val`) when `labels` maps 1:1 onto the
fields; otherwise it falls back to the field name (sanitized, e.g.
`XCOR_LI28_202_BACT_Val`). The suffix is taken after the last `.` (or the last `_`
when the name has no `.`) and compared case-insensitively with `valueSuffix`.
All-NaN `.Val` columns are discarded.

## Configuration

```yaml
reader:
  - epics-pvxs-bsas-gen2:
      - name: bsas_gen2
        pidField: pid                 # default; column always skipped
        valueSuffix: Val              # default; case-insensitive
        rows-per-data-batch: 120      # rows per DataBatch; 0 = all rows
        pvs-per-data-batch: 1         # PV columns per DataBatch; 0 = all
        data-batches-per-push: 50     # DataBatches per bus EventBatch; 0 = all
        tsSeconds: secondsPastEpoch
        tsNanos: nanoseconds
        pvs:
          - name: "BSAS:GEN2:TABLE"
            option:                   # optional per-PV overrides
              valueSuffix: VAL
```

Key                     | Level            | Default            | Description
----------------------- | ---------------- | ------------------ | ---------------------------------------------------------
`rows-per-data-batch`   | reader, `option` | `120`              | Rows per `DataBatch` (values per column); `0` = all rows
`pvs-per-data-batch`    | reader, `option` | `1`                | PV columns per `DataBatch` (they share the row timestamps); `0` = all
`data-batches-per-push` | reader, `option` | `50`               | `DataBatch`es per bus `EventBatch` push; `0` = all in one push
`tsSeconds`             | reader, `option` | `secondsPastEpoch` | Name of the per-row epoch-seconds column
`tsNanos`               | reader, `option` | `nanoseconds`      | Name of the per-row nanoseconds column
`pidField`              | reader, `option` | `pid`              | Pulse ID column name (always skipped)
`valueSuffix`           | reader, `option` | `Val`              | Suffix of the value column, case-insensitive

`option.type` is rejected: the table generation is implied by the reader type.
`column-batch-size` is rejected: use `data-batches-per-push`.

## Implementation References

Component                   | File
--------------------------- | -----------------------------------------------------------
PVXS Gen2 reader + filter   | `src/reader/impl/epics/pvxs/EpicsPVXSBsasGen2Reader.cpp`
PVXS table reader base      | `src/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.cpp`
PVXS table reader config    | `src/reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.cpp`
PVXS conversion (filter)    | `src/reader/impl/epics/pvxs/BSASEpicsDataBatchConversion.cpp`
Mock IOC (concrete example) | `test/mock/sioc.cpp` (`test:bsas_gen2_*`), `test/mock/BsasTablePV.cpp`
Tests                       | `test/reader/impl/epics/pvxs/epics_pvxs_bsas_gen2_reader_test.cpp`
EPICS Base support          | not implemented
