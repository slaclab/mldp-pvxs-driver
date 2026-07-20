# Plan: Python Enricher Plugin Directory + Self-Describing Type

## Problem

`python-enricher` instances require a full absolute `script-path` each time:

```yaml
enrichers:
  tag-payload:
    type: python-enricher
    script-path: /opt/mldp/enrichers/tag_payload.py
```

Two issues:
1. Every instance must repeat the base directory — brittle when the deployment path changes.
2. The Python type is opaque; `type: python-enricher` carries no semantic meaning. C++ enrichers expose `enricherType()` but Python scripts don't.

---

## Goal

1. Add a global `python-plugin-path` key under `enrichers:` — a base directory for all Python scripts.
2. Add a parallel `algorithms-plugin-path` key under `processors:` for the existing algorithm/processor plugin framework — same pattern, same mechanics.
3. Both frameworks fall back to a local `enrichers/` or `algorithms/` subdirectory (relative to CWD) when no explicit plugin path is configured.
4. Python scripts declare their logical type via a module-level `ENRICHER_TYPE` string.
5. When an instance's `type:` doesn't match any registered C++ enricher, the registry auto-resolves it as `<python-plugin-path>/<type>.py` (unless an explicit `script-path` is given — that takes precedence).
6. After loading, validate that `script.ENRICHER_TYPE == requested type`; throw on mismatch to prevent silently loading the wrong script.

### Target config shape

```yaml
enrichers:
  python-plugin-path: /opt/mldp/enrichers   # NEW — explicit base dir
                                             # default fallback: ./enrichers/ (relative to CWD)

  tag-payload:                               # instance name
    type: tag_payload                        # resolved → /opt/mldp/enrichers/tag_payload.py
                                             # script must define: ENRICHER_TYPE = "tag_payload"

  tag-payload-v2:                            # second instance, same script, different config
    type: tag_payload
    some-param: custom-value

  custom-script:                             # explicit script-path overrides python-plugin-path
    type: python-enricher
    script-path: /absolute/path/custom.py   # explicit path wins; ENRICHER_TYPE not required

  static-meta:                              # C++ builtins unchanged
    type: static-metadata
    metadata:
      experiment_id: run-42

# Parallel pattern for processor/algorithm framework:
processors:
  algorithms-plugin-path: /opt/mldp/algorithms  # NEW — default fallback: ./algorithms/
  # ... existing processor config unchanged
```

### Python script convention

```python
ENRICHER_TYPE = "tag_payload"   # required when loaded via python-plugin-path

def enrich(batch):
    batch["metadata"]["tag"] = "payload"
    return batch
```

`ENRICHER_TYPE` is optional when `script-path` is explicit.

---

## Design

### Resolution logic (EnricherRegistry)

For each enricher instance:
1. Try `EnricherFactory::create(type, definition)` — handles all registered C++ types and `"python-enricher"` with explicit `script-path`.
2. If that returns `nullptr` (unknown type):
   - If `definition` has `script-path` key → set `type = "python-enricher"`, pass config as-is.
   - Else if `python_plugin_path_` is set → inject `script-path = <python_plugin_path_>/<type>.py` into config, set `type = "python-enricher"`.
   - Else → throw `"unknown enricher type '<type>'"`.
3. After construction via auto-resolution path → call `enricher->enricherType()` and compare to the requested type string. Mismatch → throw.

Config injection (step 2b): `Config` is read-only, so build a YAML string that merges `script-path` into the definition node using `config::Config::configFromYamlString()`, preserving all other keys.

### `python_plugin_path_` parsing

In `EnricherRegistry` constructor, before iterating instances:

```cpp
// Explicit config wins; fall back to local ./enrichers/ directory
if (entries.front().hasChild("python-plugin-path"))
    python_plugin_path_ = entries.front().get("python-plugin-path");
else
    python_plugin_path_ = "enrichers";   // relative to CWD — used only if directory exists
```

Skip `"python-plugin-path"` key when iterating named enricher instances.

### `algorithms-plugin-path` parsing (processor framework)

