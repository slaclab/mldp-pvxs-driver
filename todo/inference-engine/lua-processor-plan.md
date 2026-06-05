# Lua Processor — Implementation Plan

> Depends on: core infrastructure from [implementation-plan.md](./implementation-plan.md)

---

## Overview

`type: lua-processor` is a bulk-loader processor type.  The factory delegates to
`LuaScriptDirectoryLoader` which scans a directory of `*.lua` files and returns
one `ChannelProcessor` per valid script.

**Dependency**: Lua 5.4 (~200KB).  Always built (no cmake gate).

---

## Script Layout — One File, Self-Describing

```lua
-- /etc/mldp/lua-processors/anomaly_detector.lua
-- LuaAlgorithm bridge injects mldp global table before this file executes.

config = {
    name           = "anomaly-detector",           -- processor name + reader_name in routes
    sources        = { "BPM:LI21:201:X",
                       "BPM:LI21:201:Y" },
    output_sources = { "VIRTUAL:ANOMALY:SCORE" },  -- ALL virtual PVs — declared for route wiring
    -- output_source = "..."                        -- singular compat alias for single-PV scripts
    alignment      = "latest-value",               -- optional, default: latest-value
    trigger        = "all-updated",                -- optional, default: any-update
}

local history     = {}
local initialized = false

function compute(snapshot)
    local results = {}

    -- emit SourceMetadataPayload once on first call so downstream writers know units
    if not initialized then
        initialized = true
        table.insert(results,
            mldp.source_metadata("VIRTUAL:ANOMALY:SCORE", {
                units       = "mm",
                description = "RMS beam anomaly score",
            })
        )
    end

    local x = snapshot["BPM:LI21:201:X"]
    local y = snapshot["BPM:LI21:201:Y"]
    table.insert(history, math.sqrt(x*x + y*y))
    if #history > 10 then table.remove(history, 1) end
    local sum = 0
    for _, v in ipairs(history) do sum = sum + v end

    -- emit TimeSeriesPayload — multiple DataColumns in one batch
    table.insert(results,
        mldp.timeseries("VIRTUAL:ANOMALY:SCORE", {
            from  = snapshot.reference_time,
            to    = snapshot.reference_time,
            score = sum / #history,
            bpm_x = x,
            bpm_y = y,
        })
    )
    return results
end
```

---

## Script Config Table Fields

| Field | Required | Notes |
|---|---|---|
| `config.name` | yes | Processor name + `reader_name` in emitted `EventBatch` |
| `config.sources` | yes | List of input `root_source` strings (base infra field) |
| `config.alignment` | no | `"latest-value"` (default) \| `"all-updated"` \| `"interpolate"` |
| `config.trigger` | no | `"any-update"` (default) \| `"all-updated"` \| `"interval"` |
| `config.output_sources` | yes* | List of ALL virtual PVs emitted — used for route wiring at startup |
| `config.output_source` | yes* | Singular compat alias — equivalent to `output_sources = {value}` |

*One of `output_source` or `output_sources` required.

---

## `mldp` Bridge API

Lua global table injected by `LuaAlgorithm` before the script file executes:

| Call | `BatchPayload` variant | Data fields |
|---|---|---|
| `mldp.timeseries(source, cols_table)` | `TimeSeriesPayload` | column name -> `float` |
| `mldp.source_metadata(source, fields_table)` | `SourceMetadataPayload` | `units`, `description`, ... |
| `mldp.configuration(source, data_table)` | `ConfigurationPayload` | arbitrary key-value |
| `mldp.configuration_activation(source)` | `ConfigurationActivationPayload` | (no data) |

Each `mldp.*` call returns a tagged Lua table `{ _mldp_type=..., _source=..., _data=... }`.
The C++ bridge in `LuaAlgorithm::compute()` reads the tag and constructs the right `BatchPayload` variant.

**`compute(snapshot)` return contract**:
- `snapshot`: Lua table keyed by `root_source` string, values = latest scalar `float`
- Returns one of:
  - **single `mldp.*` object** -> one `AlgorithmOutput`
  - **list** `{mldp.*(), mldp.*(), ...}` -> one `AlgorithmOutput` per entry -> N `bus_->push()` calls
