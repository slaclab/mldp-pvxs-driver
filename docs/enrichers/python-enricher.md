# Python Enricher

> **Back:** [Payload Enrichers](enrichers.md) | **Related:** [Enricher Implementation Guide](enrichers-implementation.md) | [Configuration Reference](../guides/configuration.md#global-enrichers-and-writer-chains)

The Python enricher executes a single `enrich(batch)` function from a `.py` file for each `EventBatch`. It is built only when the driver is configured with `BUILD_PYTHON_PROCESSOR=ON`.

## Build Requirement

```bash
cmake -S . -B build -DBUILD_PYTHON_PROCESSOR=ON
```

When disabled, the `python-enricher` type is not registered. Any configuration referencing it or using an unresolved logical type fails at startup.

## Script Resolution

Three forms are supported, evaluated in this order:

### 1. Logical type via `python-plugin-path`

Set `python-plugin-path` once under `enrichers:`. Any unrecognized `type` that is not a registered C++ enricher is resolved as `<python-plugin-path>/<type>.py`. The module must declare a string `ENRICHER_TYPE` matching the configured `type`.

```yaml
enrichers:
  python-plugin-path: /opt/mldp/enrichers
  tag-payload:
    type: tag_payload       # loads /opt/mldp/enrichers/tag_payload.py
  ts-corrector:
    type: ts_corrector      # loads /opt/mldp/enrichers/ts_corrector.py
```

`python-plugin-path` defaults to `enrichers` relative to the process working directory when not specified.

### 2. Unregistered type with explicit `script-path`

An explicit `script-path` on an unregistered logical type loads that file directly. The module must still declare `ENRICHER_TYPE` matching `type`.

```yaml
enrichers:
  corrector:
    type: ts_corrector
    script-path: /data/scripts/ts_corrector.py   # explicit path; ENRICHER_TYPE must be "ts_corrector"
```

### 3. `type: python-enricher` with explicit `script-path`

Direct form: bypasses all type-matching logic. `script-path` is required; `ENRICHER_TYPE` is optional.

```yaml
enrichers:
  beamline-policy:
    type: python-enricher
    script-path: /opt/mldp/enrichers/beamline_policy.py
```

## Input Contract

Each call receives a single dictionary:

```python
{
    "reader_name": "bsas_reader",       # str: name of the source reader
    "payload_type": "time-series",      # str: one of the four variant names below
    "metadata": {"run": "42"},          # dict[str, str]: current batch metadata
}
```

`payload_type` values:

| Value | Batch variant |
|---|---|
| `"time-series"` | Time-series frames with columns and timestamps |
| `"source-metadata"` | Reader source metadata |
| `"configuration"` | Writer configuration payload |
| `"configuration-activation"` | Configuration activation signal |

The input dictionary is a snapshot. Mutating it does not affect the C++ batch.

## Return Contract

| Return value | Effect |
|---|---|
| `None` | Accept-and-drop the batch for this writer. |
| `{}` or any dict without `"metadata"` key | Pass batch through unchanged. |
| `{"metadata": {...}}` | Merge the string-to-string dict into batch metadata, overwriting matching keys. |
| Any other type | Reject the batch (treated as enrichment failure). |
| Dict with non-string metadata keys or values | Reject the batch. |

A Python exception rejects the batch, prints the traceback, and does not stop the writer.

## Writing a Script

### Minimal pass-through

```python
def enrich(batch):
    return {}
```

### Metadata tagging

```python
ENRICHER_TYPE = "tag_payload"

def enrich(batch):
    return {
        "metadata": {
            "payload_kind": batch["payload_type"],
            "processed_by": "tag_payload",
        }
    }
```

### Conditional filtering

Drop `configuration-activation` batches for one writer while letting them through for others:

```python
ENRICHER_TYPE = "filter_config_activation"

def enrich(batch):
    if batch["payload_type"] == "configuration-activation":
        return None    # drop for this writer only
    return {}
```

### Merging existing metadata

```python
ENRICHER_TYPE = "append_run_id"

_RUN_ID = "lcls-2025-run3"

def enrich(batch):
    merged = dict(batch["metadata"])
    merged["run_id"] = _RUN_ID
    return {"metadata": merged}
```

Module-level state (like `_RUN_ID`) is initialized once when the module loads. Because one named enricher instance executes serially, module-level mutable state is safe from concurrent access within a single instance, but is shared if the same global name is referenced by multiple writer chains.

## Placing the Script

- Store scripts under the directory configured in `python-plugin-path`, or reference them with explicit `script-path`.
- Scripts are compiled and loaded at startup. A missing or invalid file stops the driver.
- Each named enricher definition loads its own module object. Two definitions pointing to the same file load two separate module instances with independent state.

## Concurrency Notes

- GIL is acquired for every `enrich()` call and released afterward. Long-running Python scripts stall other Python enrichers.
- A shared enricher instance (same global name, multiple writers) executes calls serially per the base class mutex. Writers are not stalled relative to each other beyond this per-instance serialization.

## Testing a Script

Write C++ integration tests under `test/enricher/`, following `python_enricher_test.cpp`. Cover:

- Each `payload_type` the script branches on.
- Expected metadata keys and values after enrichment.
- `None` return (filtering) paths.
- Exception behavior (the test should assert the batch is rejected, not that the driver crashes).

Run the Python enricher tests:

```bash
ctest --test-dir build -R PythonEnricher --output-on-failure
```
