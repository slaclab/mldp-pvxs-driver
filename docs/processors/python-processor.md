# Python Processor

> **Related:** [Architecture Overview](../reference/architecture.md#channel-processor-layer) | [Configuration Reference](../guides/configuration.md#processors-block)

The `python-processor` type loads Python scripts from a directory and instantiates one `ChannelProcessor` per valid script. Each processor runs an independent Python algorithm that consumes aligned input source snapshots and publishes virtual output sources back onto the data bus.

---

## Build Gate

Requires `-DBUILD_PYTHON_PROCESSOR=ON` (CMake default: **ON**). When disabled, no Python processor files are compiled and the `python-processor` factory type is not registered.

**Enable in devcontainer (default):**

```bash
cmake -S . -B build -DMLDP_PVXS_DRIVER_TESTS=ON -DBUILD_PYTHON_PROCESSOR=ON
```

**Disable:**

```bash
cmake -S . -B build -DBUILD_PYTHON_PROCESSOR=OFF
```

---

## How It Works

```
script-dir/
├── normalize.py       → ChannelProcessor("normalize", algorithm=PythonAlgorithm)
├── quality_flag.py    → ChannelProcessor("quality_flag", algorithm=PythonAlgorithm)
└── bad_script.py      → skipped (warning logged)
```

`PythonScriptDirectoryLoader::load()`:

1. Calls `Py_Initialize()` if CPython is not running.
2. Registers an embedded `mldp` helper module (once per process).
3. Adds `script-dir` to `sys.path`.
4. Sorts and iterates `.py` files alphabetically.
5. For each file: imports the module, reads `config`, constructs `ChannelProcessor`. Failures skip the script without aborting.

Each `PythonAlgorithm` holds a reference to its module and calls `module.compute(snapshot)` on every trigger.

---

## Script Contract

### Required Exports

| Symbol | Type | Description |
|--------|------|-------------|
| `config` | `dict` | Processor identity and input/output declaration. |
| `compute` | callable | Algorithm entry point called on each aligned snapshot. |

### `config` Dictionary

| Key | Type | Default | Required | Description |
|-----|------|---------|----------|-------------|
| `name` | str | — | **Yes** | Processor instance name (appears in routing and metrics). |
| `sources` | list[str] | — | **Yes** | Input PV/source names consumed by the processor. |
| `alignment` | str | `"latest-value"` | No | `"latest-value"` — use most recent value per source; `"interpolate"` — interpolate to reference time. |
| `trigger` | str | `"any-update"` | No | `"any-update"` — fire when any input updates; `"all-updated"` — fire only when all inputs have updated; `"interval"` — fire on fixed timer. |
| `trigger-interval-sec` | float | — | When `trigger="interval"` | Timer period in seconds. |
| `output_source` | str | — | One of these | Single virtual output source name. |
| `output_sources` | list[str] | — | One of these | Multiple virtual output source names. |

### `compute(snapshot)` Signature

```python
def compute(snapshot: dict) -> mldp_payload | list[mldp_payload] | None:
    ...
```

`snapshot` is a `dict` mapping source names to their latest scalar `float` values, plus `"reference_time"` (Unix timestamp as `float`). Return `None` or `[]` to emit nothing.

---

## `mldp` Helper Module

An embedded Python module (`mldp`) is automatically available in every processor script. No `pip install` required.

```python
import mldp
```

### `mldp.timeseries(source, columns)`

Emit a time-series data payload.

```python
mldp.timeseries("VIRTUAL:NORM:X", {"value": 3.14, "raw": 6.28})
```

| Arg | Type | Description |
|-----|------|-------------|
| `source` | str | Virtual output source name. |
| `columns` | dict | Column name → scalar float value. Optional: include `"from"` and/or `"to"` keys (Unix timestamps as float) to set the batch time window. |

### `mldp.source_metadata(source, **kwargs)`

Emit source attribute metadata (description, units, custom attributes).

```python
mldp.source_metadata("VIRTUAL:NORM:X", description="Normalized X position", units="mm")
```

### `mldp.configuration(source, **kwargs)`

Emit a configuration object.

```python
mldp.configuration("MY:CONFIG", category="normalization", description="Gain calibration v2")
```

| Kwarg | Description |
|-------|-------------|
| `category` | Configuration category string. |
| `description` | Human-readable description. |
| `modified_by` | Author identifier. |
| `parent_configuration_name` | Parent config name. |
| Any additional kwargs | Stored as `attributes` key/value pairs. |

### `mldp.configuration_activation(source)`

Mark a configuration as active at the current reference time.

```python
mldp.configuration_activation("MY:CONFIG")
```

---

## Complete Script Examples

### Scalar transform

```python
import mldp

config = {
    "name": "gain-correction",
    "sources": ["BPM:X:RAW"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "BPM:X:CORRECTED",
}

GAIN = 1.0423
OFFSET = -0.012

def compute(snapshot):
    raw = snapshot.get("BPM:X:RAW")
    if raw is None:
        return None
    return mldp.timeseries("BPM:X:CORRECTED", {"value": raw * GAIN + OFFSET})
```

### Multi-source ratio with metadata

```python
import mldp

config = {
    "name": "energy-ratio",
    "sources": ["LINAC:ENERGY:1", "LINAC:ENERGY:2"],
    "alignment": "latest-value",
    "trigger": "all-updated",
    "output_sources": ["VIRTUAL:ENERGY:RATIO", "VIRTUAL:ENERGY:RATIO:META"],
}

def compute(snapshot):
    e1 = snapshot.get("LINAC:ENERGY:1", 0.0)
    e2 = snapshot.get("LINAC:ENERGY:2", 1.0)
    if e2 == 0.0:
        return []
    ratio = e1 / e2
    return [
        mldp.timeseries("VIRTUAL:ENERGY:RATIO", {"value": ratio}),
        mldp.source_metadata("VIRTUAL:ENERGY:RATIO:META", description="E1/E2 ratio", units="dimensionless"),
    ]
```

### Interval trigger (periodic heartbeat)

```python
import mldp
import time

config = {
    "name": "watchdog",
    "sources": ["SYS:STATUS"],
    "alignment": "latest-value",
    "trigger": "interval",
    "trigger-interval-sec": 5.0,
    "output_source": "VIRTUAL:WATCHDOG",
}

def compute(snapshot):
    status = snapshot.get("SYS:STATUS", 0.0)
    return mldp.timeseries("VIRTUAL:WATCHDOG", {"alive": 1.0, "last_status": status})
```

---

## Wiring with Routing

Processors appear as both **writer targets** (for input routing) and **reader origins** (for output routing).

```yaml
processors:
  - type: python-processor
    script-dir: /opt/scripts/processors

routing:
  gain-correction:           # processor name from config["name"]
    from: [pvxs_reader]
    include:
      - "BPM:X:*"
  mldp_main:
    from: [gain-correction]  # processor's virtual output feeds the writer
    include:
      - "BPM:X:CORRECTED"
```

---

## Error Handling

| Condition | Behavior |
|-----------|----------|
| Script does not exist or fails to import | Skipped with `WARN` log; other scripts proceed |
| `config` is not a `dict` | Skipped |
| `config` missing `name` or `sources` | Skipped |
| `config` missing both `output_source` and `output_sources` | `configure()` throws; processor not created |
| `compute()` raises an exception | Exception printed to stderr; empty output returned (processor continues) |
| `compute()` returns an object with unknown `mldp_type` | Warning logged; object skipped |

---

## Implementation Notes

- CPython GIL is acquired via `PyGILState_Ensure()` on every `compute()` call — processor threads are safe.
- Each `PythonAlgorithm` holds `Py_INCREF`'d references to its module and `compute` callable; both are released in the destructor.
- `Py_Initialize()` is called once; subsequent loads skip re-initialization.
- The embedded `mldp` module is registered once per process via `PyImport_AddModule`.
- `script-dir` is added to `sys.path` once, so scripts can import local helper modules from the same directory.
