# Step 09 — Interpolate Alignment Policy

## Goal

Add `Interpolate` alignment to `InputBuffer`. The existing `LatestValue` and `AllUpdated`
policies are unaffected. Pure `InputBuffer` change + new unit tests.

## Depends On

Step 03 (InputBuffer).

---

## Design: Interpolate Alignment

For `Interpolate` alignment, `trySnapshot()` returns synthetic `DataBatch` entries where
each scalar column value is linearly interpolated to the `reference_time`.

**`reference_time` selection**: max timestamp across all `slots_` that have data.

**Per-source interpolation**:
- If the slot has ≥ 2 timestamps in its `DataBatch.timestamps`, interpolate the scalar column(s)
  at `reference_time` using the last two points as the interpolation segment.
- If the slot has exactly 1 timestamp (or `reference_time == slot.timestamp`), use the value as-is.
- If the slot has no data, use a zero-value `DataBatch` (empty columns). Log warning once.

**Only scalar `double` columns are interpolated** (variant index 0 = `vector<double>`).
Other column types are copied as-is (latest value).

**Trigger interaction**: `Interpolate` alignment works with any trigger policy.
`AllUpdated` trigger + `Interpolate` alignment = all sources fresh + interpolated to common time.

---

## Files to Modify

### `src/processor/InputBuffer.cpp`

Add private helper:
```cpp
util::bus::DataBatch interpolateTo(const util::bus::DataBatch& batch,
                                   const util::bus::BusTimestamp& target_time) const;
```

Modify `trySnapshot()`:
- When `alignment_ == AlignmentPolicy::Interpolate`: build `AlignedSnapshot` where each
  channel's `DataBatch` is the result of `interpolateTo(slots_[src], reference_time)`.
- Other alignments: unchanged (copy `slots_[src]` directly).

**`interpolateTo(batch, target)` logic**:
```
if batch.timestamps.size() < 2: return batch (copy as-is)

t0 = batch.timestamps[size-2]
t1 = batch.timestamps[size-1]
if t0 == t1: return batch

alpha = (target - t0) / (t1 - t0)   // clamp alpha to [0,1]

for each DataColumn col in batch.columns:
    if col.values holds vector<double>:
        v0 = col.values[size-2], v1 = col.values[size-1]
        interp = v0 + alpha * (v1 - v0)
        result_col.values = vector<double>{ interp }
    else:
        result_col.values = col.values (copy latest, no interpolation)

result batch: timestamps = {target}, columns = result_cols
```

Timestamp arithmetic: convert `BusTimestamp` to `double` nanoseconds for ratio:
`toNs(t) = t.epoch_seconds * 1'000'000'000ULL + t.nanoseconds`

---

## Test Cases to Add

Append to `test/processor/InputBufferTest.cpp`:

| Test name | Scenario |
|---|---|
| `Interpolate_SinglePoint_ReturnsCopy` | 1 timestamp in slot → value returned as-is |
| `Interpolate_MidpointAlpha` | t0=0, v0=0; t1=2, v1=4; target=1 → result=2.0 |
| `Interpolate_AtT0` | target == t0 → alpha=0, result=v0 |
| `Interpolate_AtT1` | target == t1 → alpha=1, result=v1 |
| `Interpolate_ClampBeyondT1` | target > t1 → alpha clamped to 1.0, result=v1 |
| `Interpolate_NonDoubleColumn_CopiedAsIs` | int32 column → not interpolated, copied |
| `Interpolate_AllUpdated_ReturnsInterpolated` | AllUpdated trigger + Interpolate alignment → returns only when all fresh, values interpolated |

---

## CMake Changes

None.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R InputBuffer -V
```

## Done Criteria

- All 7 new interpolate tests pass.
- All existing InputBuffer tests still pass.
- All other tests pass.
