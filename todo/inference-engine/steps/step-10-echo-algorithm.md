# Step 10 — EchoAlgorithm (cmake-gated, test/debug only)

## Goal

Add `EchoAlgorithm` behind `BUILD_ECHO_PROCESSOR=ON` CMake option.
Pass-through algorithm: re-emits the latest value of the first source unchanged.
Useful for smoke-testing the pipeline without a real computation.

## Depends On

Steps 01–06.

---

## Files to Create

### `include/processor/impl/EchoAlgorithm.h`

```cpp
#pragma once
#ifdef BUILD_ECHO_PROCESSOR

#include <processor/IAlgorithm.h>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class EchoAlgorithm final : public IAlgorithm {
public:
    void configure(const config::Config& cfg) override;
    // Reads from cfg:
    //   output-source: string (optional — auto-derived as sources[0]+"-echo" if absent)
    //   sources: list (read to auto-derive output name if output-source absent)

    std::vector<std::string>     outputSources() const noexcept override { return {output_source_}; }
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType()  const noexcept override { return "echo"; }

private:
    std::string output_source_;
};

} // namespace

#endif // BUILD_ECHO_PROCESSOR
```

### `src/processor/impl/EchoAlgorithm.cpp`

**`configure(cfg)`**:
- Try reading `output-source`. If absent or empty, read `sources[0]` and append `"-echo"`.
- If both absent, use `"VIRTUAL:ECHO:OUT"`.

**`compute(snapshot)`**:
- Take first channel from `snapshot.channels` (any order, use `begin()`).
- Emit `TimeSeriesPayload` with `root_source_name = output_source_`, frames = copy of that channel's `DataBatch`.
- Log each value at DEBUG level.
- If snapshot empty, return empty vector.

Place `REGISTER_ALGORITHM("echo", EchoAlgorithm)` in `.cpp` (inside `#ifdef BUILD_ECHO_PROCESSOR`).

---

## CMake Changes

```cmake
option(BUILD_ECHO_PROCESSOR "Build echo pass-through processor (test/debug only)" OFF)

if(BUILD_ECHO_PROCESSOR)
    target_sources(lib${PROJECT_NAME} PRIVATE
        src/processor/impl/EchoAlgorithm.cpp)
    target_compile_definitions(lib${PROJECT_NAME} PRIVATE BUILD_ECHO_PROCESSOR=1)
endif()
```

---

## Test File

### `test/processor/EchoAlgorithmTest.cpp`

Guard entire file with `#ifdef BUILD_ECHO_PROCESSOR` / `#endif`.

| Test name | Scenario |
|---|---|
| `ExplicitOutputSource` | output-source in cfg → used as-is |
| `AutoDerivedOutputSource` | no output-source, source="BPM:X" → output="BPM:X-echo" |
| `EmptySnapshotReturnsEmpty` | no channels → returns empty vector |
| `PassesThroughValues` | push DataBatch with value 42.0 → output DataBatch has same value |

Add test conditionally to CMakeLists:
```cmake
if(BUILD_ECHO_PROCESSOR)
    target_sources(mldp_pvxs_driver_test PRIVATE
        test/processor/EchoAlgorithmTest.cpp)
    target_compile_definitions(mldp_pvxs_driver_test PRIVATE BUILD_ECHO_PROCESSOR=1)
endif()
```

---

## Verification

```bash
# With echo enabled:
cmake -DBUILD_ECHO_PROCESSOR=ON -B build_test ...
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R Echo -V

# Without echo — default build must still pass:
cmake -DBUILD_ECHO_PROCESSOR=OFF -B build_test ...
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -V
```

## Done Criteria

- Echo tests pass when `BUILD_ECHO_PROCESSOR=ON`.
- All tests pass when `BUILD_ECHO_PROCESSOR=OFF` (default).
- No existing tests broken.
