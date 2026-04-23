# Implementation Plan: Reader-to-Writer Routing

> **Reference**: [`docs/todo/reader-writer-routing.md`](../todo/reader-writer-routing.md)
> **Status**: Draft
> **Scope**: Selective dispatch of `EventBatch` from named readers to named writers

---

## 1. Current Architecture

```
Reader(s) ──push()──▶ IDataBus (MLDPPVXSController) ──fan-out──▶ ALL Writers
```

**Key observations from code audit:**

| Component | File | Relevant Detail |
|-----------|------|-----------------|
| Controller | `src/controller/MLDPPVXSController.cpp:156-170` | Iterates `writers_` vector, copies batch to every writer |
| Writer interface | `include/writer/IWriter.h` | `IWriter::name()` returns string id; `push(EventBatch)` |
| Reader base | `include/reader/IReader.h` | `Reader::name()` returns string id; pushes via `bus_->push()` |
| EventBatch | `include/util/bus/IDataBus.h:31-38` | Has `root_source` but no reader identity |
| Config parsing | `src/controller/MLDPPVXSControllerConfig.cpp` | Readers/writers parsed as `(type, Config)` pairs |
| Writer config | `include/writer/WriterConfig.h` | Writer instances have `name` field in YAML |

**Critical finding**: `EventBatch` carries `root_source` (PV name) but **not** reader identity. Controller `push()` receives batch from `IDataBus` interface — no reader name is passed.

---

## 2. Design Decisions

### 2.1 Writer-Centric Routing (recommended over TODO doc's reader-centric)

Writer declares which readers feed it. Writer owns its data contract.

```yaml
# Routing absent or empty → all-to-all (backward compatible)
routing:
  hdf5_local:
    from: [bsas-reader, epics-archiver-1]
  grpc_forwarder:
    from: [bsas-reader, scalar-reader]
  monitoring:
    from: [all]          # explicit wildcard
```

**Why flip**: easier to audit "what data reaches this writer?" — one config stanza per writer.

### 2.2 Per-Reader Routing Only (v1)

No per-source (`root_source`) filtering in controller. Reasons:
- Per-source = regex/glob check per batch → breaks "negligible overhead" goal
- Source filtering can live inside writer if needed later
- Per-reader routing covers stated use cases (HDF5 gets BSAS only, gRPC gets scalars only)

### 2.3 Immutable Route Table

Built once at startup. No hot-reload in v1. Immutability = thread safety without mutex.

### 2.4 Reader Identity in EventBatch

`EventBatch` needs a `reader_name` field so controller can look up route table during `push()`.

---

## 3. Implementation Phases

### Phase 1: Add Reader Identity to EventBatch

**Files modified:**

| File | Change |
|------|--------|
| `include/util/bus/IDataBus.h` | Add `std::string reader_name` field to `EventBatchStruct` |

```cpp
struct EventBatchStruct
{
    std::string reader_name;               // ← NEW: identity of producing reader
    std::string root_source;
    // ... existing fields unchanged ...
};
```

**Impact**: field defaults to empty string → zero breakage for existing code paths.

### Phase 2: Readers Stamp Their Name

Each reader sets `reader_name` before calling `bus_->push()`.

**Files modified:**

| File | Change |
|------|--------|
| `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp` | Set `batch.reader_name = name()` before push |
| `src/reader/impl/epics/base/EpicsBaseReader.cpp` | Same |
| `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` | Same |

Grep all `bus_->push(` call sites to ensure completeness.

### Phase 3: Route Table Data Structure

**New file:** `include/controller/RouteTable.h`

