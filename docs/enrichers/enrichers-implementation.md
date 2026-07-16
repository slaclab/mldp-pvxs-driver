# Implementing Payload Enrichers

> **Related:** [Payload Enricher Guide](enrichers.md) | [Writer Implementation Guide](../writers/writers-implementation.md) | [Configuration Reference](../guides/configuration.md#global-enrichers-and-writer-chains)

This guide describes how to add a new payload enricher in C++ or Python. Enrichers are global plugins: the controller constructs one shared instance for every named YAML definition, then installs shared references into queued writer chains.

## Choose C++ or Python

| Use C++ when | Use Python when |
|---|---|
| The transformation needs direct access to complete `EventBatch` and time-series frame data. | Metadata-only policy can be expressed through `reader_name`, `payload_type`, and batch metadata. |
| It is latency-sensitive or runs on every high-rate batch. | You need rapid deployment without recompiling the driver. |
| It needs richer state than the Python metadata contract exposes. | A one-file `enrich(batch)` function is sufficient. |

The current Python bridge deliberately exposes only the reader name, payload type, and batch metadata. It cannot modify columns, timestamps, or payload values.

## C++ Enricher

### 1. Implement `IPayloadEnricher`

Create a header and source under `include/enricher/` and `src/enricher/`. The constructor receives the named definition's `config::Config`; validate mandatory settings in `configure()` and throw `std::runtime_error` for invalid startup configuration.

```cpp
#include <enricher/EnricherFactory.h>

namespace mldp_pvxs_driver::enricher {

class AddSourceEnricher final : public IPayloadEnricher
{
    REGISTER_ENRICHER("add-source", AddSourceEnricher)

public:
    explicit AddSourceEnricher(const config::Config& config)
    {
        configure(config);
    }

    void configure(const config::Config& config) override
    {
        source_ = config.get("source");
        if (source_.empty())
            throw std::runtime_error("add-source enricher requires 'source'");
    }

    bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override
    {
        batch.metadata["source"] = source_;
        return true;
    }

    std::string enricherType() const override
    {
        return "add-source";
    }

private:
    std::string source_;
};

} // namespace mldp_pvxs_driver::enricher
```

`enrich()` is called under the base class's per-instance mutex and must not throw. Return `true` to continue the chain or `false` to accept-but-drop the batch for that writer. Catch expected runtime failures inside `enrich()`, log them, and return `false`.

### 2. Register the type

`REGISTER_ENRICHER("add-source", AddSourceEnricher)` performs static factory registration. Keep it in the class body. The library is already whole-archive linked for driver executables and tests, which preserves static registrations.

### 3. Add the source to CMake

Add the implementation file to `lib${PROJECT_NAME}`:

```cmake
target_sources(lib${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/enricher/AddSourceEnricher.cpp")
```

If the enricher depends on an optional library, add a focused build option and guard both the CMake source and the registration with the same compile definition.

### 4. Configure and test it

```yaml
enrichers:
  upstream-source:
    type: add-source
    source: daq-a
writer:
  mldp:
    - name: mldp_main
      enrichers: [upstream-source]
```

Add tests under `test/enricher/`. Cover valid configuration, invalid configuration, the intended payload variants, metadata overwrite rules, filtering behavior if relevant, and state sharing if the enricher is stateful.

## Python Enricher

### 1. Enable the build gate

The type exists only when the driver is configured with Python support:

```bash
cmake -S . -B build -DMLDP_PVXS_DRIVER_TESTS=ON -DBUILD_PYTHON_PROCESSOR=ON
```

When disabled, `python-enricher` is not registered and a configuration using it fails at startup as an unknown type.

### 2. Write the module

Create one `.py` file. It needs only `enrich(batch)`; there is no required `config` object.

```python
def enrich(batch):
    metadata = dict(batch["metadata"])
    metadata["enriched_by"] = "beamline-policy"
    metadata["payload_kind"] = batch["payload_type"]
    return {"metadata": metadata}
```

To filter a batch for one writer destination:

```python
def enrich(batch):
    if batch["payload_type"] == "configuration-activation":
        return None
    return {}
```

Python enrichers cannot mutate the input dictionary to change the C++ batch. Return a dictionary with `metadata` instead. Metadata keys and values must be strings.

### 3. Reference the script

```yaml
enrichers:
  beamline-policy:
    type: python-enricher
    script-path: /opt/mldp/enrichers/beamline_policy.py
writer:
  mldp:
    - name: mldp_main
      enrichers: [beamline-policy]
```

Each named definition loads its own module. Reusing the same name across writers intentionally shares that module and its Python state. Use different global names when isolation is required.

### 4. Test the script

Place C++ integration tests under `test/enricher/`; use a temporary Python script, following `python_enricher_test.cpp`. Test every payload type your script cares about, its metadata changes, `None` filtering, and invalid/exception behavior.

## C++ Review Checklist

- [ ] The class derives from `IPayloadEnricher` and defines `REGISTER_ENRICHER`.
- [ ] `configure()` validates required fields and throws a clear startup error.
- [ ] `enrich()` is `noexcept` and does not let exceptions escape.
- [ ] State assumes calls are serialized per instance, but does not assume a single writer.
- [ ] The source is included in the appropriate CMake target and optional compile guard.
- [ ] Tests cover relevant `BatchPayload` alternatives and shared-state behavior.
- [ ] The configuration guide and the built-in enricher table are updated for user-visible settings.
