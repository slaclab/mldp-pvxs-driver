# Step 05 — ChannelProcessor (base infrastructure)

## Goal

Implement the full `ChannelProcessor` class: `push`, `fireCompute`, `start`/`stop`,
`AnyUpdate` and `AllUpdated` trigger paths.
No `Interval` worker thread yet (Step 08).
No algorithm implementations yet — tested with a minimal stub.

## Depends On

Steps 01–04.

---

## Files to Create

### `include/processor/ChannelProcessor.h`

```cpp
#pragma once
#include <processor/IChannelProcessor.h>
#include <processor/MLDPChannelProcessorConfig.h>
#include <processor/IAlgorithm.h>
#include <processor/InputBuffer.h>
#include <metrics/Metrics.h>
#include <util/bus/IDataBus.h>
#include <atomic>
#include <memory>

namespace mldp_pvxs_driver::processor {

class ChannelProcessor final : public IChannelProcessor {
public:
    ChannelProcessor(MLDPChannelProcessorConfig           config,
                     IAlgorithmUPtr                       algorithm,
                     std::shared_ptr<util::bus::IDataBus> bus,
                     std::shared_ptr<metrics::Metrics>    metrics);
    ~ChannelProcessor() override;

    // IWriter
    std::string name()   const override;
    void        start()  override;
    void        stop()   noexcept override;
    bool        push(util::bus::IDataBus::EventBatch batch) noexcept override;
    bool        acceptsPayload(const util::bus::BatchPayload&) const noexcept override;
    bool        supports_multi_root_source() const noexcept override { return true; }

    // IChannelProcessor
    const std::string&              outputReaderName()  const noexcept override;
    std::vector<std::string>        outputSourceNames() const noexcept override;
    const std::vector<std::string>& inputSourceNames()  const noexcept override;

private:
    void fireCompute(const AlignedSnapshot& snap);

    MLDPChannelProcessorConfig           config_;
    IAlgorithmUPtr                       algorithm_;
    std::shared_ptr<util::bus::IDataBus> bus_;
    std::shared_ptr<metrics::Metrics>    metrics_;
    InputBuffer                          buffer_;
    std::atomic<bool>                    running_{false};
};

} // namespace
```

### `src/processor/ChannelProcessor.cpp`

**`acceptsPayload()`**: `return std::holds_alternative<util::bus::TimeSeriesPayload>(p);`

**`push(batch)`**:
```cpp
bool ChannelProcessor::push(util::bus::IDataBus::EventBatch batch) noexcept {
    if (!running_.load(std::memory_order_relaxed)) return false;
    const auto* ts = std::get_if<util::bus::TimeSeriesPayload>(&batch.payload);
    if (!ts) return true;  // non-TS payload: accept silently, nothing to compute
    buffer_.ingest(ts->root_source_name, *ts);
    if (config_.trigger() != TriggerPolicy::Interval) {
        if (auto snap = buffer_.trySnapshot(config_.trigger())) {
            buffer_.resetFreshFlags();
            fireCompute(*snap);
        }
    }
    return true;
}
```

**`fireCompute(snap)`**:
```cpp
void ChannelProcessor::fireCompute(const AlignedSnapshot& snap) {
    std::vector<AlgorithmOutput> outputs;
    try {
        outputs = algorithm_->compute(snap);
    } catch (const std::exception& e) {
        // log warning, return
        return;
    }
    for (auto& ao : outputs) {
        util::bus::IDataBus::EventBatch out;
        out.reader_name = config_.name();
        // Set identity inside payload — variant holds the full payload from algorithm
        out.payload = std::move(ao.payload);
        bus_->push(std::move(out));
    }
}
```

**`start()`**: set `running_ = true`. Log info. (Interval worker: Step 08.)

**`stop()`**: set `running_ = false`. `buffer_.clear()`. Log info. (Worker join: Step 08.)

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/ChannelProcessor.cpp
```

---

## Test File

### `test/processor/ChannelProcessorTest.cpp`

Use a minimal stub algorithm (defined in the test file, not a registered type):

```cpp
class StubAlgorithm : public mldp_pvxs_driver::processor::IAlgorithm {
public:
    void configure(const config::Config&) override {}
    std::vector<std::string> outputSources() const noexcept override { return {"VIRTUAL:STUB"}; }
    std::string algorithmType() const noexcept override { return "stub"; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snap) override {
        last_snap = snap;
        call_count++;
        util::bus::TimeSeriesPayload ts;
        ts.root_source_name = "VIRTUAL:STUB";
        ts.end_of_batch_group = true;
        return {{ "VIRTUAL:STUB", ts }};
    }
    AlignedSnapshot last_snap;
    int             call_count{0};
};
```

Use a minimal stub bus that records pushed batches:

```cpp
class CaptureBus : public util::bus::IDataBus {
public:
    bool push(EventBatch b) override { batches.push_back(std::move(b)); return true; }
    std::vector<EventBatch> batches;
};
```

Build `ChannelProcessor` with stub algorithm, YAML config via `config::Config`:

```yaml
name: test-proc
sources:
  - SRC:A
alignment: latest-value
trigger: any-update
```

| Test name | Scenario |
|---|---|
| `AnyUpdate_PushCausesCompute` | push 1 TS batch → algorithm called once → bus gets 1 output batch |
| `AnyUpdate_OutputReaderName` | output batch `reader_name` == processor name |
| `AnyUpdate_OutputPayloadType` | output batch payload is TimeSeriesPayload with correct root_source_name |
| `NonTSPayload_Accepted_NoCompute` | push SourceMetadataPayload → push returns true, compute not called |
| `AllUpdated_NoComputeUntilBothFresh` | 2 sources, push only SRC:A → compute not called |
| `AllUpdated_ComputeAfterBothFresh` | 2 sources, push SRC:A then SRC:B → compute called once |
| `AllUpdated_FlagsResetAfterCompute` | after first compute, push SRC:A alone → compute not called again |
| `StoppedProcessor_PushReturnsFalse` | push before start() / after stop() → returns false |
| `MultipleOutputs_AllPushed` | stub returns 2 AlgorithmOutput → bus receives 2 batches |

Add test cpp to CMakeLists main test target.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R ChannelProcessor -V
```

## Done Criteria

- All 9 ChannelProcessor unit tests pass.
- No existing tests broken.
