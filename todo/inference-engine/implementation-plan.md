# Channel Processor — Implementation Plan

## Summary

Add a `ChannelProcessor` component type that acts as **both consumer and producer** on the
`IDataBus`.  It subscribes to `EventBatch` from one or more configured source PVs (wired by
the controller's route table exactly like a writer), runs a pluggable algorithm over the
incoming data, and emits new `EventBatch` back onto the bus under a configured virtual-source
name — indistinguishable to downstream writers from any real reader output.

---

## Related Plans

| Plan | Scope |
|---|---|
| [lua-processor-plan.md](./lua-processor-plan.md) | Lua 5.4 script processor: `LuaAlgorithm`, `LuaScriptDirectoryLoader`, `mldp` Lua bridge |
| [python-processor-plan.md](./python-processor-plan.md) | Python 3 script processor: `PythonAlgorithm`, `PythonScriptDirectoryLoader`, `mldp` Python module |

---

## New Classes — Quick Reference

| Class / Interface | Kind | Role |
|---|---|---|
| `MLDPChannelProcessorConfig` | concrete class | Parses + validates **base** config fields only: `name`, `sources`, `alignment`, `trigger`. No `output-source` — outputs are algorithm-specific. |
| `AlignmentPolicy` | `enum class` | Values: `LatestValue`, `AllUpdated`, `Interpolate`. Controls how `InputBuffer` forms an `AlignedSnapshot`. |
| `TriggerPolicy` | `enum class` | Values: `AnyUpdate`, `AllUpdated`, `Interval`. Controls when `fireCompute()` is called. |
| `AlignedSnapshot` | plain struct | Payload passed to `IAlgorithm::compute()`: map of `root_source -> DataBatch` + `reference_time`. |
| `AlgorithmOutput` | plain struct | One virtual source emission: `{ string output_source; BatchPayload payload; }`. C++ algorithms fill the variant directly. Script bridges use language-specific `mldp` namespace to emit typed payloads. |
| `IAlgorithm` | pure interface | `configure()` reads algorithm-specific config (incl. output source names). `outputSources()` declares output names after configure. `compute()` returns `vector<AlgorithmOutput>`. |
| `IChannelProcessor` | pure interface (extends `IWriter`) | Adds `outputReaderName()` / `outputSourceNames()` (plural) to the writer interface so the controller can wire virtual routes. |
| `InputBuffer` | concrete class | Per-source slot storage, tracks freshness, implements alignment + trigger snapshot logic. |
| `ChannelProcessor` | concrete class (owns `IAlgorithm`) | All infrastructure: `acceptsSource`, `acceptsPayload`, `push`, `fireCompute`, interval worker thread, bus re-injection. |
| `ChannelProcessorFactory` | static factory | Maps type string -> `ProcessorFactory`; returns `vector<IChannelProcessorUPtr>` (all types, incl. script bulk-loaders). Provides `REGISTER_ALGORITHM` macro. |
| `LinearTransformAlgorithm` | `IAlgorithm` impl | `result = sum(coeff[j] * source[j]) + bias`. No state. |
| `MovingAverageAlgorithm` | `IAlgorithm` impl | Sliding window mean over last N values. Stateful (`std::deque`). |
| `EchoAlgorithm` | `IAlgorithm` impl (test-only) | Pass-through with DEBUG logging. Gated by `BUILD_ECHO_PROCESSOR`. |

---

## Class Diagram

```
+-------------------------------------------------------------------------------------------------+
|                            INTERFACES / BASE TYPES                                               |
|                                                                                                  |
|   <<interface>>                    <<interface>>                                                  |
|   IWriter                          IChannelProcessor                                             |
|   -----------------                --------------------------------                              |
|   + name() : string                extends IWriter                                               |
|   + start() : bool                 + outputReaderName() : string                                 |
|   + stop() : bool                  + outputSourceNames() : vector<string>                        |
|   + push(EventBatch) : bool          <- plural: algorithm may emit N virtual sources             |
|   + acceptsSource(string) : bool                                                                 |
|   + acceptsPayload(BatchPayload) : bool  <<interface>>                                           |
|                                          IAlgorithm                                              |
|                                          -------------------------                               |
|          ^                               + configure(Config)                                     |
|          | extends                        + outputSources() : vector<string>                      |
|          |                                 <- declared after configure(), algorithm-owned          |
|                                          + compute(AlignedSnapshot)                               |
|                                            -> vector<AlgorithmOutput>                             |
|                                          + algorithmType() : string                              |
|   +------+-------------------------------+         ^                                             |
|   |        ChannelProcessor              |         | implements                                  |
|   |  ----------------------------------- |  +------+------------------------------------------+  |
|   |  - config_ : MLDPChannelProcessor    |  |  LinearTransformAlgorithm                       |  |
|   |             Config                   |  |  MovingAverageAlgorithm                         |  |
|   |  - algorithm_ : IAlgorithmUPtr       |  |  EchoAlgorithm   (BUILD_ECHO_PROCESSOR)         |  |
|   |  - bus_ : IDataBus                   |  |  [Script algorithms via IAlgorithm]             |  |
|   |  - buffer_ : InputBuffer             |  +---------------------------------------------+     |
|   |  - running_ : atomic<bool>           |                                                       |
|   |  ----------------------------------- |                                                       |
|   |  + push(EventBatch)                  |                                                       |
|   |  + acceptsSource(string)             |                                                       |
|   |  + acceptsPayload(BatchPayload)      |                                                       |
|   |  + fireCompute()           [priv]    |                                                       |
|   |  + runIntervalWorker()     [priv]    |                                                       |
|   +--------------------------------------+                                                       |
+-------------------------------------------------------------------------------------------------+

+-------------------------------------------------------------------------------------------------+
|                            SUPPORTING TYPES                                                      |
|                                                                                                  |
|  MLDPChannelProcessorConfig          InputBuffer                                                 |
|  ---------------------------         ----------------------------------------                    |
|  - name_ : string                    - slots_ : map<string, DataBatch>                           |
|  - sources_ : vector<string>         - fresh_ : set<string>                                      |
|  - alignment_ : AlignmentPolicy      - required_sources_ : vector<string>                        |
|  - trigger_ : TriggerPolicy          - alignment_ : AlignmentPolicy                              |
|  - trigger_interval_sec_ : double    ----------------------------------------                    |
|  (NO output-source -- algorithm owns)+ ingest(source, TimeSeriesPayload)                         |
|                                      + trySnapshot(TriggerPolicy)                                |
|                                        -> optional<AlignedSnapshot>                               |
|  AlignedSnapshot                     + resetFreshFlags()                                         |
|  -----------------                   + clear()                                                   |
|  channels : map<string, DataBatch>                                                               |
|  reference_time : BusTimestamp                                                                   |
|                                                                                                  |
|  AlgorithmOutput                                                                                 |
|  ---------------------------------                                                               |
|  output_source : string    <- algorithm names its own virtual PV                                 |
|  payload : BatchPayload    <- C++ fills directly; scripts use mldp.* bridge                      |
|                                                                                                  |
|  enum class AlignmentPolicy          enum class TriggerPolicy                                    |
|  -------------------------           --------------------------                                  |
|  LatestValue                         AnyUpdate                                                   |
|  AllUpdated                          AllUpdated                                                  |
|  Interpolate                         Interval                                                    |
+-------------------------------------------------------------------------------------------------+

+-------------------------------------------------------------------------------------------------+
|                            FACTORY / LOADERS                                                     |
|                                                                                                  |
|  ChannelProcessorFactory                                                                         |
|  --------------------------------------                                                          |
|  <<static>>                                                                                      |
|  + create(type, cfg, bus, metrics)                                                               |
|    -> vector<IChannelProcessorUPtr>   <- always a vector; single types return {1}                |
|  + registerType(type, ProcessorFactory)                                                          |
|  REGISTER_ALGORITHM(type, Class) macro  <- convenience for single-algorithm types                |
|                                                                                                  |
|  Registry: map<string, ProcessorFactory>                                                         |
|                                                                                                  |
|  [Script directory loaders registered as ProcessorFactory entries]                               |
|  See: lua-processor-plan.md, python-processor-plan.md                                            |
+-------------------------------------------------------------------------------------------------+
```

---

## Motivation

The existing pipeline is linear: readers produce, writers consume.  Several use-cases require
derived/computed channels:

- Virtual PV: combine N raw signals into one synthetic measurement
- ML model inference: feed raw time-series into a trained model, re-publish predictions
- Online filtering / smoothing: moving average, Kalman filter output as a new source
- Anomaly scoring: produce a scalar "anomaly score" PV from a set of input PVs

These are all the same pattern: **fan-in from real sources -> compute -> fan-out as virtual source**.

---

## Architecture Overview

```
 +---------------+     +-----------------------------------------------------------+
 |  Real Reader  |     |                  ChannelProcessor                          |
 |  (produces    |---->|  BASE INFRASTRUCTURE (shared, all algorithm types):        |
 |   EventBatch) |     |    acceptsSource()  -- filters to configured sources       |
 +---------------+     |    acceptsPayload() -- TimeSeriesPayload only              |
                       |    InputBuffer     -- per-source slot + freshness           |
                       |    alignment + trigger policies                             |
                       |    bus re-injection loop                                    |
                       |                                                            |
                       |  ALGORITHM EXTENSION POINT:                                |
                       |    IAlgorithm::configure(Config)                            |
                       |      reads ALL algorithm-specific fields                    |
                       |      (output source names, weights, window size, ...)       |
                       |    IAlgorithm::outputSources() -> vector<string>            |
                       |      declared after configure(), used for route wiring      |
                       |    IAlgorithm::compute(AlignedSnapshot)                     |
                       |      -> vector<AlgorithmOutput>                             |
                       |        each entry: { output_source, BatchPayload }          |
                       +------------------+----------------------------------------+
                                          | bus_->push() once per AlgorithmOutput
                                          v
                                   IDataBus (same bus)
                                          |
                       route table: virtual reader_name -> writers
                                          |
                                          v
                                  Downstream Writers
                                (MLDPPVMetadataWriter, etc.)
```

**Key separations**:
- `MLDPChannelProcessorConfig` owns only **input** fields: `name`, `sources`, `alignment`, `trigger`.
  No `output-source` — algorithm decides its outputs.
- Each `IAlgorithm` implementation owns **all algorithm-specific config** (read in `configure()`),
  including output source names, model paths, window sizes, coefficients — whatever the algorithm needs.
- `compute()` returns `vector<AlgorithmOutput>`; base calls `bus_->push()` for each entry.

---

## Call Cadence — When Does `compute()` Fire?

`IWriter::push()` is called **immediately** by the controller on every `EventBatch` that
passes the route filter.  Whether `IAlgorithm::compute()` actually runs depends on the
configured **trigger policy**:

| Trigger | `compute()` fires when |
|---|---|
| `any-update` | every `push()` call — one incoming batch from any source |
| `all-updated` | only after every source in `sources:` delivered >=1 batch since last fire |
| `interval` | worker thread timer fires — independent of `push()` calls |

```
bus::push(EventBatch)
  -> controller fan-out
    -> ChannelProcessor::push()         <- called on EVERY matching batch (any source, any rate)
        -> buffer_.ingest(source, ts)
        -> if trigger condition met:
            snap = buffer_.trySnapshot()
            algorithm_->compute(snap)  <- called only when trigger fires
            bus_->push(virtual batch)  <- re-enters bus as new reader_name
```

`compute()` call rate by trigger:
- `any-update`  -> one call per incoming batch (matches source update rate)
- `all-updated` -> one call per round where all sources have fresh data
- `interval`    -> wall-clock rate, decoupled from source rate entirely

---

## Base Infrastructure (owned by `ChannelProcessor` for all processors)

Every algorithm implementation gets these **for free** — none of these need to be re-implemented:

| Responsibility | Where |
|---|---|
| Parse `sources:` list from config | `MLDPChannelProcessorConfig` |
| Parse `alignment` / `trigger` / `trigger-interval-sec` | `MLDPChannelProcessorConfig` |
| `IWriter::acceptsSource(root_source)` — returns true iff `root_source in sources_` | `ChannelProcessor` base |
| `IWriter::acceptsPayload(payload)` — returns true for `TimeSeriesPayload` only | `ChannelProcessor` base |
| `IWriter::push(EventBatch)` — ingests into `InputBuffer`, fires trigger | `ChannelProcessor` base |
| `InputBuffer` — per-source slot, alignment policy | `ChannelProcessor` base |
| Trigger logic — `any-update` / `all-updated` / `interval` worker thread | `ChannelProcessor` base |
| Loop over `vector<AlgorithmOutput>`, emit one `bus_->push()` per entry | `ChannelProcessor` base |
| `start()` / `stop()` lifecycle, worker thread join | `ChannelProcessor` base |

**Algorithm implementors write only**:
```cpp
// All algorithm-specific config (incl. output source names) — whatever keys the algorithm needs:
void configure(const config::Config& cfg) override;

// Declares output virtual PV names after configure(); base uses this for route wiring:
std::vector<std::string> outputSources() const noexcept override;

// Pure compute — no bus, no threading:
std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
```

---

## New Configuration Section

Base fields (`name`, `sources`, `alignment`, `trigger`) are shared across all processor types.
Every algorithm-specific field — including output source name(s) — is defined under keys that
the algorithm implementation chooses.  There is **no** shared `output-source` key.

```yaml
controllers:
  - name: main
    processors:                          # parallel to readers:/writers: — all processor types here
      - type: linear-transform
        name: virtual-quad-focus
        # --- BASE INFRASTRUCTURE (mandatory for all types) ---
        sources:                         # input PV root_sources to consume
          - QUAD:LI21:201:BACT
          - QUAD:LI21:203:BACT
        alignment: latest-value          # latest-value | all-updated | interpolate
        trigger: any-update              # any-update | all-updated | interval
        trigger-interval-sec: 1.0        # required only when trigger=interval
        # --- ALGORITHM-SPECIFIC (type: linear-transform — owns output naming) ---
        output-source: VIRTUAL:QUAD:FOCUS:CALC   # algorithm's own key name
        coefficients: [1.2, -0.8]
        bias: 0.05
        output-column: result

      - type: moving-average
        name: smooth-bpm-x
        # base (mandatory):
        sources:
          - BPM:LI21:201:X
        alignment: latest-value
        trigger: any-update
        # algorithm-specific:
        output-source: VIRTUAL:BPM:LI21:201:X:AVG   # algorithm's own key name
        window-size: 10
        output-column: avg

      - type: lua-processor              # see lua-processor-plan.md
        script-dir: /etc/mldp/lua-processors

      - type: python-processor           # see python-processor-plan.md
        script-dir: /etc/mldp/python-processors

    routes:
      - reader: epics-archiver
        writer: metadata-writer
      # virtual routes — processor name used as reader name:
      - reader: virtual-quad-focus
        writer: metadata-writer
      - reader: smooth-bpm-x
        writer: metadata-writer
      - reader: "*"                       # wildcard catches all script processors
        writer: metadata-writer
```

> **Rule**: `output-source` (or any equivalent key) is read **only** by
> `IAlgorithm::configure()`.  `MLDPChannelProcessorConfig` never touches it.
> Algorithm implementations that emit multiple virtual PVs may use a list key instead:
> ```yaml
> output-sources:
>   - VIRTUAL:MODEL:PRED
>   - VIRTUAL:MODEL:UNCERTAINTY
> ```

---

## Component Design

### 1. `MLDPChannelProcessorConfig` — base config (input routing fields only)

```
include/processor/MLDPChannelProcessorConfig.h
src/processor/MLDPChannelProcessorConfig.cpp
```

Parses **only** the shared input-routing fields.  No output naming — that belongs entirely to
each `IAlgorithm` implementation.  Algorithm-specific config is left in the raw `config::Config`
and read only by `IAlgorithm::configure()`.

```cpp
namespace mldp_pvxs_driver::processor {

class MLDPChannelProcessorConfig {
public:
    struct Error : public std::runtime_error { using std::runtime_error::runtime_error; };
    explicit MLDPChannelProcessorConfig(const config::Config& cfg);

    const std::string&              name()               const noexcept;
    const std::vector<std::string>& sources()            const noexcept;
    AlignmentPolicy                 alignment()          const noexcept;
    TriggerPolicy                   trigger()            const noexcept;
    double                          triggerIntervalSec() const noexcept; // trigger=interval only

private:
    std::string              name_;
    std::vector<std::string> sources_;
    AlignmentPolicy          alignment_{AlignmentPolicy::LatestValue};
    TriggerPolicy            trigger_{TriggerPolicy::AnyUpdate};
    double                   trigger_interval_sec_{0.0};
};

} // namespace
```

Validation (throws `Error`):
- `name` non-empty
- `sources` non-empty list
- `trigger-interval-sec` > 0 when trigger=interval
- No `output-source` validation — algorithm responsible for that

### 2. `IAlgorithm` — the only interface algorithm authors implement

```
include/processor/IAlgorithm.h
```

Each algorithm implementation reads **all** its own config in `configure()`.
Output source naming is entirely algorithm-owned — no shared `output-source` base field.
Algorithm authors choose their own YAML key names freely.

```cpp
namespace mldp_pvxs_driver::processor {

// One virtual-PV emission from compute().  C++ algorithms fill payload directly.
// Script bridges build payload from language-specific mldp.* tagged objects.
struct AlgorithmOutput {
    std::string             output_source;  // virtual PV root_source — named by algorithm
    util::bus::BatchPayload payload;        // TimeSeriesPayload | SourceMetadataPayload |
                                            // ConfigurationPayload | ConfigurationActivationPayload
};

class IAlgorithm {
public:
    virtual ~IAlgorithm() = default;

    // Read ALL algorithm-specific config here.
    // YAML key names are algorithm-defined (e.g. "output-source", "outputs", "model-path", ...).
    // Base processor config (sources, alignment, trigger) is already parsed — do NOT re-read it here.
    virtual void configure(const config::Config& cfg) = 0;

    // Declares ALL output virtual PV names after configure().
    // Called by ChannelProcessor at construction for route-wiring.
    // Must be stable after configure() returns.
    virtual std::vector<std::string> outputSources() const noexcept = 0;

    // Pure compute: snapshot in -> one AlgorithmOutput per virtual PV.
    // No bus access, no threading, no source routing.
    virtual std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) = 0;

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
    // reader_name placed in emitted EventBatch = processor config name
    virtual const std::string&              outputReaderName()  const noexcept = 0;
    // All root_sources this processor may emit — delegated from IAlgorithm::outputSources()
    virtual std::vector<std::string>        outputSourceNames() const noexcept = 0;
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
    const std::string&       outputReaderName()  const noexcept override { return config_.name(); }
    std::vector<std::string> outputSourceNames() const noexcept override { return algorithm_->outputSources(); }

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

**`acceptsSource()` impl**:
```cpp
bool ChannelProcessor::acceptsSource(const std::string& root_source) const {
    const auto& srcs = config_.sources();
    return std::find(srcs.begin(), srcs.end(), root_source) != srcs.end();
}
```

**`acceptsPayload()` impl**:
```cpp
bool ChannelProcessor::acceptsPayload(const util::bus::BatchPayload& p) const {
    return std::holds_alternative<util::bus::TimeSeriesPayload>(p);
}
```

**`push()` impl**:
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
    // Factory function: maps a config block -> 0..N processors.
    // Single-algorithm types return {1}; script-dir types (lua-processor, python-processor)
    // return one entry per valid script file found in script-dir.
    using ProcessorFactory = std::function<
        std::vector<IChannelProcessorUPtr>(
            const config::Config&,
            std::shared_ptr<util::bus::IDataBus>,
            std::shared_ptr<metrics::Metrics>)>;

    static std::vector<IChannelProcessorUPtr> create(
        const std::string& type,
        const config::Config& cfg,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics> metrics);

    static bool registerType(const std::string& type, ProcessorFactory f);
};

// Convenience macro for single-algorithm types (wraps IAlgorithm -> one ChannelProcessor)
#define REGISTER_ALGORITHM(type_str, AlgorithmClass)                              \
    static bool _reg_##AlgorithmClass =                                           \
        ChannelProcessorFactory::registerType(                                    \
            type_str,                                                              \
            [](const config::Config& cfg,                                          \
               std::shared_ptr<util::bus::IDataBus> bus,                          \
               std::shared_ptr<metrics::Metrics> metrics)                          \
                -> std::vector<IChannelProcessorUPtr> {                           \
                auto alg = std::make_unique<AlgorithmClass>();                    \
                alg->configure(cfg);                                              \
                auto proc = std::make_unique<ChannelProcessor>(                   \
                    MLDPChannelProcessorConfig(cfg), std::move(alg), bus, metrics);\
                std::vector<IChannelProcessorUPtr> v;                             \
                v.push_back(std::move(proc));                                     \
                return v;                                                          \
            });
```

`create()` internals:
1. Lookup `ProcessorFactory` by type string, throw if unknown
2. Delegate entirely to the registered factory — single-algorithm types parse base config + construct one `ChannelProcessor`; script-dir types call their respective loader and return N processors
3. Controller merges results into `processors_` with `insert` + move iterators

---

## Algorithm Implementations

Algorithm authors implement **one class**, one header, one source file.  No bus, no config
for base fields, no threading.

### `LinearTransformAlgorithm`

Reads all its own config in `configure()` — including the output source name under whatever
YAML key it chooses (`output-source` by convention).  Base config never touches it.

```cpp
class LinearTransformAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("linear-transform", LinearTransformAlgorithm)
public:
    void configure(const config::Config& cfg) override;
    // Algorithm-specific config read here:
    //   output-source: VIRTUAL:QUAD:FOCUS:CALC   <- algorithm's own key
    //   coefficients: [1.2, -0.8]
    //   bias: 0.05
    //   output-column: result

    std::vector<std::string>  outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "linear-transform"; }
private:
    std::string         output_source_{"VIRTUAL:LINEAR:OUT"};
    std::vector<double> coefficients_;
    double              bias_{0.0};
    std::string         output_column_{"result"};
};
```

`compute()`: returns `{ AlgorithmOutput{ output_source_, TimeSeriesPayload{ DataBatch{ {output_column_: result} } } } }`
where `result = sum(coefficient[j] * snapshot.channels[sources[j]].latest_scalar) + bias`

### `MovingAverageAlgorithm`

Reads all its own config in `configure()` including the output source name.

```cpp
class MovingAverageAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("moving-average", MovingAverageAlgorithm)
public:
    void configure(const config::Config& cfg) override;
    // Algorithm-specific config read here:
    //   output-source: VIRTUAL:BPM:LI21:201:X:AVG   <- algorithm's own key
    //   window-size: 10
    //   output-column: avg

    std::vector<std::string>  outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "moving-average"; }
private:
    std::string        output_source_{"VIRTUAL:MA:OUT"};
    std::deque<double> window_;
    size_t             window_size_{10};
    std::string        output_column_{"avg"};
};
```

### `EchoAlgorithm` (cmake-gated `BUILD_ECHO_PROCESSOR=ON`, test/example only)

Logs every received value and re-emits unchanged.  Not built by default — smoke-test only.
Algorithm reads its own output source name via `configure()`:

```yaml
- type: echo
  name: bpm-x-echo
  sources:
    - BPM:LI21:201:X
  # algorithm-specific — EchoAlgorithm reads this:
  output-source: BPM:LI21:201:X-echo    # optional: auto-derived as sources[0]+"-echo" if absent
```

```cpp
class EchoAlgorithm final : public IAlgorithm {
    REGISTER_ALGORITHM("echo", EchoAlgorithm)
public:
    void configure(const config::Config& cfg) override;
    std::vector<std::string>  outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "echo"; }
private:
    std::string output_source_;  // set in configure(); default sources[0]+"-echo"
};
```

CMake guard:
```cmake
if(BUILD_ECHO_PROCESSOR)
    target_sources(mldp_pvxs_driver PRIVATE
        src/processor/impl/EchoAlgorithm.cpp)
endif()
```

---

## Script Algorithm Abstraction

Script-based processors (Lua, Python, future languages) share a common integration pattern
with the core infrastructure.  This section defines the abstraction — language-specific
details live in their respective plans.

### Pattern: Directory Loader as `ProcessorFactory`

Each script language registers a `ProcessorFactory` that:
1. Takes a `script-dir` path from config
2. Scans directory for script files (`*.lua`, `*.py`, etc.)
3. Per file: loads script, reads `config` table/dict for base fields
4. Constructs `MLDPChannelProcessorConfig` from base fields
5. Constructs language-specific `IAlgorithm` impl holding the script runtime state
6. Wraps in `ChannelProcessor(config, algorithm, bus, metrics)`
7. Returns `vector<IChannelProcessorUPtr>` (one per valid script)
8. On per-file error: log warning + skip (other scripts continue)

### Script `config` Contract

All script languages expose a module-level `config` structure with these **base fields**
(read by the directory loader, NOT by the algorithm):

| Field | Required | Default |
|---|---|---|
| `name` | yes | — |
| `sources` | yes | — |
| `alignment` | no | `"latest-value"` |
| `trigger` | no | `"any-update"` |
| `output_sources` (or `output_source`) | yes | — |

`output_sources` / `output_source` is read by the algorithm's `configure()` for route wiring.

### `mldp` Bridge API Contract

Each script language provides a namespace/module (`mldp`) with typed payload constructors:

| Constructor | Maps to `BatchPayload` variant |
|---|---|
| `mldp.timeseries(source, columns)` | `TimeSeriesPayload` |
| `mldp.source_metadata(source, fields)` | `SourceMetadataPayload` |
| `mldp.configuration(source, data)` | `ConfigurationPayload` |
| `mldp.configuration_activation(source)` | `ConfigurationActivationPayload` |

Each constructor returns a **tagged object** (language-specific representation).
The C++ bridge in the algorithm's `compute()` reads the tag and constructs the
appropriate `BatchPayload` variant for `AlgorithmOutput`.

### `compute(snapshot)` Contract

- **Input**: language-native map/dict/table keyed by `root_source` string, values = scalar float.
  Includes `reference_time` field.
- **Output**: single tagged object OR list of tagged objects.
  Each -> one `AlgorithmOutput` -> one `bus_->push()` call.
- Runtime errors: log warning, return empty (base skips push).
- Source name validation: warn if emitted source not in declared `output_sources`.

### Algorithm Type Summary

| Type | Dep | Stateful | Who writes | Config location |
|---|---|---|---|---|
| `linear-transform` | none | no | C++ class | YAML `processors:` |
| `moving-average` | none | yes (window) | C++ class | YAML `processors:` |
| `echo` | none | no | C++ class (test only) | YAML `processors:` (`BUILD_ECHO_PROCESSOR=ON`) |
| `lua-processor` | Lua 5.4 (~200KB) | yes (script globals) | physics/ops team | `.lua` file — see [lua-processor-plan.md](./lua-processor-plan.md) |
| `python-processor` | CPython 3.x (~3MB) | yes (module globals) | physics/ops team | `.py` file — see [python-processor-plan.md](./python-processor-plan.md) |

---

## Output EventBatch Shape

`ChannelProcessor::fireCompute()` calls `algorithm_->compute(snap)` -> `vector<AlgorithmOutput>`.
Base emits **one `bus_->push()`** per entry, passing `ao.payload` through unchanged.

```
// For each AlgorithmOutput ao in algorithm_->compute(snapshot):

EventBatchStruct {
    reader_name  = config_.name()           // processor name — appears as "reader" in routes
    root_source  = ao.output_source         // <- algorithm-owned virtual PV name
    metadata     = {
        "processor_type":  algorithm_->algorithmType(),
        "processor_name":  config_.name(),
        "source_0": sources[0], "source_1": sources[1], ...
    }
    payload      = ao.payload               // <- full BatchPayload variant from algorithm/bridge
}
```

C++ algorithm example — `LinearTransformAlgorithm::compute()`:
```cpp
return {{ output_source_, util::bus::TimeSeriesPayload{
    { DataBatch{ {snapshot.reference_time}, {{output_column_, {result}}} } },
    /*is_tabular=*/true, /*end_of_batch_group=*/true
}}};
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
    auto batch = ChannelProcessorFactory::create(type, cfg, bus_, metrics_);
    processors_.insert(processors_.end(),
        std::make_move_iterator(batch.begin()),
        std::make_move_iterator(batch.end()));
}
for (auto& p : processors_) p->start();

// In push() dispatch loop — processors treated as writers:
// (existing writer dispatch already calls acceptsSource() + acceptsPayload())
// processors_ included alongside writers_ in the fan-out loop
```

No route table changes needed.  Processors register under `name()` as writers.
Processor output re-enters bus with `reader_name = processor->name()`, naturally routed
to downstream writers by the existing route table.

**No circular route risk**: `acceptsSource()` checks only `config_.sources()` (input PVs).
Algorithm output names are never in that list — route table matches `reader_name -> writers`
so a processor's own virtual-PV outputs can never route back to itself.

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
        EchoAlgorithm.h             # guarded by BUILD_ECHO_PROCESSOR

src/processor/
    ChannelProcessorFactory.cpp
    MLDPChannelProcessorConfig.cpp
    InputBuffer.cpp
    ChannelProcessor.cpp
    impl/
        LinearTransformAlgorithm.cpp
        MovingAverageAlgorithm.cpp
        EchoAlgorithm.cpp

test/processor/
    MLDPChannelProcessorConfigTest.cpp
    InputBufferTest.cpp
    LinearTransformAlgorithmTest.cpp
    MovingAverageAlgorithmTest.cpp
    ChannelProcessorTest.cpp       # unit: mock bus, mock IAlgorithm
    mldppvxs_controller_processor_integration_test.cpp
```

Script processor files listed in their respective plans:
- [lua-processor-plan.md](./lua-processor-plan.md)
- [python-processor-plan.md](./python-processor-plan.md)

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
11. Integration test: real reader -> `linear-transform` processor -> metadata writer

### Phase 2 — Alignment and Interval Trigger

12. `interval` trigger: worker thread in `ChannelProcessor`, CV-based interruptible sleep
13. `interpolate` alignment policy in `InputBuffer`
14. Unit tests for all alignment + trigger combos

### Phase 3 — More C++ Algorithms

15. `MovingAverageAlgorithm`
16. `EchoAlgorithm` (cmake-gated `BUILD_ECHO_PROCESSOR`)

### Phase 4 — Script Processors

17. Lua processor — see [lua-processor-plan.md](./lua-processor-plan.md)
18. Python processor — see [python-processor-plan.md](./python-processor-plan.md)

### Phase 5 — Hardening

19. Metrics: processor fire rate, compute latency, buffer depth
20. Back-pressure: drop oldest batch when `InputBuffer` exceeds `max-buffer-depth`
21. Config validation: detect `output-source` collision with real reader `root_source`
22. Chain detection: warn (optional: error) on processor-feeds-processor cycles
23. Integration tests: multi-source alignment, chained processors

---

## Open Questions

| # | Question | Impact |
|---|---|---|
| 1 | Should emitted `EventBatch` carry a `SourceMetadataPayload` push before `TimeSeriesPayload`? Downstream writers may expect metadata before time-series for a new source. | Phase 1 |
| 2 | Should `output-source` shadowing a real reader's `root_source` be a hard error (config throw) or a warning? | Phase 1 config validation |
| 3 | Can a processor feed another processor (chain)? Route table allows it; circular chain detection needed. | Phase 5 |
| 4 | Multi-controller: if two controllers share one bus, processor output is visible to both controllers' writers. Single-controller assumption safe? | Architecture |
| 5 | `MovingAverageAlgorithm` accumulates state across calls — should `stop()`/`start()` clear the window? | Phase 3 |

---

## Non-Goals

- Feedback loops between virtual sources and physical actuators (PV write-back)
- Distributed/remote algorithm execution
- Algorithm versioning / A-B testing
- Hot-reload of algorithm config without controller restart
