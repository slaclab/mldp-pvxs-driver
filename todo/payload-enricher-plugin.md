# Plan: Payload Enricher Plugin

## Problem

Writers receive `EventBatch` payloads exactly as produced by readers/algorithms.
No mechanism exists to augment batches — e.g. inject metadata columns, add KV
attributes, stamp provenance fields, or normalize units — without modifying the
reader or the writer itself.

The algorithm plugin (`IAlgorithm`) solves a different problem: it *generates*
new virtual sources from aligned inputs. It is not usable here because:
- Enrichers must not change `root_source_name` or spawn new bus events.
- Enrichers run per-writer (or per-writer-group), not per-source alignment.
- Enrichers need no snapshot/buffer/trigger machinery.

## Goal

Introduce an `IPayloadEnricher` plugin layer that sits between the controller
dispatch and the writer's `push()`. Each enricher receives a mutable copy of
`EventBatch`, adds/replaces fields, and passes it on. Multiple enrichers chain.
Writers declare which enricher chain (if any) they belong to.

---

## Design

### 1. `IPayloadEnricher` Interface

```
include/enricher/IPayloadEnricher.h
```

```cpp
namespace mldp_pvxs_driver::enricher {

class IPayloadEnricher {
public:
    virtual ~IPayloadEnricher() = default;

    // Called once after construction with the enricher-specific YAML subtree.
    virtual void configure(const config::Config& cfg) = 0;

    // Enrich batch in-place. Must not throw. Returns false to drop the batch.
    virtual bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept = 0;

    // Type string used to select this enricher in YAML config.
    virtual std::string enricherType() const noexcept = 0;
};

using IPayloadEnricherUPtr = std::unique_ptr<IPayloadEnricher>;

} // namespace mldp_pvxs_driver::enricher
```

Key decisions:
- `enrich()` mutates `EventBatch&` in-place (no copy — caller already owns the batch destined for this writer).
- Returns `bool` so an enricher can act as a gate/filter (return `false` → skip writer).
- `configure()` mirrors `IAlgorithm::configure()` — same YAML-view pattern.

---

### 2. `EnricherFactory` — same static-init registry pattern

```
include/enricher/EnricherFactory.h
src/enricher/EnricherFactory.cpp
```

```cpp
class EnricherFactory
    : public util::factory::Factory<EnricherFactory, IPayloadEnricher,
                                    const config::Config&> { … };

template <typename T>
class EnricherRegistrator {
public:
    explicit EnricherRegistrator(const char* typeName) {
        EnricherFactory::registerType(typeName,
            [](const config::Config& cfg) {
                auto e = std::make_unique<T>();
                e->configure(cfg);
                return e;
            });
    }
};

#define REGISTER_ENRICHER(TYPE_STRING, CLASSNAME) \
    static inline ::mldp_pvxs_driver::enricher::EnricherRegistrator<CLASSNAME> \
        enricher_registrator_{TYPE_STRING};
```

---

### 3. `EnricherChain` — ordered pipeline

```
include/enricher/EnricherChain.h
src/enricher/EnricherChain.cpp
```

```cpp
class EnricherChain {
public:
    // Build from ordered list of (type, config) pairs.
    static EnricherChain build(const std::vector<config::Config>& enricher_nodes);

    // Run all enrichers in order. Returns false if any enricher drops the batch.
    bool run(util::bus::IDataBus::EventBatch& batch) noexcept;

    bool empty() const noexcept;

private:
    std::vector<IPayloadEnricherUPtr> enrichers_;
};
```

- `run()` short-circuits on first `false` (drop semantics).
- `empty()` allows zero-cost bypass in the dispatch hot path.

---

### 4. Writer-side wiring

Each writer gains an optional `EnricherChain` member, injected at construction
time by the controller. `WriterFactory::create()` grows an optional enricher-node
argument.

**`BaseQueuedWriter`** updated:

```cpp
// In push() before queueing:
if (!enricher_chain_.empty()) {
    if (!enricher_chain_.run(batch)) return false;  // enricher dropped it
}
// ... existing toItems() path
```

Or — cleaner — the controller applies the chain **before** calling `writer.push()`,
so `IWriter` stays unchanged. See §6 below.

---

### 5. Config schema

