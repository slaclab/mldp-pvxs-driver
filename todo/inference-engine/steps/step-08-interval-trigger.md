# Step 08 — Interval Trigger Worker Thread

## Goal

Add the `interval` trigger: a background thread in `ChannelProcessor` that wakes on a
configurable wall-clock period and calls `fireCompute()` independently of `push()`.

## Depends On

Steps 01–07 (primarily Step 05 — `ChannelProcessor`).

---

## Files to Modify

### `include/processor/ChannelProcessor.h`

Add members:
```cpp
#include <condition_variable>
#include <mutex>
#include <thread>
// ...
private:
    // Existing members ...
    std::condition_variable worker_cv_;
    std::mutex              worker_mutex_;
    std::thread             worker_thread_;
```

Add private method:
```cpp
void runIntervalWorker();
```

### `src/processor/ChannelProcessor.cpp`

**`start()`** — add after `running_ = true`:
```cpp
if (config_.trigger() == TriggerPolicy::Interval) {
    worker_thread_ = std::thread(&ChannelProcessor::runIntervalWorker, this);
}
```

**`stop()`** — add before `buffer_.clear()`:
```cpp
{
    std::lock_guard<std::mutex> lock(worker_mutex_);
    running_.store(false, std::memory_order_relaxed);
}
worker_cv_.notify_all();
if (worker_thread_.joinable()) {
    worker_thread_.join();
}
```

**`push()` with interval trigger** — ingest but do NOT call `trySnapshot` or `fireCompute`:
```cpp
if (config_.trigger() == TriggerPolicy::Interval) {
    // just ingest; worker thread handles compute
    return true;
}
```

**`runIntervalWorker()`**:
```cpp
void ChannelProcessor::runIntervalWorker() {
    using namespace std::chrono;
    const auto period = duration<double>(config_.triggerIntervalSec());
    std::unique_lock<std::mutex> lock(worker_mutex_);
    while (running_.load(std::memory_order_relaxed)) {
        worker_cv_.wait_for(lock, period);
        if (!running_.load(std::memory_order_relaxed)) break;
        if (auto snap = buffer_.trySnapshot(TriggerPolicy::Interval)) {
            fireCompute(*snap);
        }
    }
}
```

Note: `trySnapshot(Interval)` always returns a snapshot (Step 03 spec). Worker fires on schedule
even if no data has arrived (empty snapshot → algorithm handles gracefully or returns empty output).

---

## Test File

### `test/processor/ChannelProcessorIntervalTest.cpp`

| Test name | Scenario |
|---|---|
| `IntervalTrigger_FiresWithoutPush` | start processor, wait 1.5× interval, verify compute called ≥1 |
| `IntervalTrigger_PushDoesNotTriggerCompute` | push batch, verify compute NOT called synchronously |
| `IntervalTrigger_UsesLatestValueAfterPush` | push value, wait for interval fire → snapshot has that value |
| `IntervalTrigger_StopsCleanly` | start, wait for ≥1 fire, stop → no deadlock, thread joins |
| `IntervalTrigger_ShortInterval` | interval=0.05s, run 0.5s → ≥8 fires |

Use a stub algorithm with atomic counter for `call_count`.
Use short intervals (50–100ms) to keep tests fast.

> **Important**: These are timing-dependent tests. Use relaxed assertions (`>= expected`, not `== exact`).

Add test cpp to CMakeLists main test target.

---

## CMake Changes

None — modifying existing files only.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R ChannelProcessorInterval -V --timeout 30
```

## Done Criteria

- All 5 interval tests pass reliably.
- `AnyUpdate` and `AllUpdated` tests from Step 05 still pass (no regression).
- All existing tests pass.
