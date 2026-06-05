# Step 11 — Python Processor (PythonAlgorithm + PythonScriptDirectoryLoader)

## Goal

Add `type: python-processor` bulk-loader: scans a script directory, loads each `.py` file
as a `PythonAlgorithm`, wraps in `ChannelProcessor`. Gated by `BUILD_PYTHON_PROCESSOR=ON`.

## Depends On

Steps 01–06 (all core infrastructure).

---

## Files to Create

### `include/processor/impl/PythonAlgorithm.h`

```cpp
#pragma once
#ifdef BUILD_PYTHON_PROCESSOR

#include <processor/IAlgorithm.h>
// Forward-declare PyObject to avoid Python.h in header
struct _object;
using PyObject = _object;

namespace mldp_pvxs_driver::processor {

class PythonAlgorithm final : public IAlgorithm {
public:
    // module: borrowed reference from PythonScriptDirectoryLoader; caller owns lifetime.
    // GIL must be held when constructing.
    explicit PythonAlgorithm(PyObject* module);
    ~PythonAlgorithm() override;

    void configure(const config::Config& cfg) override;
    // Reads config["output_sources"] (list) or config["output_source"] (string) from Python module.
    // Throws std::runtime_error if neither present.

    std::vector<std::string>     outputSources() const noexcept override;
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType()  const noexcept override { return "python"; }
    void        reset()          noexcept override;

private:
    // Build AlgorithmOutput from a _MldpPayload Python object.
    // Returns nullopt on type error (logs warning).
    std::optional<AlgorithmOutput> payloadFromPyObject(PyObject* obj) const;

    PyObject*                module_;         // borrowed
    PyObject*                compute_fn_;     // borrowed from module
    std::vector<std::string> output_sources_;
};

} // namespace

#endif // BUILD_PYTHON_PROCESSOR
```

### `src/processor/impl/PythonAlgorithm.cpp`

**CPython embedding strategy**:
- Include `<Python.h>` only in this `.cpp`.
- `Py_Initialize()` is called once by `PythonScriptDirectoryLoader::load()` before constructing any `PythonAlgorithm`.
- Each `PythonAlgorithm` holds a `PyObject*` module reference. Module lifetime = algorithm lifetime.
- GIL management: each `compute()` call acquires GIL via `PyGILState_Ensure()` + `PyGILState_Release()`.

**`mldp` module** injected into `sys.modules` before any script imports it:

```python
# Injected pure-Python module (as a C string literal, exec'd into sys.modules)
_MLDP_MODULE_SOURCE = R"py(
from dataclasses import dataclass
from typing import Any

@dataclass
class _MldpPayload:
    mldp_type: str   # "timeseries" | "source_metadata" | "configuration" | "configuration_activation"
    source: str
    data: Any        # dict for timeseries/metadata/config, None for activation

def timeseries(source, columns: dict) -> _MldpPayload:
    return _MldpPayload("timeseries", source, columns)

def source_metadata(source, **kwargs) -> _MldpPayload:
    return _MldpPayload("source_metadata", source, kwargs)

def configuration(source, **kwargs) -> _MldpPayload:
    return _MldpPayload("configuration", source, kwargs)

def configuration_activation(source) -> _MldpPayload:
    return _MldpPayload("configuration_activation", source, None)
)py";
```

Register via `PyRun_String` into a new module dict, then `PyImport_AddModule("mldp")`.

**`configure(cfg)`**:
- Not called with a `config::Config` object — Python scripts carry their own config dict.
- `configure()` reads `config["output_sources"]` or `config["output_source"]` **from the Python module's
  `config` dict** (not from a C++ `Config`). Use `PyDict_GetItemString` on the module's `config` attribute.
- Implementation: acquire GIL, `PyObject_GetAttrString(module_, "config")`, extract output keys.
- Normalize to `vector<string>` stored in `output_sources_`.