```yaml
writers:
  - name: my-mldp-writer
    type: mldp
    enrichers:                        # optional ordered list
      - type: add-metadata-column
        column_name: experiment_id
        value: "run-42"
      - type: clamp-timestamps
        max_nanos: 999999999
```

Each writer config block may contain an `enrichers` list. The controller reads
this list, builds an `EnricherChain` per writer, and associates it.

---

### 6. Controller dispatch — preferred injection point

**Avoid modifying `IWriter`**. Instead, in `MLDPPVXSController::dispatchToWriters()`:

```cpp
// Per writer:
auto batch_copy = batch;                        // copy for this writer
if (!enricher_chains_[writer_idx].run(batch_copy)) continue;  // dropped
writer->push(std::move(batch_copy));
```

`enricher_chains_` is a `std::vector<EnricherChain>` parallel to `writers_`,
populated at startup from the per-writer config.

This keeps `IWriter` and `BaseQueuedWriter` untouched.

---

### 7. Writer-group enrichers (optional, phase 2)

A writer group shares one `EnricherChain` that runs before fan-out to members:

```yaml
writer_groups:
  - name: mldp-cluster
    enrichers:
      - type: stamp-ingestion-time
    writers: [mldp-writer-1, mldp-writer-2]
```

Phase 2 only — not in initial implementation.

---

## Concrete Enricher Examples (ship with feature)

### `StaticMetadataEnricher`
Injects fixed KV pairs into `EventBatchStruct::metadata`.
Config: `key: value` map.

### `ColumnAttributeEnricher`
Adds KV pairs to `DataColumn::metadata` for matching column names (glob pattern).
Config: `column_pattern`, `attributes` map.

### `TimestampClampEnricher`
Clamps `nanoseconds` field of every `TimestampEntry` to `[0, 999_999_999]`.
Useful as a guard for the MLDP server rejection bug (see `mldp-writer-bidi-stream.md`).
Config: none (always enforces IEEE nanosecond range).

---

## Files to Create

| File | Purpose |
|------|---------|
| `include/enricher/IPayloadEnricher.h` | Abstract interface |
| `include/enricher/EnricherFactory.h` | Registry + `REGISTER_ENRICHER` macro |
| `include/enricher/EnricherChain.h` | Ordered pipeline + `build()` factory |
| `src/enricher/EnricherFactory.cpp` | Factory `create()` impl |
| `src/enricher/EnricherChain.cpp` | `run()` + `build()` impl |
| `include/enricher/impl/StaticMetadataEnricher.h` | First built-in enricher |
| `include/enricher/impl/ColumnAttributeEnricher.h` | Second built-in enricher |
| `include/enricher/impl/TimestampClampEnricher.h` | Third built-in enricher |
| `src/enricher/impl/StaticMetadataEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/ColumnAttributeEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/TimestampClampEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `test/enricher/EnricherChainTest.cpp` | Chain ordering, drop semantics |
| `test/enricher/StaticMetadataEnricherTest.cpp` | KV injection coverage |
| `test/enricher/TimestampClampEnricherTest.cpp` | Boundary clamping |

## Files to Modify

| File | Change |
|------|--------|
| `src/controller/MLDPPVXSController.cpp` | Build `enricher_chains_` at startup; apply per-writer in `dispatchToWriters()` |
| `include/controller/MLDPPVXSController.h` | Add `std::vector<EnricherChain> enricher_chains_` member |
| `src/CMakeLists.txt` / `CMakeLists.txt` | Add `src/enricher/` sources |

---

## Sequence Diagram

```
Reader ──push()──► Controller::dispatchToWriters()
                        │
                        ├─ copy batch
                        ├─ enricher_chains_[i].run(batch_copy)   ← NEW
                        │       │
                        │       ├─ enricher[0].enrich(batch)
                        │       ├─ enricher[1].enrich(batch)
                        │       └─ ... (short-circuit on false)
                        │
                        └─ writer[i].push(batch_copy)
```

---

## Non-Goals

- Enrichers do **not** produce new bus events (that is `IAlgorithm`'s job).
- Enrichers do **not** have snapshot/alignment/trigger logic.
- Enrichers do **not** own threads — they run synchronously on the controller consumer thread.
- Phase-1 enrichers target `TimeSeriesPayload` only; other payload types pass through unchanged unless the enricher explicitly inspects the variant.
