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

```
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
update looks like this:

```cpp
// ── Schema (defined once at PV creation) ──────────────────────────────────
nt::NTTable bsasTableBuilder;
bsasTableBuilder.add_column(TypeCode::Float64, "PV_NAME_A_DOUBLE_VALUE");
bsasTableBuilder.add_column(TypeCode::String,  "PV_NAME_B_STRING_VALUE");
bsasTableBuilder.add_column(TypeCode::UInt32,  "secondsPastEpoch");  // timestamp col
bsasTableBuilder.add_column(TypeCode::UInt32,  "nanoseconds");       // timestamp col

// ── Update (3 rows = 3 beam pulses captured in one IOC cycle) ─────────────
pv["labels"] = {"PV_NAME_A_DOUBLE_VALUE", "PV_NAME_B_STRING_VALUE",
                "secondsPastEpoch", "nanoseconds"};

// Measurement data — one value per pulse
pv["value.PV_NAME_A_DOUBLE_VALUE"] = {1.0, 2.0, 3.0};
pv["value.PV_NAME_B_STRING_VALUE"] = {"OK", "WARNING", "FAULT"};

// Per-row timestamps — nanosecond offset distinguishes individual pulses
pv["value.secondsPastEpoch"] = {T,    T,    T   };
pv["value.nanoseconds"]      = {T+0,  T+1,  T+2 };

// NTTable-level timestamp — metadata for the update itself (not per-row)
pv["timeStamp.secondsPastEpoch"] = T;
pv["timeStamp.nanoseconds"]      = T;
```

In tabular form, those three rows map to:

| Row | PV_NAME_A_DOUBLE_VALUE | PV_NAME_B_STRING_VALUE | secondsPastEpoch | nanoseconds |
|-----|------------------------|------------------------|------------------|-------------|
|  0  | 1.0                    | `"OK"`                 | T                | T + 0       |
|  1  | 2.0                    | `"WARNING"`            | T                | T + 1       |
|  2  | 3.0                    | `"FAULT"`              | T                | T + 2       |

## Timestamp Resolution

The driver looks for the per-row timestamp arrays in two locations (first
match wins):

1. **Top-level fields** — `epicsValue[tsSecondsField]` / `epicsValue[tsNanosField]`
2. **Inside `value.*`** — `epicsValue["value"][tsSecondsField]` / `epicsValue["value"][tsNanosField]`

The field names are configurable per PV (see [Configuration](#configuration));
the defaults match the SLAC convention.

## How the Driver Processes a BSAS Update

```
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
│    • Emit EventValue under column name  │
│    • Flush batch when column limit hit  │
└─────────────────────────────────────────┘
```

Each non-timestamp column becomes its **own source** in the `EventBatch`,
keyed by the column name.  The timestamp columns are consumed for row
indexing and are never forwarded as sources themselves.

## Configuration

```yaml
pvs:
  - name: "BSAS:TABLE:PV"
    option:
      type: slac-bsas-table
      tsSeconds: secondsPastEpoch   # omit to use this default
      tsNanos:   nanoseconds        # omit to use this default
```

| Key         | Default            | Description                                    |
|-------------|--------------------|------------------------------------------------|
| `type`      | *(required)*       | Must be `slac-bsas-table` to activate this mode|
| `tsSeconds` | `secondsPastEpoch` | Name of the per-row epoch-seconds column       |
| `tsNanos`   | `nanoseconds`      | Name of the per-row nanoseconds column         |

## Implementation References

| Component                    | File                                                          |
|------------------------------|---------------------------------------------------------------|
| PVXS conversion class        | `include/reader/impl/epics/BSASEpicsMLDPConversion.h`         |
| PVXS conversion impl         | `src/reader/impl/epics/BSASEpicsMLDPConversion.cpp`           |
| EPICS Base conversion        | `include/reader/impl/epics/EpicsPVDataConversion.h`           |
| PVXS reader dispatch         | `src/reader/impl/epics/EpicsPVXSReader.cpp`                   |
| EPICS Base reader dispatch   | `src/reader/impl/epics/EpicsBaseReader.cpp`                   |
| Mock IOC (concrete example)  | `test/mock/sioc.cpp`                                          |
