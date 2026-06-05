# Step 01 — Enums, AlignedSnapshot, AlgorithmOutput

## Goal

Add the three header-only types that all later steps depend on.
Zero cpp files. Zero new tests. Compiles with existing codebase unchanged.

## Files to Create

### `include/processor/AlignmentPolicy.h`
```cpp
#pragma once
namespace mldp_pvxs_driver::processor {
enum class AlignmentPolicy { LatestValue, AllUpdated, Interpolate };
} // namespace
```

### `include/processor/TriggerPolicy.h`
```cpp
#pragma once
namespace mldp_pvxs_driver::processor {
enum class TriggerPolicy { AnyUpdate, AllUpdated, Interval };
} // namespace
```

### `include/processor/AlignedSnapshot.h`
```cpp
#pragma once
#include <string>
#include <unordered_map>
#include <util/bus/IDataBus.h>

namespace mldp_pvxs_driver::processor {

struct AlignedSnapshot {
    std::unordered_map<std::string, util::bus::DataBatch> channels;  // keyed by root_source_name
    util::bus::BusTimestamp                               reference_time;
};

} // namespace
```

### `include/processor/AlgorithmOutput.h`
```cpp
#pragma once
#include <string>
#include <util/bus/IDataBus.h>

namespace mldp_pvxs_driver::processor {

// One virtual-PV emission from IAlgorithm::compute().
// output_source must be set as the identity field inside payload:
//   TimeSeriesPayload.root_source_name        = output_source
//   SourceMetadataPayload.root_source_name    = output_source
//   ConfigurationPayload.root_source_name     = output_source
//   ConfigurationActivationPayload.configuration_name = output_source
struct AlgorithmOutput {
    std::string             output_source;
    util::bus::BatchPayload payload;
};

} // namespace
```

## CMake Changes

None — header-only.

## Verification

```bash
# Inside devcontainer — should compile with no errors:
cmake --build build_test --target lib${PROJECT_NAME} 2>&1 | grep -E "error:|warning:" | head -20
```

No tests needed: these are trivially-correct enum/struct definitions.

## Done Criteria

- Four headers exist, compile cleanly as part of `libmldp_pvxs_driver`.
- No existing tests broken.
