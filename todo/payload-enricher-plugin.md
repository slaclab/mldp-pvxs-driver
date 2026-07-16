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

### `ShardSlotEnricher`
Assigns a stable MongoDB shard slot (`shardSlot` key in `DataColumn::metadata`)
to every column in a `TimeSeriesPayload`. Uses round-robin shard selection with
a random slot within each shard's range. The PV→slot assignment is **permanent**:
the first time a PV is seen a slot is chosen and cached; all subsequent batches
for that PV use the identical slot value.

```yaml
type: shard-slot
num_shards: 6          # number of MongoDB shards (default 6)
```

Implementation notes:
- Maintains `std::unordered_map<std::string, uint16_t> pv_slot_map_` and
  `std::mt19937 rng_` as enricher member state (seeded once in `configure()`).
- Shard range per slot: `[shard * (65536 / num_shards), shard * (65536 / num_shards) + shardSize - 1]`.
  Random slot selected uniformly within that range via `std::uniform_int_distribution<uint32_t>`.
- Iterates all frames in `TimeSeriesPayload`; stamps `shardSlot` (5-char zero-padded decimal)
  on each `DataColumn::metadata` entry that does not already have the key.
- Replaces the inline Phase 5b shard logic in `HDF5BsasGen1Reader` — once this enricher
  ships, `pv_shard_slot_map_`, `next_shard_`, and `rng_` are removed from the reader.

**Shard-range change semantics (important for future maintenance):**
- `pv_slot_map_` lives in-process memory only; it resets on process restart.
- If `num_shards` changes between runs, only PVs seen for the first time after
  the change receive slots in the new ranges. PVs already in `pv_slot_map_` at
  startup (i.e., all PVs, since the map is empty at boot) will be assigned fresh
  slots under the new shard layout on their first batch of the new run.
- MongoDB shard keys are immutable per document: historical documents written
  with old `shardSlot` values remain on their original shards. Routing the same
  PV to a different slot across runs means historical vs. new data for that PV
  land on different shards, requiring scatter-gather reads until an offline ETL
  or MongoDB live-reshard (v6.0+) consolidates them.
- Conclusion: changing `num_shards` in config is safe for brand-new PVs; for
  existing PVs it splits data across shards. Plan any shard-layout changes as a
  coordinated migration, not a live config tweak.

### `PythonPayloadEnricher`
Allows users to implement `IPayloadEnricher` in Python without recompiling.
Follows the exact same pattern as `PythonAlgorithm` (`include/processor/impl/PythonAlgorithm.h`):
- Entire class guarded by `#ifdef BUILD_PYTHON_PROCESSOR` (reuses existing CMake option).
- Header forward-declares `struct _object; using PyObject = _object;` — `<Python.h>` not exposed to consumers.
- Uses Python C API directly (no pybind11). GIL acquired/released via `GILGuard` RAII helper.
- `enricherType()` returns `"python-enricher"`.

**Duck-typed Python contract** — user script exposes at module scope:

```python
# Required: config dict
config = {
    "name": "my-enricher",   # informational
}

# Required: callable — receives batch dict, returns modified dict or None to drop
def enrich(batch: dict) -> dict | None:
    # batch["reader_name"]  : str
    # batch["metadata"]     : dict[str, str]   — mutable
    # batch["payload_type"] : "timeseries" | "source_metadata" | "configuration" | ...
    # batch["payload"]      : payload-specific dict (see timeseries shape below)
    batch["metadata"]["enriched_by"] = "my-enricher"
    return batch  # return None to drop the batch
```

`TimeSeriesPayload` shape exposed to Python:
```python
{
    "root_source_name": "...",
    "end_of_batch_group": bool,
    "is_tabular": bool,
    "frames": [
        {
            "timestamps": [float, ...],   # seconds-since-epoch per sample
            "columns": [
                {
                    "name": "PV:NAME",
                    "metadata": {"key": "value", ...},  # mutable
                    "values": [float, ...]
                }
            ]
        }
    ]
}
```

Other payload variants pass as minimal type-tagged dicts; enricher returns `batch` unchanged to pass them through.

**C++ internals:**
- `configure()`: reads `script_path` (single file) or `script-dir` (directory, reuses `PythonScriptDirectoryLoader`); resolves and stores `enrich_fn_` callable.
- `enrich()`: converts `batch` → Python dict (`batchToDict`), calls `enrich_fn_(dict)`, writes result back via `dictToBatch`. Returns `false` on `None` return or Python exception (exception is logged + cleared, never propagates as C++ exception).
- Only `metadata`, `DataColumn::metadata`, and `DataColumn::values` need round-trip write-back; structural fields (`root_source_name`, `reader_name`) are read-only from Python.

Config:
```yaml
- type: python-enricher
  script_path: /etc/mldp/enrichers/stamp_metadata.py
# or:
  script-dir: /etc/mldp/enrichers/
```

Macro: `REGISTER_ENRICHER("python-enricher", PythonPayloadEnricher)` — inside `#ifdef BUILD_PYTHON_PROCESSOR`.

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
| `include/enricher/impl/ShardSlotEnricher.h` | Fourth built-in enricher |
| `include/enricher/impl/PythonPayloadEnricher.h` | Python-backed enricher (`#ifdef BUILD_PYTHON_PROCESSOR`) |
| `src/enricher/impl/StaticMetadataEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/ColumnAttributeEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/TimestampClampEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/ShardSlotEnricher.cpp` | Impl + `REGISTER_ENRICHER` |
| `src/enricher/impl/PythonPayloadEnricher.cpp` | Impl + `REGISTER_ENRICHER` (`#ifdef BUILD_PYTHON_PROCESSOR`) |
| `test/enricher/EnricherChainTest.cpp` | Chain ordering, drop semantics |
| `test/enricher/StaticMetadataEnricherTest.cpp` | KV injection coverage |
| `test/enricher/TimestampClampEnricherTest.cpp` | Boundary clamping |
| `test/enricher/ShardSlotEnricherTest.cpp` | Slot stability, round-robin shard distribution |
| `test/enricher/PythonPayloadEnricherTest.cpp` | Pass-through, drop on None, exception safety, metadata round-trip (`#ifdef BUILD_PYTHON_PROCESSOR`) |

## Files to Modify

| File | Change |
|------|--------|
| `src/controller/MLDPPVXSController.cpp` | Build `enricher_chains_` at startup; apply per-writer in `dispatchToWriters()` |
| `include/controller/MLDPPVXSController.h` | Add `std::vector<EnricherChain> enricher_chains_` member |
| `CMakeLists.txt` | Add `src/enricher/` sources unconditionally; add `PythonPayloadEnricher.cpp` inside existing `if(BUILD_PYTHON_PROCESSOR)` block (no new CMake option needed) |
| `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h` | Remove `pv_shard_slot_map_`, `next_shard_`, `rng_` members |
| `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp` | Remove Phase 5b shard-slot block; remove `rng_` init in ctor |
| `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h` | Remove `numShards()` / `num_shards_` (moved to enricher config) |

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