Same pattern in the processor registry (the class that reads the `processors:` config block and builds `PythonScriptDirectoryLoader` instances). Before iterating processor instances:

```cpp
if (entries.front().hasChild("algorithms-plugin-path"))
    algorithms_plugin_path_ = entries.front().get("algorithms-plugin-path");
else
    algorithms_plugin_path_ = "algorithms";   // local fallback
```

The processor framework currently uses `script-dir` per instance. With this change, when `type:` is unknown and no `script-dir` is given, the registry tries `<algorithms_plugin_path_>/<type>.py` (or `<algorithms_plugin_path_>/<type>/` for directory-based scripts).

---

## Files to Modify

### Enricher framework

| File | Change |
|------|--------|
| `include/enricher/EnricherRegistry.h` | Add `std::string python_plugin_path_` member |
| `src/enricher/EnricherRegistry.cpp` | Parse `python-plugin-path` (default `"enrichers"`); skip it in instance loop; auto-resolve unknown types; validate `enricherType()` post-construction |
| `include/enricher/PythonEnricher.h` | Add `std::string python_enricher_type_` member |
| `src/enricher/PythonEnricher.cpp` | Read `ENRICHER_TYPE` attr in `configure()`; update `enricherType()` to return it |

### Processor/algorithm framework

| File | Change |
|------|--------|
| processor registry header (find exact name — likely `include/processor/ProcessorRegistry.h` or similar) | Add `std::string algorithms_plugin_path_` member |
| processor registry `.cpp` | Parse `algorithms-plugin-path` (default `"algorithms"`); auto-resolve unknown processor types to `<path>/<type>.py` or `<path>/<type>/` |

---

## `PythonEnricher` changes in detail

In `configure()`, after loading the module:

```cpp
python_enricher_type_ = "python-enricher";   // default
PyObject* attr = PyObject_GetAttrString(module_, "ENRICHER_TYPE");
if (attr != nullptr)
{
    if (PyUnicode_Check(attr))
        python_enricher_type_ = PyUnicode_AsUTF8(attr);
    Py_DECREF(attr);
}
else
    PyErr_Clear();  // attribute absent — not an error
```

`enricherType()` returns `python_enricher_type_`.

---

## YAML merge approach for injecting `script-path`

Because `Config` is read-only, build a YAML string from the existing definition node, append `script-path: <resolved>`, and reparse:

```cpp
// pseudo-code in EnricherRegistry
std::string yaml = definitionToYamlString(definition);   // serialize existing keys
yaml += "\nscript-path: " + resolved_path;
auto merged = config::Config::configFromYamlString(yaml);
auto enricher = EnricherFactory::create("python-enricher", merged.front());
```

`definitionToYamlString` can be implemented using `ryml` (already a dep via `Config::raw()`) to emit the node's YAML text.

---

## Validation

1. Build in devcontainer with `-DBUILD_PYTHON_PROCESSOR=ON`.
2. Existing enricher and processor tests pass unmodified.
3. New tests in `test/enricher/`:
   - `python_plugin_path_resolves_script` — registry loads script via explicit base dir; `enricherType()` returns script's `ENRICHER_TYPE`.
   - `python_plugin_path_local_fallback` — no `python-plugin-path` in config; script found via default `./enrichers/` local dir.
   - `python_plugin_path_type_mismatch_throws` — script with wrong `ENRICHER_TYPE` causes construction to throw.
   - `explicit_script_path_overrides_plugin_path` — explicit `script-path` used even when `python-plugin-path` is set.
   - `explicit_script_path_no_enricher_type_required` — `ENRICHER_TYPE` absent in script, loaded via `script-path` → no error, `enricherType()` returns `"python-enricher"`.
   - `cpp_builtin_unaffected` — `static-metadata` instance alongside `python-plugin-path` entry works correctly.
4. New tests in `test/processor/` (or equivalent):
   - `algorithms_plugin_path_resolves_script` — processor instance resolved via explicit `algorithms-plugin-path`.
   - `algorithms_local_fallback` — no `algorithms-plugin-path`; script found via default `./algorithms/` local dir.
