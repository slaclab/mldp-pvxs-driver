# Python Processor — Implementation Plan

> Depends on: core infrastructure from [implementation-plan.md](./implementation-plan.md)

---

## Overview

`type: python-processor` is a bulk-loader processor type.  The factory delegates to
`PythonScriptDirectoryLoader` which scans a directory of `*.py` files and returns
one `ChannelProcessor` per valid script.

**Dependency**: CPython 3.x (~3MB).  Gated by `BUILD_PYTHON_PROCESSOR=ON` cmake option.

---

## Script Layout — One File, Self-Describing

```python
# /etc/mldp/python-processors/anomaly_detector.py
# mldp module injected into sys.modules by PythonAlgorithm bridge before this module loads.

import mldp

config = {
    "name":           "py-anomaly-detector",
    "sources":        ["BPM:LI21:201:X", "BPM:LI21:201:Y"],
    "output_sources": ["VIRTUAL:PY:ANOMALY:SCORE"],   # ALL virtual PVs — declared for route wiring
    # "output_source": "..."                           # singular compat alias for single-PV scripts
    "alignment":      "latest-value",
    "trigger":        "all-updated",
}

_history     = []
_initialized = False

def compute(snapshot: dict) -> list:
    global _initialized
    results = []

    if not _initialized:
        _initialized = True
        results.append(mldp.source_metadata("VIRTUAL:PY:ANOMALY:SCORE",
            units="mm", description="RMS beam anomaly score"))

    x = snapshot["BPM:LI21:201:X"]
    y = snapshot["BPM:LI21:201:Y"]
    _history.append((x*x + y*y) ** 0.5)
    if len(_history) > 10:
        _history.pop(0)

    results.append(mldp.timeseries("VIRTUAL:PY:ANOMALY:SCORE", {
        "from": snapshot["reference_time"],
        "to":   snapshot["reference_time"],
        "score": sum(_history) / len(_history),
        "bpm_x": x,
        "bpm_y": y,
    }))
    return results
```

---

## Script Config Dict Fields

| Field | Required | Notes |
|---|---|---|
| `config["name"]` | yes | Processor name + `reader_name` in emitted `EventBatch` |
| `config["sources"]` | yes | List of input `root_source` strings |
| `config["alignment"]` | no | `"latest-value"` (default) \| `"all-updated"` \| `"interpolate"` |
| `config["trigger"]` | no | `"any-update"` (default) \| `"all-updated"` \| `"interval"` |
| `config["output_sources"]` | yes* | List of ALL virtual PVs emitted — used for route wiring at startup |
| `config["output_source"]` | yes* | Singular compat alias — equivalent to `output_sources = [value]` |

*One of `output_source` or `output_sources` required.

---

## `mldp` Module API

Injected into `sys.modules` by `PythonAlgorithm` bridge, importable as `import mldp`:

| Call | `BatchPayload` variant | Data fields |
|---|---|---|
| `mldp.timeseries(source, cols: dict)` | `TimeSeriesPayload` | column name -> `float` |
| `mldp.source_metadata(source, **kwargs)` | `SourceMetadataPayload` | `units`, `description`, ... |
| `mldp.configuration(source, **kwargs)` | `ConfigurationPayload` | arbitrary key-value |
| `mldp.configuration_activation(source)` | `ConfigurationActivationPayload` | (no data) |

Each `mldp.*` call returns a `_MldpPayload` dataclass `(mldp_type, source, data)`.
The C++ bridge in `PythonAlgorithm::compute()` reads the `mldp_type` attr and constructs the right `BatchPayload` variant.

**`compute(snapshot)` return contract**:
- `snapshot`: Python `dict` keyed by `root_source` string, values = `float`
- Returns one of:
  - **single `mldp.*` object** -> one `AlgorithmOutput`
  - **`list`** -> one `AlgorithmOutput` per entry -> N `bus_->push()` calls
- Bridge warns at runtime if a source name used in `mldp.*` was not declared in `config["output_sources"]`.

---

## `PythonScriptDirectoryLoader`

```
include/processor/PythonScriptDirectoryLoader.h
src/processor/PythonScriptDirectoryLoader.cpp
```

```cpp
class PythonScriptDirectoryLoader {
public:
    // Scans dir for *.py, reads config dict from each, instantiates ChannelProcessor.
    // Returns one processor per valid script file.
    // Called by ChannelProcessorFactory for type "python-processor".
    static std::vector<IChannelProcessorUPtr> load(
        const std::filesystem::path& script_dir,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics> metrics);
};
```