```cpp
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mldp_pvxs_driver::controller {

/**
 * @brief Immutable routing table: writer_name → set of accepted reader names.
 *
 * Built once at startup from YAML config. Thread-safe by immutability.
 * When empty, controller falls back to all-to-all dispatch.
 */
class RouteTable
{
public:
    /// Construct empty table (all-to-all mode).
    RouteTable() = default;

    /// Build from parsed config. Validates all names exist.
    /// @throws std::runtime_error on unknown reader/writer names.
    static RouteTable build(
        const std::vector<std::pair<std::string, std::vector<std::string>>>& routes,
        const std::unordered_set<std::string>& known_readers,
        const std::unordered_set<std::string>& known_writers);

    /// True when no routing configured → all-to-all.
    bool isAllToAll() const noexcept;

    /// Check if writer should receive batches from given reader.
    /// O(1) average — unordered_set lookup.
    bool accepts(const std::string& writer_name,
                 const std::string& reader_name) const noexcept;

    /// Warn about orphan readers/writers not mentioned in any route.
    std::vector<std::string> orphanReaders(
        const std::unordered_set<std::string>& known_readers) const;
    std::vector<std::string> orphanWriters(
        const std::unordered_set<std::string>& known_writers) const;

private:
    /// writer_name → set of reader names accepted (or "all" sentinel).
    std::unordered_map<std::string, std::unordered_set<std::string>> table_;
    bool all_to_all_{true};
};

} // namespace mldp_pvxs_driver::controller
```

**New file:** `src/controller/RouteTable.cpp`

Implementation details:
- `build()`: iterate config, populate `table_`. If any entry has `"all"`, store special sentinel.
- `accepts()`: if `all_to_all_` return true. Otherwise lookup writer in `table_`, check reader in set. Writer not in table = receives nothing.
- Validation: every reader name in config must exist in `known_readers`. Every writer name in config must exist in `known_writers`. Unknown name → `throw std::runtime_error`.

### Phase 4: Parse Routing Config

**Files modified:**

| File | Change |
|------|--------|
| `include/controller/MLDPPVXSControllerConfig.h` | Add `routeEntries()` accessor returning `vector<pair<string, vector<string>>>` |
| `src/controller/MLDPPVXSControllerConfig.cpp` | Add `parseRouting()` — parse `routing:` block |

YAML structure parsed:

```yaml
routing:
  writer_name_1:
    from: [reader_a, reader_b]
  writer_name_2:
    from: [all]
```

`parseRouting()` logic:
1. If no `routing` key → return empty (all-to-all)
2. Iterate map children of `routing`
3. Each child key = writer name, child must have `from` sequence
4. Collect as `{writer_name, vector<reader_name>}`

### Phase 5: Integrate Route Table into Controller Dispatch

**Files modified:**

| File | Change |
|------|--------|
| `include/controller/MLDPPVXSController.h` | Add `RouteTable route_table_` member |
| `src/controller/MLDPPVXSController.cpp` | Build route table in `start()`, filter in `push()` |

**`start()` changes:**

After building writers and readers, collect known names and build route table:

```cpp
// After writer/reader creation...
std::unordered_set<std::string> known_writers;
for (const auto& w : writers_) known_writers.insert(w->name());

std::unordered_set<std::string> known_readers;
for (const auto& r : readers_) known_readers.insert(r->name());

route_table_ = RouteTable::build(config_.routeEntries(), known_readers, known_writers);

// Log orphan warnings
for (const auto& name : route_table_.orphanReaders(known_readers))
    warnf(*logger_, "Reader '{}' not mentioned in any route — receives no output destination", name);
for (const auto& name : route_table_.orphanWriters(known_writers))
    warnf(*logger_, "Writer '{}' not mentioned in any route — will receive no data", name);
```

**`push()` changes:**

```cpp
// Replace unconditional fan-out with filtered dispatch:
for (std::size_t i = 0; i < n; ++i)
{
    if (!route_table_.accepts(writers_[i]->name(), batch_values.reader_name))
        continue;

    auto* writerPtr = writers_[i].get();
    EventBatch batchCopy = batch_values;
    futures.push_back(
        thread_pool_->submit_task([writerPtr, b = std::move(batchCopy)]() mutable -> bool {
            return writerPtr->push(std::move(b));
        }));
}
```