> **Note on `configure(const config::Config&)` signature**: The `IAlgorithm` interface takes
> `const config::Config& cfg`. For `PythonAlgorithm`, this argument is unused (config read from
> Python module's `config` dict). Pass a dummy empty `Config` when constructing via the loader.

**`compute(snapshot)`**:
1. Acquire GIL (`PyGILState_STATE gstate = PyGILState_Ensure();`).
2. Build Python `dict`:
   - For each `(source, batch)` in `snapshot.channels`: key=source string, value=latest scalar float.
   - Add `"reference_time"` key = `float(epoch_sec + ns * 1e-9)`.
3. Call `PyObject_CallFunctionObjArgs(compute_fn_, py_dict, NULL)`.
4. On Python exception: log traceback via `PyErr_Print()`, release GIL, return empty.
5. Inspect return:
   - Single `_MldpPayload`: one `AlgorithmOutput`.
   - `list`: iterate, one `AlgorithmOutput` per entry (skip non-`_MldpPayload` with warning).
6. Release GIL (`PyGILState_Release(gstate)`).
7. Return outputs.

**`payloadFromPyObject(obj)`**:
- Read `obj.mldp_type` (string), `obj.source` (string), `obj.data` (dict or None).
- `"timeseries"`: build `TimeSeriesPayload{ .root_source_name=source, .frames={DataBatch with columns from data dict} }`.
  Each key in `data` dict = column name (string), value = float → `DataColumn{ name, vector<double>{val} }`.
  Timestamp from `data["from"]` / `data["to"]` if present, else `snapshot.reference_time`.
- `"source_metadata"`: build `SourceMetadataPayload{ .root_source_name=source, .sources={ {source, SourceMetadataEntry{...}} } }`.
  Map `kwargs` keys to `SourceMetadataEntry` fields (`description`, `units`→`attributes["units"]`, etc.).
- `"configuration"`: build `ConfigurationPayload{ .root_source_name=source, ... }`.
- `"configuration_activation"`: build `ConfigurationActivationPayload{ .configuration_name=source }`.
- Unknown type: log warning, return `nullopt`.

**`reset()`**: no state in base algorithm. Sub-state is in Python module globals (caller responsibility).

---

### `include/processor/PythonScriptDirectoryLoader.h`

```cpp
#pragma once
#ifdef BUILD_PYTHON_PROCESSOR

#include <processor/IChannelProcessor.h>
#include <metrics/Metrics.h>
#include <util/bus/IDataBus.h>
#include <filesystem>
#include <memory>
#include <vector>

namespace mldp_pvxs_driver::processor {

class PythonScriptDirectoryLoader {
public:
    static std::vector<IChannelProcessorUPtr> load(
        const std::filesystem::path&         script_dir,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics>    metrics);
};

} // namespace

#endif // BUILD_PYTHON_PROCESSOR
```

### `src/processor/PythonScriptDirectoryLoader.cpp`

**`load()` sequence**:
1. `Py_Initialize()` if not already initialized (`Py_IsInitialized()`).
2. Register `mldp` module in `sys.modules` once (guard with static bool).
3. Scan `script_dir` for `*.py` files (`std::filesystem::directory_iterator`).
4. Per file:
   a. Compute module name from filename stem (`std::filesystem::path::stem()`).
   b. Add `script_dir` to `sys.path` (once, before first import).
   c. Import module: `PyImport_ImportModule(module_name)`.
   d. Read `config` attribute: `PyObject_GetAttrString(module, "config")` → Python dict.
   e. Extract base fields: `name`, `sources`, `alignment`, `trigger` from dict.
      Build `config::Config` fragment and construct `MLDPChannelProcessorConfig`.
   f. Construct `PythonAlgorithm(module)`. Call `algorithm->configure(empty_cfg)` (reads output_sources from module).
   g. `ChannelProcessor(proc_config, std::move(algorithm), bus, metrics)`.
   h. On any exception: log warning + `PyErr_Print()`, skip file.
5. Release GIL before returning.

Config dict → `config::Config` fragment: use `Config::fromMap()` or manually build YAML string
from Python dict values and parse. Check existing `config::Config` API for the cleanest approach.

**Registering the factory**:
In `PythonScriptDirectoryLoader.cpp`:
```cpp
static bool _reg_python_processor =
    ChannelProcessorFactory::registerType(
        "python-processor",
        [](const config::Config& cfg,
           std::shared_ptr<util::bus::IDataBus> bus,
           std::shared_ptr<metrics::Metrics> metrics)
            -> std::vector<IChannelProcessorUPtr>
        {
            auto dir = cfg["script-dir"].as<std::string>();
            return PythonScriptDirectoryLoader::load(dir, bus, metrics);
        });
```

---

## CMake Changes

```cmake
option(BUILD_PYTHON_PROCESSOR "Build Python script processor (requires CPython 3.x)" OFF)

if(BUILD_PYTHON_PROCESSOR)
    find_package(Python3 REQUIRED COMPONENTS Development)
    target_sources(lib${PROJECT_NAME} PRIVATE
        src/processor/PythonScriptDirectoryLoader.cpp
        src/processor/impl/PythonAlgorithm.cpp)
    target_link_libraries(lib${PROJECT_NAME} PRIVATE Python3::Python)
    target_compile_definitions(lib${PROJECT_NAME} PRIVATE BUILD_PYTHON_PROCESSOR=1)
endif()
```

---

## Test Files

### `test/processor/PythonAlgorithmTest.cpp`

All tests gated `#ifdef BUILD_PYTHON_PROCESSOR`.

| Test name | Scenario |
|---|---|
| `ComputeTimeseries` | script returns `mldp.timeseries(...)` → `TimeSeriesPayload` with correct columns |
| `ComputeSourceMetadata` | script returns `mldp.source_metadata(...)` → `SourceMetadataPayload` |
| `ComputeList` | script returns list of 2 mldp objects → 2 `AlgorithmOutput` entries |
| `ComputeEmpty` | script `compute()` returns empty list → 0 outputs, no crash |
| `ComputePythonException` | script raises exception → 0 outputs, no crash, warning logged |
| `OutputSourcesFromConfigDict` | module `config["output_sources"] = ["VIRTUAL:X"]` → `outputSources()` returns it |
| `OutputSourceSingularCompat` | module `config["output_source"] = "VIRTUAL:X"` → `outputSources()` returns `["VIRTUAL:X"]` |
| `MissingOutputSourceThrows` | no output_source(s) in config → `configure()` throws |

Use inline Python scripts embedded as string literals in the test (no file I/O needed):
```cpp
// In test setup:
Py_Initialize();
// register mldp module...
PyRun_SimpleString(R"py(
import sys, types
# minimal mldp stub
...)py");
PyObject* module = PyImport_AddModule("test_script");
// ... inject compute function ...
```

### `test/processor/PythonScriptDirectoryLoaderTest.cpp`

All tests gated `#ifdef BUILD_PYTHON_PROCESSOR`.

| Test name | Scenario |
|---|---|
| `LoadsValidScript` | write valid `.py` to temp dir → returns 1 processor |
| `SkipsInvalidScript` | 1 valid + 1 invalid `.py` → returns 1 processor, no crash |
| `EmptyDirectory` | empty dir → returns empty vector |
| `ScriptNameBecomesProcessorName` | `config["name"] = "my-proc"` → `processor->name() == "my-proc"` |
| `OutputSourcesWired` | `config["output_sources"] = ["VIRTUAL:X"]` → `processor->outputSourceNames() == ["VIRTUAL:X"]` |

Use `std::filesystem::temp_directory_path()` + `mkdtemp`-style temp dirs; clean up in test teardown.

Add conditionally to CMakeLists:
```cmake
if(BUILD_PYTHON_PROCESSOR)
    target_sources(mldp_pvxs_driver_test PRIVATE
        test/processor/PythonAlgorithmTest.cpp
        test/processor/PythonScriptDirectoryLoaderTest.cpp)
    target_compile_definitions(mldp_pvxs_driver_test PRIVATE BUILD_PYTHON_PROCESSOR=1)
endif()
```

---

## Open Questions (carry forward from python-processor-plan.md)

These are deferred but should be decided before shipping:
1. `Py_Finalize()` on shutdown: skip for v1 (embedded interpreter lifetime = process lifetime).
2. virtualenv support: `virtualenv-path` config key, added to `sys.path` before imports.
3. Max compute time: not implemented in v1.
4. NumPy arrays: not in v1 (scalar floats only in `mldp.timeseries` data dict).

---

## Verification

```bash
# With Python enabled:
cmake -DBUILD_PYTHON_PROCESSOR=ON -B build_test ...
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -R "Python" -V

# Default build must pass unchanged:
cmake -DBUILD_PYTHON_PROCESSOR=OFF -B build_test ...
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -V
```

## Done Criteria

- All 8 PythonAlgorithmTest + 5 PythonScriptDirectoryLoaderTest pass when `BUILD_PYTHON_PROCESSOR=ON`.
- Default build (OFF) passes all tests unchanged.
- `mldp.timeseries` → `TimeSeriesPayload.root_source_name` set correctly.
- Integration: Python script in temp dir → processor registered → bus receives virtual PV batch.
