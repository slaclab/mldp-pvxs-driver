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

```
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
`BPMS:LI28:202:X`) would look like:

```cpp
// ── Schema ─────────────────────────────────────────────────────────────────
nt::NTTable bsasGen2Builder;
bsasGen2Builder.add_column(TypeCode::UInt64,  "pid");
// PV A statistics
bsasGen2Builder.add_column(TypeCode::Float64, "XCOR:LI28:202:BACT.Val");
bsasGen2Builder.add_column(TypeCode::UInt32,  "XCOR:LI28:202:BACT.CNT");
bsasGen2Builder.add_column(TypeCode::Float64, "XCOR:LI28:202:BACT.RMS");
bsasGen2Builder.add_column(TypeCode::Float64, "XCOR:LI28:202:BACT.AVG");
bsasGen2Builder.add_column(TypeCode::Float64, "XCOR:LI28:202:BACT.MIN");
bsasGen2Builder.add_column(TypeCode::Float64, "XCOR:LI28:202:BACT.MAX");
// PV B statistics
bsasGen2Builder.add_column(TypeCode::Float64, "BPMS:LI28:202:X.Val");
bsasGen2Builder.add_column(TypeCode::UInt32,  "BPMS:LI28:202:X.CNT");
bsasGen2Builder.add_column(TypeCode::Float64, "BPMS:LI28:202:X.RMS");
bsasGen2Builder.add_column(TypeCode::Float64, "BPMS:LI28:202:X.AVG");
bsasGen2Builder.add_column(TypeCode::Float64, "BPMS:LI28:202:X.MIN");
bsasGen2Builder.add_column(TypeCode::Float64, "BPMS:LI28:202:X.MAX");
// Timestamps
bsasGen2Builder.add_column(TypeCode::UInt64,  "secondsPastEpoch");
bsasGen2Builder.add_column(TypeCode::UInt32,  "nanoseconds");

// ── Update (2 rows = 2 pulse IDs) ──────────────────────────────────────────
pv["value.pid"]                      = {1999UL,  2999UL};

pv["value.XCOR:LI28:202:BACT.Val"]  = {0.12,    0.15};
pv["value.XCOR:LI28:202:BACT.CNT"]  = {10U,     10U};
pv["value.XCOR:LI28:202:BACT.RMS"]  = {0.121,   0.151};
pv["value.XCOR:LI28:202:BACT.AVG"]  = {0.119,   0.148};
pv["value.XCOR:LI28:202:BACT.MIN"]  = {0.10,    0.13};
pv["value.XCOR:LI28:202:BACT.MAX"]  = {0.14,    0.17};

pv["value.BPMS:LI28:202:X.Val"]     = {-0.5,    -0.4};
pv["value.BPMS:LI28:202:X.CNT"]     = {10U,     10U};
pv["value.BPMS:LI28:202:X.RMS"]     = {0.51,    0.41};
pv["value.BPMS:LI28:202:X.AVG"]     = {-0.49,   -0.39};
pv["value.BPMS:LI28:202:X.MIN"]     = {-0.6,    -0.5};
pv["value.BPMS:LI28:202:X.MAX"]     = {-0.4,    -0.3};

pv["value.secondsPastEpoch"]         = {T,       T};
pv["value.nanoseconds"]              = {T_ns+0,  T_ns+1};

pv["timeStamp.secondsPastEpoch"]     = T;
pv["timeStamp.nanoseconds"]          = T_ns;
```

In tabular form:

| Row | pid    | XCOR…BACT.Val | XCOR…BACT.CNT | XCOR…BACT.AVG | … | secondsPastEpoch | nanoseconds |
|-----|--------|---------------|---------------|---------------|---|------------------|-------------|
|  0  | 1999   | 0.12          | 10            | 0.119         | … | T                | T_ns + 0    |
|  1  | 2999   | 0.15          | 10            | 0.148         | … | T                | T_ns + 1    |

## Comparison with Gen 1

| Aspect              | Gen 1 (slac-bsas-table)          | Gen 2 (slac-bsas-table-gen2)                  |
|---------------------|----------------------------------|-----------------------------------------------|
| Row index           | Implicit (array position)        | Explicit `pid` column (Pulse ID)              |
| PV representation   | One column per PV (raw value)    | Six columns per PV (Val/CNT/RMS/AVG/MIN/MAX)  |
| Timestamp columns   | `secondsPastEpoch`, `nanoseconds`| `secondsPastEpoch`, `nanoseconds` (same)      |
| Typical use case    | Raw sample forwarding            | Pre-aggregated statistical summaries          |

## How the Driver Processes a BSAS Gen 2 Update

```
Incoming NTTable update
        │
        ▼
┌─────────────────────────────────────────────┐
│ 1. Validate: compound value + "value"       │
│    struct must be present                   │
└─────────────┬───────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────┐
│ 2. Locate pid, timestamp arrays             │
│    (top-level → value.* fallback)           │
│    Normalise timestamp arrays to uint64     │
│    Row count = pid array length             │
└─────────────┬───────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────┐
│ 3. Group columns by PV base name            │
│    (strip .Val/.CNT/.RMS/.AVG/.MIN/.MAX)    │
└─────────────┬───────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────┐
│ 4. For each PV group:                       │
│    • Convert all six stat arrays to         │
│      DataFrame columns                      │
│    • Attach per-row TimestampList           │
│    • Emit EventValue keyed by PV base name  │
│    • Flush batch when column limit hit      │
└─────────────────────────────────────────────┘
```

Each PV base name becomes its **own source** in the `EventBatch`, carrying all six
statistic columns as a structured value.  The `pid` and timestamp columns are consumed
for row indexing and are not forwarded as sources.

## Configuration

```yaml
pvs:
  - name: "BSAS:GEN2:TABLE:PV"
    option:
      type: slac-bsas-table-gen2
      pidField:  pid                  # omit to use this default
      tsSeconds: secondsPastEpoch     # omit to use this default
      tsNanos:   nanoseconds          # omit to use this default
```

| Key         | Default            | Description                                         |
|-------------|--------------------|-----------------------------------------------------|
| `type`      | *(required)*       | Must be `slac-bsas-table-gen2` to activate this mode|
| `pidField`  | `pid`              | Name of the Pulse ID column                         |
| `tsSeconds` | `secondsPastEpoch` | Name of the per-row epoch-seconds column            |
| `tsNanos`   | `nanoseconds`      | Name of the per-row nanoseconds column              |

## Implementation References

> **Note:** This feature is not yet implemented.  The table below lists the planned
> file locations that mirror the Gen 1 layout.

| Component                    | File (planned)                                                     |
|------------------------------|--------------------------------------------------------------------|
| PVXS conversion class        | `include/reader/impl/epics/BSASGen2EpicsMLDPConversion.h`          |
| PVXS conversion impl         | `src/reader/impl/epics/BSASGen2EpicsMLDPConversion.cpp`            |
| EPICS Base conversion        | `include/reader/impl/epics/base/EpicsPVDataConversion.h`                |
| PVXS reader dispatch         | `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp`                        |
| EPICS Base reader dispatch   | `src/reader/impl/epics/base/EpicsBaseReader.cpp`                        |
| Mock IOC (concrete example)  | `test/mock/sioc.cpp`                                               |