**Performance**: when `all_to_all_` is true, `accepts()` returns immediately — zero overhead vs current behavior.

### Phase 6: Metrics

**Files modified:**

| File | Change |
|------|--------|
| `src/controller/MLDPPVXSController.cpp` | Add counter for routed/skipped batches |

New metrics:
- `mldp_pvxs_driver_controller_route_skipped_total{writer="X", reader="Y"}` — batch skipped by routing
- Log at debug level when batch skipped

### Phase 7: Tests

**New files:**

| File | Purpose |
|------|---------|
| `test/controller/RouteTableTest.cpp` | Unit tests for RouteTable |
| `test/controller/RoutingIntegrationTest.cpp` | End-to-end routing dispatch |

**RouteTable unit tests:**
1. Empty config → `isAllToAll() == true`, `accepts()` always true
2. Single writer, two readers → only matched readers pass
3. `from: [all]` → writer accepts every reader
4. Unknown reader name in config → `build()` throws
5. Unknown writer name in config → `build()` throws
6. `orphanReaders()` returns readers not in any route
7. `orphanWriters()` returns writers not in any route
8. Duplicate reader in `from` list → handled gracefully (set dedup)

**Routing integration tests:**
1. Two readers, two writers, routing configured → batches dispatched correctly
2. No routing config → all-to-all (backward compat)
3. Writer with `from: [all]` receives from every reader

---

## 4. File Change Summary

| Action | File | Phase |
|--------|------|-------|
| **Modify** | `include/util/bus/IDataBus.h` | 1 |
| **Modify** | `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp` | 2 |
| **Modify** | `src/reader/impl/epics/base/EpicsBaseReader.cpp` | 2 |
| **Modify** | `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` | 2 |
| **Create** | `include/controller/RouteTable.h` | 3 |
| **Create** | `src/controller/RouteTable.cpp` | 3 |
| **Modify** | `include/controller/MLDPPVXSControllerConfig.h` | 4 |
| **Modify** | `src/controller/MLDPPVXSControllerConfig.cpp` | 4 |
| **Modify** | `include/controller/MLDPPVXSController.h` | 5 |
| **Modify** | `src/controller/MLDPPVXSController.cpp` | 5 |
| **Modify** | `CMakeLists.txt` | 3 (add RouteTable.cpp) |
| **Create** | `test/controller/RouteTableTest.cpp` | 7 |
| **Create** | `test/controller/RoutingIntegrationTest.cpp` | 7 |

---

## 5. Migration & Backward Compatibility

- **No routing config** → all-to-all. Zero behavior change for existing deployments.
- **`reader_name` empty** → `accepts()` treats as "unknown reader". When routing configured, unknown reader matches nothing → batch dropped with warning log. Forces readers to stamp identity.
- **Existing YAML** needs no changes unless user wants selective routing.

---

## 6. Open Questions Resolved

| Question from TODO | Decision | Rationale |
|--------------------|----------|-----------|
| Glob/wildcard patterns for sources? | **Deferred to v2** | Per-reader routing covers stated use cases. Source-level filtering adds per-event cost. |
| Per-source vs per-reader routing? | **Per-reader** | O(1) lookup per batch vs O(N) per event. Source filtering can be writer-internal if needed. |
| Hot-reload routing config? | **No (v1)** | Immutable table = thread-safe without locks. Revisit when operational need arises. |

---

## 7. Dependency Graph

```
Phase 1 (EventBatch field)
    │
    ▼
Phase 2 (Readers stamp name)     Phase 3 (RouteTable class)
    │                                  │
    │                             Phase 4 (Config parsing)
    │                                  │
    └──────────┬───────────────────────┘
               ▼
         Phase 5 (Controller integration)
               │
               ▼
         Phase 6 (Metrics)
               │
               ▼
         Phase 7 (Tests)
```

Phases 2 and 3-4 can run in parallel. Phase 5 depends on all prior phases.
