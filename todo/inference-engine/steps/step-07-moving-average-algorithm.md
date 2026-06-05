# Step 07 — MovingAverageAlgorithm

## Goal

Second C++ algorithm. Validates stateful algorithm pattern (sliding window).
No infrastructure changes needed.

## Depends On

Steps 01–06.

---

## Files to Create

### `include/processor/impl/MovingAverageAlgorithm.h`

```cpp
#pragma once
#include <processor/IAlgorithm.h>
#include <deque>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class MovingAverageAlgorithm final : public IAlgorithm {
public:
    void configure(const config::Config& cfg) override;
    // Reads from cfg:
    //   output-source: string (required)
    //   window-size:   int    (optional, default 10, must be >= 1)
    //   output-column: string (optional, default "avg")
    //   source:        string (optional — single source name to average.
    //                          If absent, uses first key in snapshot.channels.)

    std::vector<std::string>     outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType()  const noexcept override { return "moving-average"; }

private:
    std::string        output_source_{"VIRTUAL:MA:OUT"};
    std::deque<double> window_;
    std::size_t        window_size_{10};
    std::string        output_column_{"avg"};
    std::string        source_name_;  // empty = auto-pick first channel
};

} // namespace
```

### `src/processor/impl/MovingAverageAlgorithm.cpp`

**`configure(cfg)`**:
- Read `output-source` — required.
- Read `window-size` — optional int ≥ 1. Default 10. Throw if < 1.
- Read `output-column` — optional string, default `"avg"`.
- Read `source` — optional string. Stored in `source_name_`.

**`compute(snapshot)`**:
- Pick channel: `source_name_` if non-empty and present in `snapshot.channels`, else first entry.
- Extract scalar: `DataBatch.columns[0].values` as `vector<double>`, take `.back()`.
  Fall through to 0.0 if missing/wrong type.
- `window_.push_back(value)`. If `window_.size() > window_size_`, `window_.pop_front()`.
- `avg = accumulate(window_) / window_.size()`.
- Return `{ AlgorithmOutput{ output_source_, TimeSeriesPayload{
    .root_source_name = output_source_,
    .frames = { DataBatch{ {snapshot.reference_time}, { DataColumn{ output_column_, vector<double>{avg} } } } },
    .is_tabular = true,
    .end_of_batch_group = true
  } } }`.

**Note on `stop()`**: `window_` cleared in `ChannelProcessor::stop()` via `buffer_.clear()`.
The window itself is in the algorithm — the algorithm's window must be cleared on `start()`/`stop()`.
`IAlgorithm` has no `start()`/`stop()`. Add `reset()` method to `IAlgorithm` (optional, default no-op),
or simply clear in `configure()` (always called before start). For simplicity: `ChannelProcessor::stop()`
calls `algorithm_->reset()` — add `virtual void reset() noexcept {}` to `IAlgorithm`.
`MovingAverageAlgorithm::reset()` clears `window_`.

Place `REGISTER_ALGORITHM("moving-average", MovingAverageAlgorithm)` in `.cpp`.

---

## Files to Modify

### `include/processor/IAlgorithm.h`

Add:
```cpp
// Called by ChannelProcessor::stop() — reset internal state for a clean restart.
// Default: no-op. Stateful algorithms (e.g. moving-average) override.
virtual void reset() noexcept {}
```

### `src/processor/ChannelProcessor.cpp`

In `stop()`: add `algorithm_->reset();` after `running_ = false`.

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/impl/MovingAverageAlgorithm.cpp
```

---

## Test File

### `test/processor/MovingAverageAlgorithmTest.cpp`

| Test name | Scenario |
|---|---|
| `SingleValueWindow1` | window-size=1, push value → avg = that value |
| `AccumulatesCorrectly` | window-size=3, push 1,2,3 → avg = 2.0 |
| `SlidesWindow` | window-size=2, push 1,2,3 → avg = (2+3)/2 = 2.5 |
| `WindowNotExceeded` | push 20 values, window-size=5 → deque.size() never > 5 |
| `MissingOutputSourceThrows` | no output-source → configure() throws |
| `WindowSizeZeroThrows` | window-size=0 → configure() throws |
| `ResetClearsWindow` | push values, call reset(), push 1 value → avg = that value only |
| `OutputSourceSetCorrectly` | root_source_name in output = configured output-source |

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R MovingAverage -V
```

## Done Criteria

- All 8 MovingAverageAlgorithmTest cases pass.
- `IAlgorithm::reset()` default no-op does not break LinearTransform tests.
- All existing tests pass.
