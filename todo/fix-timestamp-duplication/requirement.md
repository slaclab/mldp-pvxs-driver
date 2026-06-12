# Fix Timestamp Duplication in gRPC Payload (P1)

## Priority

P1 — 1.407× payload inflation, conversion time scales linearly with column count

## Problem

`BSASEpicsMLDPConversion::convertColumn()` calls `fillTimestamps()` for every
non-timestamp column. Each column's `DataBatch` carries a full copy of the
row timestamp list (304 K rows × 16 bytes/row = ~4.88 MB per column).

The source NTTable stores timestamps once for all columns. After conversion, N
data columns carry N independent copies of those timestamps inside their gRPC
frames. Measured expansion ratio: **1.407×** (242.3 MB payload out / 172.2 MB
reader in) over a 75 s session.

Effect on conversion time: `fillTimestamps` iterates all rows per column.
At 132 ms/event with ~304 K rows and estimated 2–4 columns, timestamp copying
accounts for the majority of the 132 ms budget. Adding more columns will
extend conversion time and payload size linearly.

## Root cause location

`src/reader/impl/epics/pvxs/BSASEpicsMLDPConversion.cpp`

`fillTimestamps()` — called once per column inside `convertColumn()`:

```cpp
void fillTimestamps(DataBatch& batch, size_t n,
                    const std::vector<uint64_t>& tsSeconds,
                    const std::vector<uint64_t>& tsNanos)
{
    batch.timestamps.reserve(n);
    for (size_t i = 0; i < n; ++i)
        batch.timestamps.push_back(TimestampEntry{tsSeconds[i], tsNanos[i]});
}
```

## Fix direction

Options (pick one):

### Option A — Shared timestamp vector (preferred)
Build `tsSeconds` / `tsNanos` once. Emit a single `DataBatch` with all
columns packed into it (one `DataBatch` per NTTable event, not per column).
All columns share one timestamp list.

Requires downstream consumers to handle multi-column `DataBatch` — audit
`MLDPWriter::buildIngestDataRequest` and the `DataBatch` schema to confirm
it already supports multiple columns per batch.

### Option B — Shared `std::shared_ptr<std::vector<TimestampEntry>>`
Keep one `DataBatch` per column but replace `std::vector<TimestampEntry>`
in `DataBatch` with a `shared_ptr`. All column batches from the same NTTable
event point to the same timestamp vector. No copy.

Requires changing `DataBatch` definition in the shared bus header.

### Option C — Emit timestamps only in the first column batch
Writer reconstructs timestamps from the first batch; subsequent batches
carry an index reference. More complex, may require protocol change.

**Recommendation: Option A** if `DataBatch` already supports multiple columns;
Option B if changing `DataBatch` schema is constrained by other consumers.

## Acceptance criteria

- Payload expansion ratio falls to ≤ 1.05× (timestamps sent once, not N times).
- Conversion time per event decreases proportionally to column count reduction.
- All existing BSAS NTTable conversion tests pass.
- `metrics.jsonl` `writer_payload_bytes_total / reader_data_bytes_total` ≤ 1.05 in a regression run against `BSAS:SYS0:1:CUHXR_TBL`.
