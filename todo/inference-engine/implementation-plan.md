# Channel Processor — Implementation Plan

## Summary

Add a `ChannelProcessor` component type that acts as **both consumer and producer** on the
`IDataBus`.  It subscribes to `EventBatch` from one or more configured source PVs (wired by
the controller's route table exactly like a writer), runs a pluggable algorithm over the
incoming data, and emits new `EventBatch` back onto the bus under a configured virtual-source
name — indistinguishable to downstream writers from any real reader output.

---

## Motivation

The existing pipeline is linear: readers produce, writers consume.  Several use-cases require
derived/computed channels:

- Virtual PV: combine N raw signals into one synthetic measurement
- ML model inference: feed raw time-series into a trained model, re-publish predictions
- Online filtering / smoothing: moving average, Kalman filter output as a new source
- Anomaly scoring: produce a scalar "anomaly score" PV from a set of input PVs

These are all the same pattern: **fan-in from real sources → compute → fan-out as virtual source**.

---

## Architecture Overview

```
 ┌──────────────┐          ┌──────────────────────────────────────┐
 │  Real Reader │          │          ChannelProcessor             │
 │  (produces   │──push──▶│  BASE INFRASTRUCTURE (all free):      │
 │   EventBatch)│          │    acceptsSource() — filters sources  │
 └──────────────┘          │    acceptsPayload() — TimeSeriesOnly  │
                            │    InputBuffer — per-source buffering │
                            │    alignment + trigger policies       │
                            │    bus re-injection plumbing          │
                            │                                       │
                            │  ALGORITHM EXTENSION POINT:          │
                            │    IAlgorithm::compute(snapshot)      │
                            │    → vector<DataColumn>               │
                            └──────────────┬───────────────────────┘
                                           │ bus_->push(virtual EventBatch)
                                           ▼
                                    IDataBus (same bus)
                                           │
                          route table resolves virtual-reader → writers
                                           │
                                           ▼
                                   Downstream Writers
                                 (MLDPPVMetadataWriter, etc.)
```

**Key separation**:
- Base `ChannelProcessor` class owns **all** routing, buffering, alignment, trigger, and
  bus re-injection logic.  Zero algorithm code touches any of this.
- `IAlgorithm::compute()` receives a clean `AlignedSnapshot`; returns `vector<DataColumn>`.
  No bus access, no config parsing for sources, no thread management.

---

## Call Cadence — When Does `compute()` Fire?

`IWriter::push()` is called **immediately** by the controller on every `EventBatch` that
passes the route filter.  Whether `IAlgorithm::compute()` actually runs depends on the
configured **trigger policy**:

| Trigger | `compute()` fires when |
|---|---|
| `any-update` | every `push()` call — one incoming batch from any source |
| `all-updated` | only after every source in `sources:` delivered ≥1 batch since last fire |
| `interval` | worker thread timer fires — independent of `push()` calls |

```
bus::push(EventBatch)
  → controller fan-out
    → ChannelProcessor::push()         ← called on EVERY matching batch (any source, any rate)
        → buffer_.ingest(source, ts)
        → if trigger condition met:
            snap = buffer_.trySnapshot()
            algorithm_->compute(snap)  ← called only when trigger fires
            bus_->push(virtual batch)  ← re-enters bus as new reader_name
```

`compute()` call rate by trigger:
- `any-update`  → one call per incoming batch (matches source update rate)
- `all-updated` → one call per round where all sources have fresh data
- `interval`    → wall-clock rate, decoupled from source rate entirely

---

## Base Infrastructure (owned by `ChannelProcessor` for all processors)

Every algorithm implementation gets these **for free** — none of these need to be re-implemented:

| Responsibility | Where |
|---|---|
| Parse `sources:` list from config | `MLDPChannelProcessorConfig` |
| Parse `output-source` | `MLDPChannelProcessorConfig` |
| Parse `alignment` / `trigger` / `trigger-interval-sec` | `MLDPChannelProcessorConfig` |
| `IWriter::acceptsSource(root_source)` — returns true iff `root_source ∈ sources_` | `ChannelProcessor` base |
| `IWriter::acceptsPayload(payload)` — returns true for `TimeSeriesPayload` only | `ChannelProcessor` base |
| `IWriter::push(EventBatch)` — ingests into `InputBuffer`, fires trigger | `ChannelProcessor` base |
| `InputBuffer` — per-source slot, alignment policy | `ChannelProcessor` base |
| Trigger logic — `any-update` / `all-updated` / `interval` worker thread | `ChannelProcessor` base |
| `bus_->push(virtual EventBatch)` after `compute()` returns | `ChannelProcessor` base |
| `start()` / `stop()` lifecycle, worker thread join | `ChannelProcessor` base |

**Algorithm implementors write only**:
```cpp
std::vector<DataColumn> compute(const AlignedSnapshot& snapshot) override;
```

---

## New Configuration Section

```yaml
controllers:
  - name: main
    processors:                          # parallel to readers:/writers:
      - type: linear-transform
        name: virtual-quad-focus
        # --- BASE INFRASTRUCTURE (all types, no exceptions) ---
        sources:                         # input PV root_sources to consume
          - QUAD:LI21:201:BACT
          - QUAD:LI21:203:BACT
        output-source: VIRTUAL:QUAD:FOCUS:CALC   # root_source of emitted EventBatch
        alignment: latest-value          # latest-value | all-updated | interpolate
        trigger: any-update              # any-update | all-updated | interval
        trigger-interval-sec: 1.0        # required only when trigger=interval
        # --- ALGORITHM-SPECIFIC (type: linear-transform only) ---
        coefficients: [1.2, -0.8]
        bias: 0.05
        output-column: result

      - type: moving-average
        name: smooth-bpm-x
        # base infra (mandatory for every processor type):
        sources:
          - BPM:LI21:201:X
        output-source: VIRTUAL:BPM:LI21:201:X:AVG
        # algorithm-specific:
        window-size: 10
        output-column: avg

      - type: onnx-model
        name: anomaly-detector
        sources:
          - BPM:LI21:201:X
          - BPM:LI21:201:Y
          - BPM:LI21:203:X
        output-source: VIRTUAL:ANOMALY:SCORE
        alignment: interpolate
        trigger: all-updated
        model-path: /path/to/model.onnx
        input-names: [feat0, feat1, feat2]
        output-names: [score]

    routes:
      - reader: epics-archiver
        writer: metadata-writer
      # virtual routes — processor name used as reader name:
      - reader: virtual-quad-focus
        writer: metadata-writer
      - reader: smooth-bpm-x
        writer: metadata-writer
      - reader: anomaly-detector
        writer: metadata-writer
```

---

## Component Design

### 1. `MLDPChannelProcessorConfig` — base config (infrastructure fields only)

```
include/processor/MLDPChannelProcessorConfig.h
src/processor/MLDPChannelProcessorConfig.cpp
```

Parses the **shared** fields that every processor type carries.  Algorithm-specific fields are
left in the raw `config::Config` and read only by `IAlgorithm::configure()`.

```cpp
namespace mldp_pvxs_driver::processor {

class MLDPChannelProcessorConfig {
public:
    struct Error : public std::runtime_error { using std::runtime_error::runtime_error; };
    explicit MLDPChannelProcessorConfig(const config::Config& cfg);

    const std::string&              name()               const noexcept;
    const std::vector<std::string>& sources()            const noexcept;
    const std::string&              outputSource()       const noexcept;
    AlignmentPolicy                 alignment()          const noexcept;
    TriggerPolicy                   trigger()            const noexcept;
    double                          triggerIntervalSec() const noexcept; // trigger=interval only

private:
    std::string              name_;
    std::vector<std::string> sources_;
    std::string              output_source_;
    AlignmentPolicy          alignment_{AlignmentPolicy::LatestValue};
    TriggerPolicy            trigger_{TriggerPolicy::AnyUpdate};
    double                   trigger_interval_sec_{0.0};
};

} // namespace
```

Validation (throws `Error`):
- `name` non-empty
- `sources` non-empty list
- `output-source` non-empty
- `trigger-interval-sec` > 0 when trigger=interval

### 2. `IAlgorithm` — the only interface algorithm authors implement

```
include/processor/IAlgorithm.h
```

```cpp
namespace mldp_pvxs_driver::processor {

class IAlgorithm {
public:
    virtual ~IAlgorithm() = default;

    // Called once on construction with the full config (for algorithm-specific fields)
    virtual void configure(const config::Config& cfg) = 0;

    // Pure compute: aligned snapshot in → output columns out.
    // No bus access, no threading, no source routing.
    virtual std::vector<util::bus::DataColumn> compute(const AlignedSnapshot& snapshot) = 0;

    virtual std::string algorithmType() const noexcept = 0;
};

using IAlgorithmUPtr = std::unique_ptr<IAlgorithm>;

} // namespace
```

### 3. `AlignedSnapshot` — what `IAlgorithm::compute()` receives

```
include/processor/AlignedSnapshot.h
```

```cpp
struct AlignedSnapshot {
    // keyed by root_source string (same as sources: config values)
    std::unordered_map<std::string, util::bus::DataBatch> channels;
    util::bus::BusTimestamp reference_time;
};
```

### 4. `InputBuffer` — base infrastructure, algorithm authors never touch

```
include/processor/InputBuffer.h
src/processor/InputBuffer.cpp
```

```cpp
class InputBuffer {
public:
    explicit InputBuffer(const std::vector<std::string>& source_names,
                         AlignmentPolicy policy);

    // Called by ChannelProcessor::push() for each incoming EventBatch
    void ingest(const std::string& root_source, const util::bus::TimeSeriesPayload& payload);

    // Returns snapshot if trigger condition met, else nullopt.
    // any-update: always returns snapshot after ingest (uses latest per source)
    // all-updated: returns only when every source slot is fresh
    std::optional<AlignedSnapshot> trySnapshot(TriggerPolicy trigger);

    void resetFreshFlags();  // called after snapshot consumed
    void clear();

private:
    std::unordered_map<std::string, util::bus::DataBatch> slots_;
    std::unordered_set<std::string>                       fresh_;
    std::vector<std::string>                              required_sources_;
    AlignmentPolicy                                       alignment_;
};
```

### 5. `IChannelProcessor` — the interface wired by the controller

```
include/processor/IChannelProcessor.h
```

```cpp
class IChannelProcessor : public writer::IWriter {
public:
    // reader_name used in emitted EventBatch = config name
    virtual const std::string& outputReaderName() const noexcept = 0;
    // root_source used in emitted EventBatch
    virtual const std::string& outputSourceName() const noexcept = 0;
};

using IChannelProcessorUPtr = std::unique_ptr<IChannelProcessor>;
```

### 6. `ChannelProcessor` — base implementation (all infrastructure lives here)

```
include/processor/ChannelProcessor.h
src/processor/ChannelProcessor.cpp
```

```cpp
class ChannelProcessor final : public IChannelProcessor {
public:
    ChannelProcessor(MLDPChannelProcessorConfig config,
                     IAlgorithmUPtr algorithm,
                     std::shared_ptr<util::bus::IDataBus> bus,
                     std::shared_ptr<metrics::Metrics> metrics);

    // IWriter — all implemented here, never in algorithm
    std::string name()         const override { return config_.name(); }
    bool        start()        noexcept override;
    bool        stop()         noexcept override;
    bool        push(const util::bus::EventBatch&) noexcept override;
    bool        acceptsPayload(const util::bus::BatchPayload&) const override;
    bool        acceptsSource(const std::string& root_source)  const override;
    bool        supports_multi_root_source() const override { return true; }

    // IChannelProcessor
    const std::string& outputReaderName() const noexcept override { return config_.name(); }
    const std::string& outputSourceName() const noexcept override { return config_.outputSource(); }

private:
    void fireCompute();          // called by push() or interval worker
    void runIntervalWorker();    // only when trigger=interval

    MLDPChannelProcessorConfig             config_;
    IAlgorithmUPtr                         algorithm_;
    std::shared_ptr<util::bus::IDataBus>   bus_;
    std::shared_ptr<metrics::Metrics>      metrics_;
    InputBuffer                            buffer_;
    std::atomic<bool>                      running_{false};
    std::condition_variable                worker_cv_;
    std::mutex                             worker_mutex_;
    std::thread                            worker_thread_;   // only trigger=interval
};
```

**`acceptsSource()` impl** — the route filter that prevents wrong batches reaching the algorithm:
```cpp
bool ChannelProcessor::acceptsSource(const std::string& root_source) const {
    const auto& srcs = config_.sources();
    return std::find(srcs.begin(), srcs.end(), root_source) != srcs.end();
}
```

**`acceptsPayload()` impl** — processors only consume time-series:
```cpp
bool ChannelProcessor::acceptsPayload(const util::bus::BatchPayload& p) const {
    return std::holds_alternative<util::bus::TimeSeriesPayload>(p);
}
```

**`push()` impl** — ingest, check trigger, maybe fire:
```cpp
bool ChannelProcessor::push(const util::bus::EventBatch& batch) noexcept {
    const auto* ts = std::get_if<util::bus::TimeSeriesPayload>(&batch.payload);
    if (!ts) return true;
    buffer_.ingest(batch.root_source, *ts);
    if (config_.trigger() != TriggerPolicy::Interval) {
        if (auto snap = buffer_.trySnapshot(config_.trigger())) {
            fireCompute(*snap);
        }
    }
    return true;
}
```

### 7. `ChannelProcessorFactory`

```
include/processor/ChannelProcessorFactory.h
src/processor/ChannelProcessorFactory.cpp
```

Factory creates a `ChannelProcessor` wrapping whatever `IAlgorithm` the type string maps to.
Algorithm authors never construct the processor shell manually.

```cpp
class ChannelProcessorFactory {
public:
    using AlgorithmFactory = std::function<IAlgorithmUPtr(const config::Config&)>;

    static IChannelProcessorUPtr create(
        const std::string& type,
        const config::Config& cfg,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics> metrics);

    static bool registerAlgorithm(const std::string& type, AlgorithmFactory f);
};

// Self-registration macro for algorithm implementations
#define REGISTER_ALGORITHM(type_str, AlgorithmClass)                              \
    static bool _reg_##AlgorithmClass =                                           \
        ChannelProcessorFactory::registerAlgorithm(                               \
            type_str,                                                              \
            [](const config::Config& cfg) -> IAlgorithmUPtr {                     \
                auto alg = std::make_unique<AlgorithmClass>();                    \
                alg->configure(cfg);                                              \
                return alg;                                                       \
            });
```

`create()` internals:
1. Construct `MLDPChannelProcessorConfig(cfg)` — parse base fields, throw on invalid
2. Lookup algorithm factory by type string, throw if unknown
3. Construct `IAlgorithm` via factory
4. Wrap both in `ChannelProcessor(std::move(config), std::move(alg), bus, metrics)`

---

## Algorithm Implementations

Algorithm authors implement **one class**, one header, one source file.  No bus, no config
for base fields, no threading.

### `LinearTransformAlgorithm`

```cpp
class LinearTransformAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("linear-transform", LinearTransformAlgorithm)
public:
    void configure(const config::Config& cfg) override;
    std::vector<DataColumn> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "linear-transform"; }
private:
    std::vector<double> coefficients_;
    double              bias_{0.0};
    std::string         output_column_{"result"};
};
```

`compute()`: `result = Σ(coefficient[j] * snapshot.channels[sources[j]].latest_scalar) + bias`

### `MovingAverageAlgorithm`

```cpp
class MovingAverageAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("moving-average", MovingAverageAlgorithm)
public:
    void configure(const config::Config& cfg) override;
    std::vector<DataColumn> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "moving-average"; }
private:
    std::deque<double> window_;
    size_t             window_size_{10};
    std::string        output_column_{"avg"};
};
```

### `OnnxModelAlgorithm` (cmake-gated `ENABLE_ONNX=ON`)

```cpp
class OnnxModelAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("onnx-model", OnnxModelAlgorithm)
    // ...
};
```

---

## Output EventBatch Shape

Base `ChannelProcessor::fireCompute()` always emits this shape — algorithm controls only `columns`:

```
EventBatchStruct {
    reader_name  = config_.name()           // processor name — appears as "reader" in routes
    root_source  = config_.outputSource()   // virtual PV name
    metadata     = {
        "processor_type":  algorithm->algorithmType(),
        "processor_name":  config_.name(),
        "source_0": sources[0], "source_1": sources[1], ...
    }
    payload = TimeSeriesPayload {
        frames = [DataBatch {
            timestamps = [snapshot.reference_time],
            columns    = algorithm_->compute(snapshot)   // ← only thing algorithm controls
        }],
        is_tabular         = true,
        end_of_batch_group = true
    }
}
```

---

## Controller Changes

### `MLDPPVXSControllerConfig`

Add `processorEntries() -> vector<pair<string,Config>>` (parse `processors:` list, same pattern
as `readerEntries()` / `writerEntries()`).

### `MLDPPVXSController`

```cpp
// new member
std::vector<IChannelProcessorUPtr> processors_;

// In start():
for (auto& [type, cfg] : controller_config_.processorEntries()) {
    processors_.push_back(
        ChannelProcessorFactory::create(type, cfg, bus_, metrics_));
}
for (auto& p : processors_) p->start();

// In push() dispatch loop — processors treated as writers:
// (existing writer dispatch already calls acceptsSource() + acceptsPayload())
// processors_ included alongside writers_ in the fan-out loop
```

No route table changes needed.  Processors register under `name()` as writers.
Processor output re-enters bus with `reader_name = processor->name()`, naturally routed
to downstream writers by the existing route table.

**No circular route risk**: `acceptsSource()` returns false for `root_source == outputSource()`,
and route table matches `reader_name → writers` so the processor's own output name can never
route back to itself.

---

## File Layout

```
include/processor/
    IChannelProcessor.h
    ChannelProcessorFactory.h
    MLDPChannelProcessorConfig.h
    AlignmentPolicy.h              # enum class AlignmentPolicy
    TriggerPolicy.h                # enum class TriggerPolicy
    AlignedSnapshot.h
    InputBuffer.h
    IAlgorithm.h
    ChannelProcessor.h
    impl/
        LinearTransformAlgorithm.h
        MovingAverageAlgorithm.h
        OnnxModelAlgorithm.h       # guarded by ENABLE_ONNX

src/processor/
    ChannelProcessorFactory.cpp
    MLDPChannelProcessorConfig.cpp
    InputBuffer.cpp
    ChannelProcessor.cpp
    impl/
        LinearTransformAlgorithm.cpp
        MovingAverageAlgorithm.cpp
        OnnxModelAlgorithm.cpp

test/processor/
    MLDPChannelProcessorConfigTest.cpp
    InputBufferTest.cpp
    LinearTransformAlgorithmTest.cpp
    MovingAverageAlgorithmTest.cpp
    ChannelProcessorTest.cpp       # unit: mock bus, mock IAlgorithm
    mldppvxs_controller_processor_integration_test.cpp
```

---

## Implementation Phases

### Phase 1 — Core Plumbing

1. Enums: `AlignmentPolicy`, `TriggerPolicy`
2. `AlignedSnapshot` struct
3. `MLDPChannelProcessorConfig` + unit tests
4. `IAlgorithm` interface
5. `IChannelProcessor` interface
6. `ChannelProcessorFactory` + `REGISTER_ALGORITHM` macro
7. `InputBuffer` (`latest-value` + `all-updated` only) + unit tests
8. `ChannelProcessor` base class (`acceptsSource`, `acceptsPayload`, `push`, `fireCompute`, `start`/`stop`)
9. `MLDPPVXSControllerConfig::processorEntries()` + controller `processors_` wiring
10. `LinearTransformAlgorithm` (validates full pipeline end-to-end)
11. Integration test: real reader → `linear-transform` processor → metadata writer

### Phase 2 — Alignment and Interval Trigger

12. `interval` trigger: worker thread in `ChannelProcessor`, CV-based interruptible sleep
13. `interpolate` alignment policy in `InputBuffer`
14. Unit tests for all alignment + trigger combos

### Phase 3 — More Algorithms

15. `MovingAverageAlgorithm`
16. `OnnxModelAlgorithm` (cmake-gated `ENABLE_ONNX`)

### Phase 4 — Hardening

17. Metrics: processor fire rate, compute latency, buffer depth
18. Back-pressure: drop oldest batch when `InputBuffer` exceeds `max-buffer-depth`
19. Config validation: detect `output-source` collision with real reader `root_source`
20. Chain detection: warn (Phase 4 optional: error) on processor-feeds-processor cycles
21. Integration tests: multi-source alignment, chained processors

---

## Open Questions

| # | Question | Impact |
|---|---|---|
| 1 | Should emitted `EventBatch` carry a `SourceMetadataPayload` push before `TimeSeriesPayload`? Downstream writers may expect metadata before time-series for a new source. | Phase 1 |
| 2 | Should `output-source` shadowing a real reader's `root_source` be a hard error (config throw) or a warning? | Phase 1 config validation |
| 3 | Can a processor feed another processor (chain)? Route table allows it; circular chain detection needed. | Phase 4 |
| 4 | ONNX Runtime: git submodule vs system dep vs CMake FetchContent? | Phase 3 |
| 5 | Multi-controller: if two controllers share one bus, processor output is visible to both controllers' writers. Single-controller assumption safe? | Architecture |
| 6 | `MovingAverageAlgorithm` accumulates state across calls — should `stop()`/`start()` clear the window? | Phase 3 |

---

## Non-Goals

- Feedback loops between virtual sources and physical actuators (PV write-back)
- Distributed/remote algorithm execution
- Python-embedded algorithms
- Algorithm versioning / A-B testing
- Hot-reload of algorithm config without controller restart