- Bridge warns at runtime if a source name used in `mldp.*` was not declared in `config.output_sources`.

---

## `LuaScriptDirectoryLoader`

```
include/processor/LuaScriptDirectoryLoader.h
src/processor/LuaScriptDirectoryLoader.cpp
```

```cpp
class LuaScriptDirectoryLoader {
public:
    // Scans dir for *.lua, reads config table from each, instantiates ChannelProcessor.
    // Returns one processor per valid script file.
    // Called by ChannelProcessorFactory for type "lua-processor".
    static std::vector<IChannelProcessorUPtr> load(
        const std::filesystem::path& script_dir,
        std::shared_ptr<util::bus::IDataBus> bus,
        std::shared_ptr<metrics::Metrics> metrics);
};
```

Startup sequence per `.lua` file:
1. `luaL_newstate()` — fresh Lua state
2. Register `mldp` global table in the Lua state (typed constructors + tag mechanism)
3. `luaL_dofile(script_path)` — executes the file (defines `config` table + `compute` function)
4. Read **base** fields from `config` table: `name`, `sources`, `alignment`, `trigger`
5. Construct `MLDPChannelProcessorConfig` from those base fields only
6. Construct `LuaAlgorithm` with the already-loaded Lua state; `configure()` reads `config.output_source` / `config.output_sources`
7. Wrap in `ChannelProcessor(config, lua_algorithm, bus, metrics)`
8. On error in any step: log warning + skip file (other scripts continue)

---

## `LuaAlgorithm` Internals

```
include/processor/impl/LuaAlgorithm.h
src/processor/impl/LuaAlgorithm.cpp
```

- Holds `lua_State*` (loaded and initialized by `LuaScriptDirectoryLoader`, `mldp` table already registered)
- `configure()` reads `config.output_source` / `config.output_sources` from global `config` table; normalizes to `output_sources_` list
- `outputSources()`: returns `output_sources_` — all declared virtual PVs
- `compute(snapshot)`:
  1. Push Lua table: keys = `root_source` strings, values = latest scalar `double`
  2. Call global `compute` Lua function (1 arg, 1 return)
  3. Inspect return value:
     - **`mldp.*` tagged table**: read `_mldp_type` -> build `BatchPayload` variant -> one `AlgorithmOutput`
     - **list (array table)**: iterate entries, read each `_mldp_type` -> N `AlgorithmOutput` entries
  4. Return `vector<AlgorithmOutput>`
- Lua state isolated per instance — no shared state between scripts
- Runtime error in `compute()`: log warning, return empty `vector<AlgorithmOutput>` (base skips push)

---

## YAML Configuration

```yaml
controllers:
  - name: main
    processors:
      - type: lua-processor              # bulk Lua loader
        script-dir: /etc/mldp/lua-processors
        # future options: sandbox, max-compute-ms, reload-on-sighup, ...
```

---

## File Layout

```
include/processor/
    LuaScriptDirectoryLoader.h
    impl/
        LuaAlgorithm.h

src/processor/
    LuaScriptDirectoryLoader.cpp
    impl/
        LuaAlgorithm.cpp

test/processor/
    LuaAlgorithmTest.cpp
    LuaScriptDirectoryLoaderTest.cpp
```

---

## Implementation Steps

1. Add Lua 5.4 dependency to CMakeLists.txt
2. `LuaAlgorithm` class — holds `lua_State*`, implements `IAlgorithm`
3. `mldp` bridge table registration (C functions exposed to Lua)
4. `LuaScriptDirectoryLoader` — directory scan, per-file load sequence
5. Register `type: lua-processor` in `ChannelProcessorFactory`
6. Unit tests: mock scripts in temp dirs, verify compute output
7. Integration test: real Lua script -> processor -> bus -> writer

---

## Open Questions

| # | Question |
|---|---|
| 1 | Sandbox level: restrict `os`/`io` libs by default? Configurable? |
| 2 | Max compute time: kill long-running Lua `compute()` via `lua_sethook`? |
| 3 | Hot-reload: watch `script-dir` for changes, reload on SIGHUP? (Phase 4+) |
