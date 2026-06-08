# Step 06 — LinearTransformAlgorithm + Controller Wiring

## Goal

First real `IAlgorithm` implementation. Validates the full pipeline end-to-end:
config parsing → factory registration → `ChannelProcessor` → bus re-injection → writer.

Also adds `processors:` support to `MLDPPVXSControllerConfig` and `MLDPPVXSController`.

## Depends On

Steps 01–05.

---

## Files to Create

### `include/processor/impl/LinearTransformAlgorithm.h`

```cpp
#pragma once
#include <processor/IAlgorithm.h>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class LinearTransformAlgorithm final : public IAlgorithm {
public:
    void configure(const config::Config& cfg) override;
    // Reads from cfg:
    //   output-source: string (required)
    //   coefficients:  list<double> (required, length must match sources count — validated at compute time)
    //   bias:          double (optional, default 0.0)
    //   output-column: string (optional, default "result")

    std::vector<std::string>     outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType()  const noexcept override { return "linear-transform"; }

private:
    std::string         output_source_{"VIRTUAL:LINEAR:OUT"};
    std::vector<double> coefficients_;
    double              bias_{0.0};
    std::string         output_column_{"result"};
};

} // namespace
```

### `src/processor/impl/LinearTransformAlgorithm.cpp`

**`configure(cfg)`**:
- Read `output-source` — required non-empty string. Throw `std::runtime_error` if absent/empty.
- Read `coefficients` — required list of doubles. Throw if absent or empty.
- Read `bias` — optional double, default 0.0.
- Read `output-column` — optional string, default `"result"`.

