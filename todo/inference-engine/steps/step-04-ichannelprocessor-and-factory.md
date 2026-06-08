# Step 04 — IChannelProcessor Interface + ChannelProcessorFactory

## Goal

Define the interface the controller will wire, and the factory/macro that algorithm authors
use to register their types. No algorithm implementations yet. No `ChannelProcessor` yet.

## Depends On

Step 02 (IAlgorithm, MLDPChannelProcessorConfig).

---

## Files to Create

### `include/processor/IChannelProcessor.h`

```cpp
#pragma once
#include <writer/IWriter.h>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class IChannelProcessor : public writer::IWriter {
public:
    // reader_name placed in emitted EventBatch (= processor config name)
    virtual const std::string&              outputReaderName()  const noexcept = 0;
    // All virtual PV root_source_names this processor may emit
    virtual std::vector<std::string>        outputSourceNames() const noexcept = 0;
    // All input root_source_names this processor consumes (for route table wiring)
    virtual const std::vector<std::string>& inputSourceNames()  const noexcept = 0;
};

using IChannelProcessorUPtr = std::unique_ptr<IChannelProcessor>;

} // namespace
```

---

### `include/processor/ChannelProcessorFactory.h`

```cpp
#pragma once
#include <processor/IChannelProcessor.h>
#include <processor/MLDPChannelProcessorConfig.h>
#include <processor/IAlgorithm.h>
#include <config/Config.h>
#include <metrics/Metrics.h>
#include <util/bus/IDataBus.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class ChannelProcessorFactory {
public:
    using ProcessorFactory = std::function<
        std::vector<IChannelProcessorUPtr>(
            const config::Config&,
            std::shared_ptr<util::bus::IDataBus>,
            std::shared_ptr<metrics::Metrics>)>;

    // Returns all processors for one config block (1 for single-algorithm types,
    // N for script-dir types).
    // Throws std::runtime_error for unknown type.
    static std::vector<IChannelProcessorUPtr> create(
        const std::string&                   type,
        const config::Config&                cfg,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics>    metrics);

    static bool registerType(const std::string& type, ProcessorFactory f);

private:
    static std::unordered_map<std::string, ProcessorFactory>& registry();
};

} // namespace

// Convenience macro: wraps a single IAlgorithm class into one ChannelProcessor.
// Place at namespace scope in the algorithm's .cpp file.
#define REGISTER_ALGORITHM(type_str, AlgorithmClass)                                         \
    static bool _reg_##AlgorithmClass =                                                      \
        mldp_pvxs_driver::processor::ChannelProcessorFactory::registerType(                  \
            type_str,                                                                        \
            [](const mldp_pvxs_driver::config::Config&                cfg,                  \
               std::shared_ptr<mldp_pvxs_driver::util::bus::IDataBus> bus,                   \
               std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>    metrics)               \
                -> std::vector<mldp_pvxs_driver::processor::IChannelProcessorUPtr>           \
            {                                                                                \
                auto alg = std::make_unique<AlgorithmClass>();                               \
                alg->configure(cfg);                                                         \
                auto proc = std::make_unique<mldp_pvxs_driver::processor::ChannelProcessor>( \
                    mldp_pvxs_driver::processor::MLDPChannelProcessorConfig(cfg),            \
                    std::move(alg), bus, metrics);                                           \
                std::vector<mldp_pvxs_driver::processor::IChannelProcessorUPtr> v;           \
                v.push_back(std::move(proc));                                                \
                return v;                                                                    \
            });
```

> **Note**: `REGISTER_ALGORITHM` references `ChannelProcessor` (forward-declared here).
> Add `#include <processor/ChannelProcessor.h>` before using the macro in algorithm .cpp files.

### `src/processor/ChannelProcessorFactory.cpp`

```cpp
// Static registry map + create() + registerType() implementations.
// create(): lookup type in registry, throw std::runtime_error if not found, delegate to factory fn.
// registry(): returns reference to function-local static map (Meyer's singleton).
```

---

## CMake Changes

Add to `libmldp_pvxs_driver` sources:
```cmake
src/processor/ChannelProcessorFactory.cpp
```

No test file for this step — factory tested implicitly once algorithms are registered (Step 06+).

---

## Verification

```bash
cmake --build build_test --target lib${PROJECT_NAME} 2>&1 | grep -E "^.*error:" | head -20
```

## Done Criteria

- Headers and factory compile cleanly.
- `ChannelProcessorFactory::create("unknown", ...)` throws (can add 1-line test or verify manually).
- No existing tests broken.