Startup sequence per `.py` file:
1. `Py_Initialize()` (once, global) — embed CPython interpreter
2. Register `mldp` module in `sys.modules` (typed constructors + `_MldpPayload` dataclass)
3. Import the script as a module
4. Read **base** fields from `config` dict: `name`, `sources`, `alignment`, `trigger`
5. Construct `MLDPChannelProcessorConfig` from those base fields only
6. Construct `PythonAlgorithm` with the loaded module; `configure()` reads `config["output_source"]` / `config["output_sources"]`
7. Wrap in `ChannelProcessor(config, python_algorithm, bus, metrics)`
8. On error in any step: log warning + skip file (other scripts continue)

---

## `PythonAlgorithm` Internals

```
include/processor/impl/PythonAlgorithm.h
src/processor/impl/PythonAlgorithm.cpp
```

- Holds `PyObject*` module reference (loaded by `PythonScriptDirectoryLoader`, `mldp` already in `sys.modules`)
- `configure()` reads `config["output_source"]` / `config["output_sources"]`; normalizes to `output_sources_` list
- `outputSources()`: returns `output_sources_` — all declared virtual PVs
- `compute(snapshot)`:
  1. Build Python `dict`: keys = `root_source` strings, values = `PyFloat` scalars
  2. Call module-level `compute(snapshot)` (1 arg, 1 return)
  3. Inspect return value:
     - **`_MldpPayload`**: read `mldp_type` attr -> build `BatchPayload` variant -> one `AlgorithmOutput`
     - **`list`**: iterate entries, read each `mldp_type` -> N `AlgorithmOutput` entries
  4. Return `vector<AlgorithmOutput>`
- GIL acquired for each `compute()` call — thread-safe per instance
- Runtime error in `compute()`: log warning + Python traceback, return empty vector

---

## YAML Configuration

```yaml
controllers:
  - name: main
    processors:
      - type: python-processor           # bulk Python loader
        script-dir: /etc/mldp/python-processors
        # future options: virtualenv-path, max-compute-ms, ...
```

---

## CMake Integration

```cmake
option(BUILD_PYTHON_PROCESSOR "Build Python script processor (requires CPython 3.x)" OFF)

if(BUILD_PYTHON_PROCESSOR)
    find_package(Python3 REQUIRED COMPONENTS Development)
    target_sources(mldp_pvxs_driver PRIVATE
        src/processor/PythonScriptDirectoryLoader.cpp
        src/processor/impl/PythonAlgorithm.cpp)
    target_link_libraries(mldp_pvxs_driver PRIVATE Python3::Python)
    target_compile_definitions(mldp_pvxs_driver PRIVATE BUILD_PYTHON_PROCESSOR=1)
endif()
```

---

## File Layout

```
include/processor/
    PythonScriptDirectoryLoader.h
    impl/
        PythonAlgorithm.h

src/processor/
    PythonScriptDirectoryLoader.cpp
    impl/
        PythonAlgorithm.cpp

test/processor/
    PythonAlgorithmTest.cpp
    PythonScriptDirectoryLoaderTest.cpp
```

---

## Implementation Steps

1. Add CPython 3.x find_package + cmake gate (`BUILD_PYTHON_PROCESSOR`)
2. `PythonAlgorithm` class — holds `PyObject*` module, implements `IAlgorithm`
3. `mldp` Python module registration (C extension or injected pure-Python in `sys.modules`)
4. `PythonScriptDirectoryLoader` — directory scan, per-file load sequence
5. Register `type: python-processor` in `ChannelProcessorFactory`
6. Unit tests: mock scripts in temp dirs, verify compute output
7. Integration test: real Python script -> processor -> bus -> writer

---

## Threading & GIL Considerations

- `Py_Initialize()` called once at process startup (before any processor starts)
- Each `PythonAlgorithm::compute()` acquires GIL via `PyGILState_Ensure()` / `PyGILState_Release()`
- Multiple Python processors share one interpreter but each has isolated module namespace
- No sub-interpreters (complexity not justified for v1)
- If `interval` trigger: worker thread acquires GIL only during compute window

---

## Open Questions

| # | Question |
|---|---|
| 1 | virtualenv support: allow `virtualenv-path` config to add site-packages? |
| 2 | Max compute time: signal-based timeout for long-running Python `compute()`? |
| 3 | NumPy/SciPy: allow array-valued columns in `mldp.timeseries()`? |
| 4 | Hot-reload: watch `script-dir` for changes? (Phase 4+) |
| 5 | `Py_Finalize()` on shutdown: safe given embedded use? Or skip? |