**`compute(snapshot)`**:
- For each source `i` in `snapshot.channels` (ordered by algorithm's internal coefficient index):
  - Sources map is unordered. Algorithm must compute `result` by iterating `coefficients_` and
    matching to snapshot channels by the order implied at `configure()` time.
  - Simple approach: `MLDPChannelProcessorConfig::sources()` order is the coefficient order.
    But `LinearTransformAlgorithm` doesn't own the sources list — it only owns coefficients.
  - **Resolved approach**: iterate `coefficients_[i]` in order; need source[i] from snapshot.
    Store `sources_` list in `configure()` by reading the `sources` key from cfg (same key as base config).
    Base config and algorithm both read it — algorithm needs it only for coefficient matching.
- `result = sum(coefficients_[i] * first_scalar_value(snapshot.channels[sources_[i]])) + bias_`
- `first_scalar_value`: extract first `double` from `DataBatch.columns[0].values` (variant index 0 = `vector<double>`).
  If column empty or wrong type, use 0.0.
- Return `{ AlgorithmOutput{ output_source_, TimeSeriesPayload{
    .root_source_name = output_source_,
    .frames = { DataBatch{ {snapshot.reference_time}, { DataColumn{ output_column_, vector<double>{result} } } } },
    .is_tabular = true,
    .end_of_batch_group = true
  } } }`.

Place `REGISTER_ALGORITHM("linear-transform", LinearTransformAlgorithm)` at file scope in `.cpp`.

---

## Files to Modify

### `include/controller/MLDPPVXSControllerConfig.h`

Add:
```cpp
// Returns processor entries as (type, config-node) pairs.
// Parsed from the `processors:` YAML sequence (same structure as readers/writers).
const std::vector<std::pair<std::string, config::Config>>& processorEntries() const;
```

Add private member:
```cpp
std::vector<std::pair<std::string, config::Config>> processorEntries_;
```

Add private method:
```cpp
void parseProcessors(const ::mldp_pvxs_driver::config::Config& root);
```

### `src/controller/MLDPPVXSControllerConfig.cpp`

Add `parseProcessors()` — same pattern as `parseReaders()`:
- Iterate `processors:` YAML list.
- Each entry has a `type` key. Strip it, push `{type, entry_node}` to `processorEntries_`.
- Call `parseProcessors(root)` from `parse()`.

### `include/controller/MLDPPVXSController.h`

Add member:
```cpp
std::vector<processor::IChannelProcessorUPtr> processors_;
```

Add include for `<processor/IChannelProcessor.h>`.

### `src/controller/MLDPPVXSController.cpp`

In `start()` — after writers are built, before route table:
```cpp
for (auto& [type, cfg] : config_.processorEntries()) {
    auto batch = processor::ChannelProcessorFactory::create(type, cfg, shared_from_this(), metrics_);
    processors_.insert(processors_.end(),
        std::make_move_iterator(batch.begin()),
        std::make_move_iterator(batch.end()));
}
for (auto& p : processors_) p->start();
```

Register processor names as known "readers" for route table (processor name is used as `reader_name`
in emitted batches):
```cpp
for (const auto& p : processors_)
    known_readers.insert(p->outputReaderName());
```

Register processors in route table as writers (they receive input batches):
```cpp
for (const auto& p : processors_) {
    known_writers.insert(p->name());
    // Their input sources are already filtered by route table acceptsSource()
    // via include_patterns populated from inputSourceNames().
    // Add explicit source-filter routes for each processor.
}
```

In `push()` fan-out loop — include `processors_` alongside `writers_`:
```cpp
// After existing writers loop, add identical loop for processors_:
for (std::size_t i = 0; i < processors_.size(); ++i) {
    if (!route_table_.accepts(processors_[i]->name(), batch_values.reader_name)) continue;
    if (!route_table_.acceptsSource(processors_[i]->name(), rootSource)) continue;
    if (!processors_[i]->acceptsPayload(batch_values.payload)) continue;
    // submit task same as writers
}
```

In `stop()` — stop processors before writers:
```cpp
for (auto& p : processors_) p->stop();
processors_.clear();
```

> **Route wiring for processors**: Processors need routes that match their `inputSourceNames()`.
> The simplest approach for Phase 1: add a wildcard route entry per processor automatically at startup
> (or require explicit `routes:` entries in YAML — the latter is cleaner and already how writers work).
> Use explicit routes for now. Document in YAML example.

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/impl/LinearTransformAlgorithm.cpp
```

---

## Test Files

### `test/processor/LinearTransformAlgorithmTest.cpp`

| Test name | Scenario |
|---|---|
| `ComputesTwoSourceLinearCombination` | 2 sources, coeffs [2.0, -1.0], bias 0.5 → result = 2*x - y + 0.5 |
| `SingleSource` | 1 source, coeff [1.0] → result = x |
| `DefaultBias` | no bias in config → result = weighted sum only |
| `OutputSourceSetCorrectly` | output batch root_source_name == configured output-source |
| `OutputColumnNameInBatch` | output DataBatch has column with configured output-column name |
| `MissingOutputSourceThrows` | no output-source in cfg → configure() throws |

### `test/controller/mldppvxs_controller_processor_integration_test.cpp`

Full pipeline test (no real EPICS needed — push directly to bus):

```yaml
controllers:
  - name: test
    processors:
      - type: linear-transform
        name: linear-proc
        sources:
          - SRC:A
        alignment: latest-value
        trigger: any-update
        output-source: VIRTUAL:LINEAR:OUT
        coefficients: [1.0]
        bias: 0.0
        output-column: val
    writers:
      - type: mldp-pv-metadata   # or any writer that records batches
        ...
    routing:
      routes:
        - reader: test-reader
          writer: linear-proc
        - reader: linear-proc
          writer: metadata-writer
```

Test: push `TimeSeriesPayload` with `root_source_name="SRC:A"` and `reader_name="test-reader"` directly
to controller. Assert writer receives batch with `reader_name="linear-proc"` and
`root_source_name="VIRTUAL:LINEAR:OUT"`.

Add both test files to CMakeLists main test target.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R "LinearTransform|ControllerProcessor" -V
```

## Done Criteria

- `LinearTransformAlgorithmTest` — all 6 cases pass.
- Integration test passes: virtual PV reaches writer with correct reader/source names.
- `processorEntries()` returns empty list when `processors:` absent (no regression in existing controller tests).
- All existing tests pass.
