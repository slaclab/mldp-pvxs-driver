# Step 03 — InputBuffer

## Goal

Implement the per-source slot storage that `ChannelProcessor` will use.
`AnyUpdate` and `AllUpdated` trigger logic only — `Interpolate` deferred to Step 09.

## Depends On

Step 01 (AlignmentPolicy, TriggerPolicy, AlignedSnapshot).

---

## Files to Create

### `include/processor/InputBuffer.h`

```cpp
#pragma once
#include <processor/AlignedSnapshot.h>
#include <processor/AlignmentPolicy.h>
#include <processor/TriggerPolicy.h>
#include <util/bus/IDataBus.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mldp_pvxs_driver::processor {

class InputBuffer {
public:
    explicit InputBuffer(const std::vector<std::string>& source_names,
                         AlignmentPolicy                 policy);

    // Store latest DataBatch for source; mark fresh.
    void ingest(const std::string& root_source_name,
                const util::bus::TimeSeriesPayload& payload);

    // Returns snapshot if trigger condition met, else nullopt.
    // AnyUpdate : always returns after any ingest (latest-value per slot).
    // AllUpdated: returns only when every required source is fresh.
    // Interval  : always returns current state (used by worker thread, not push()).
    std::optional<AlignedSnapshot> trySnapshot(TriggerPolicy trigger);

    void resetFreshFlags();
    void clear();

private:
    std::unordered_map<std::string, util::bus::DataBatch> slots_;
    std::unordered_set<std::string>                       fresh_;
    std::vector<std::string>                              required_sources_;
    AlignmentPolicy                                       alignment_;
};

} // namespace
```

### `src/processor/InputBuffer.cpp`

Key implementation notes:

**`ingest(source, payload)`**:
- Ignore source not in `required_sources_` (no-op — route table already filters, but be defensive).
- `payload.frames` may be empty. If empty, skip slot update.
- For `LatestValue` and `AllUpdated` alignment: store `payload.frames.back()` (last DataBatch) in `slots_[source]`.
- Mark `fresh_.insert(source)`.
- `reference_time` from `payload.frames.back().timestamps.back()` if non-empty, else zero `BusTimestamp`.

**`trySnapshot(trigger)`**:
- `AnyUpdate`: always build + return snapshot from current `slots_` (no freshness check needed — caller just ingested).
- `AllUpdated`: return snapshot only when `fresh_.size() == required_sources_.size()`. Else return `nullopt`.
- `Interval`: return snapshot unconditionally (current latest per slot, even if stale).
- Snapshot `reference_time` = max timestamp across all `slots_` entries (use `epoch_seconds * 1e9 + nanoseconds` to compare).
- After returning snapshot from `AllUpdated`, do NOT auto-reset flags here — caller calls `resetFreshFlags()`.

**`resetFreshFlags()`**: `fresh_.clear()`.

**`clear()`**: `slots_.clear(); fresh_.clear()`.

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/InputBuffer.cpp
```

---

## Test File

### `test/processor/InputBufferTest.cpp`

| Test name | Scenario |
|---|---|
| `AnyUpdate_ReturnsAfterFirstIngest` | single source, AnyUpdate → snapshot returned after 1 ingest |
| `AnyUpdate_SnapshotContainsLatestValue` | ingest twice, AnyUpdate → snapshot has second value |
| `AllUpdated_NoSnapshotUntilAllFresh` | 2 sources, ingest only 1 → nullopt |
| `AllUpdated_SnapshotAfterBothFresh` | 2 sources, ingest both → snapshot returned |
| `AllUpdated_ResetThenNoSnapshot` | after snapshot + resetFreshFlags → next single ingest = nullopt |
| `Interval_AlwaysReturnsSnapshot` | before any ingest (slots empty) → snapshot returned with empty channels |
| `Interval_SnapshotContainsLatest` | ingest → interval snapshot has that value |
| `IgnoresUnknownSource` | ingest source not in required_sources → no crash, slot unchanged |
| `EmptyPayloadFrames` | ingest with empty frames → slot unchanged, fresh not set |
| `ReferenceTimeIsMax` | 2 sources with different timestamps → reference_time = later one |

Add test cpp to CMakeLists main test target.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R InputBuffer -V
```

## Done Criteria

- All 10 InputBuffer tests pass.
- No existing tests broken.
