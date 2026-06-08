# Step 02 — IAlgorithm Interface + MLDPChannelProcessorConfig

## Goal

Define the algorithm contract and the base config parser.
Two headers + two cpp files + unit tests for the config parser.

## Depends On

Step 01 (AlignmentPolicy, TriggerPolicy, AlignedSnapshot, AlgorithmOutput).

---

## Files to Create

### `include/processor/IAlgorithm.h`

```cpp
#pragma once
#include <processor/AlgorithmOutput.h>
#include <processor/AlignedSnapshot.h>
#include <config/Config.h>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class IAlgorithm {
public:
    virtual ~IAlgorithm() = default;

    // Read ALL algorithm-specific config (output source names, weights, paths, ...).
    // Base config (sources, alignment, trigger) already parsed — do NOT re-read here.
    virtual void configure(const config::Config& cfg) = 0;

    // Declared output virtual PV names — stable after configure() returns.
    virtual std::vector<std::string> outputSources() const noexcept = 0;

    // Pure compute: no bus access, no threading.
    virtual std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) = 0;

    virtual std::string algorithmType() const noexcept = 0;
};

using IAlgorithmUPtr = std::unique_ptr<IAlgorithm>;

} // namespace
```

---

### `include/processor/MLDPChannelProcessorConfig.h`

```cpp
#pragma once
#include <processor/AlignmentPolicy.h>
#include <processor/TriggerPolicy.h>
#include <config/Config.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class MLDPChannelProcessorConfig {
public:
    struct Error : public std::runtime_error { using std::runtime_error::runtime_error; };

    explicit MLDPChannelProcessorConfig(const config::Config& cfg);

    const std::string&              name()               const noexcept;
    const std::vector<std::string>& sources()            const noexcept;
    AlignmentPolicy                 alignment()          const noexcept;
    TriggerPolicy                   trigger()            const noexcept;
    double                          triggerIntervalSec() const noexcept;

private:
    std::string              name_;
    std::vector<std::string> sources_;
    AlignmentPolicy          alignment_{AlignmentPolicy::LatestValue};
    TriggerPolicy            trigger_{TriggerPolicy::AnyUpdate};
    double                   trigger_interval_sec_{0.0};
};

} // namespace
```

### `src/processor/MLDPChannelProcessorConfig.cpp`

Parse logic:
- `name` — required, non-empty string. Throw `Error` if absent or empty.
- `sources` — required, non-empty list of strings. Throw `Error` if absent or empty.
- `alignment` — optional string:
  - `"latest-value"` → `LatestValue` (default)
  - `"all-updated"`  → `AllUpdated`
  - `"interpolate"`  → `Interpolate`
  - Unknown value → throw `Error`
- `trigger` — optional string:
  - `"any-update"`  → `AnyUpdate` (default)
  - `"all-updated"` → `AllUpdated`
  - `"interval"`    → `Interval`
  - Unknown value → throw `Error`
- `trigger-interval-sec` — required (and must be > 0) when trigger = `Interval`. Ignored otherwise.
- `output-source` / `output-sources` — NOT read here. Algorithm owns those keys.

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/MLDPChannelProcessorConfig.cpp
```

---

## Test File

### `test/processor/MLDPChannelProcessorConfigTest.cpp`

Test cases (use GoogleTest, match existing test style):

| Test name | Input | Expected |
|---|---|---|
| `ParsesMinimalValid` | name + sources only | defaults: LatestValue, AnyUpdate, interval=0 |
| `ParsesAllAlignment` | each alignment string | correct enum |
| `ParsesAllTrigger` | each trigger string | correct enum |
| `IntervalRequiresPositiveSec` | trigger=interval, sec=0 | throws Error |
| `IntervalRequiresPositiveSec_Absent` | trigger=interval, no sec key | throws Error |
| `EmptyNameThrows` | name="" | throws Error |
| `EmptySourcesThrows` | sources=[] | throws Error |
| `MissingSourcesThrows` | no sources key | throws Error |
| `UnknownAlignmentThrows` | alignment="bogus" | throws Error |
| `UnknownTriggerThrows` | trigger="bogus" | throws Error |
| `OutputSourceKeyIgnored` | has output-source key | parses OK, key not in accessor |

Add test cpp to CMakeLists `mldp_pvxs_driver_test` target.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R MLDPChannelProcessorConfig -V
```

## Done Criteria

- `IAlgorithm` header compiles.
- `MLDPChannelProcessorConfig` unit tests pass (all 11 cases).
- No existing tests broken.
