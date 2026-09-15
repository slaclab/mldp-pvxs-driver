# Plan: Remove Auto-Route (All-to-All) and Add Writer Type Declarations

## Context

Currently, omitting the `routing:` YAML block causes all-to-all mode — every reader feeds every writer. This implicit behavior masks misconfigurations. Goal: make routing mandatory, error on missing routes, and let writers declare which reader/processor **types** they accept so the system validates compatibility at startup.

---

## Phase 1: Remove all-to-all from RouteTable

**Files:** `include/controller/RouteTable.h`, `src/controller/RouteTable.cpp`

- Remove `bool all_to_all_{true}` member
- Remove `isAllToAll()` method
- Remove deprecated `RouteEntry` typedef
- `build()` with empty routes → return table with empty `table_` (rejects everything)
- `accepts()` / `acceptsSource()` — remove early-return-true on all_to_all
- `orphanReaders()` / `orphanWriters()` — remove early-return-empty on all_to_all

---

## Phase 2: Require routing in config parsing

**File:** `src/controller/MLDPPVXSControllerConfig.cpp`

- `parseRouting()`: throw `Error` when `routing:` key is absent (currently returns silently)

**File:** `src/config/validate.cpp`

- Emit ERROR diagnostic when `!cfg.hasChild("routing")`
- Cross-check every declared writer name has a routing entry

---

## Phase 3: Error on orphan writers at startup

**File:** `src/controller/MLDPPVXSController.cpp` (~line 365)

- After `RouteTable::build(...)`, call `orphanWriters(known_writers)`
- If non-empty → `throw std::runtime_error` listing the unrouted writers
- Simplify line 521: remove `!route_table_.isAllToAll() &&` guard (always check empty reader_name)

---

## Phase 4: Add `acceptedPayloadTypes()` to IWriter

Writers already have runtime `acceptsPayload(BatchPayload&)` that checks variant type. New approach: declare accepted **payload types** statically via a typed enum so the route table can validate at startup that routed readers produce compatible payloads. No raw strings — compile-time safety.

### 4.1 — Define `PayloadType` enum

**File:** `include/util/bus/IDataBus.h` (near `BatchPayload` variant definition, line ~109)

```cpp
enum class PayloadType : uint8_t {
  TimeSeries,
  SourceMetadata,
  Configuration,
  ConfigurationActivation
};
```

Each enumerator maps 1:1 to a `BatchPayload` variant alternative:
- `PayloadType::TimeSeries` → `TimeSeriesPayload`
- `PayloadType::SourceMetadata` → `SourceMetadataPayload`
- `PayloadType::Configuration` → `ConfigurationPayload`
- `PayloadType::ConfigurationActivation` → `ConfigurationActivationPayload`

Add a helper to resolve variant index → enum at compile time (for runtime dispatch if needed):

```cpp
template <typename T>
constexpr PayloadType payloadTypeFor();

template <> constexpr PayloadType payloadTypeFor<TimeSeriesPayload>() { return PayloadType::TimeSeries; }
template <> constexpr PayloadType payloadTypeFor<SourceMetadataPayload>() { return PayloadType::SourceMetadata; }
template <> constexpr PayloadType payloadTypeFor<ConfigurationPayload>() { return PayloadType::Configuration; }
template <> constexpr PayloadType payloadTypeFor<ConfigurationActivationPayload>() { return PayloadType::ConfigurationActivation; }
```

### 4.2 — Add `acceptedPayloadTypes()` to IWriter

**File:** `include/writer/IWriter.h`

```cpp
virtual std::unordered_set<PayloadType> acceptedPayloadTypes() const { return {}; }
```

Empty set = accept all (backward compat, same as current `acceptsPayload()` default returning true). Override in concrete writers:

| Writer class | Accepted payload types |
|---|---|
| `MLDPWriter` | `{PayloadType::TimeSeries}` |
| `HDF5WriterBase` | `{PayloadType::TimeSeries}` |
| `MLDPConfigurationWriter` | `{PayloadType::Configuration, PayloadType::ConfigurationActivation}` |
| `MLDPPVMetadataWriter` | `{PayloadType::SourceMetadata}` |

### 4.3 — Readers declare produced payload types

**Readers must also declare what payload types they produce.** Add to `IReader` (or store in config):

```cpp
virtual std::unordered_set<PayloadType> producedPayloadTypes() const = 0;
```

| Reader type | Produces |
|---|---|
| `epics-pvxs` | `{PayloadType::TimeSeries}` |
| `epics-base` | `{PayloadType::TimeSeries}` |
| `epics-archiver` | `{PayloadType::TimeSeries}` |
| `hdf5-bsas-gen1` | `{PayloadType::TimeSeries}` |
| `epics-ds-metadata` | `{PayloadType::SourceMetadata}` |
| `slac-calendar` | `{PayloadType::Configuration, PayloadType::ConfigurationActivation}` |

Processors produce `{PayloadType::TimeSeries}` (all current algorithms output `TimeSeriesPayload`).

### 4.4 — Hash support for `std::unordered_set<PayloadType>`

Add in same header or a dedicated `PayloadTypeHash.h`:

```cpp
struct PayloadTypeHash {
  std::size_t operator()(PayloadType t) const noexcept {
    return std::hash<uint8_t>{}(static_cast<uint8_t>(t));
  }
};
using PayloadTypeSet = std::unordered_set<PayloadType, PayloadTypeHash>;
```

Use `PayloadTypeSet` everywhere instead of `std::unordered_set<PayloadType>`.

---

## Phase 5: Startup type-compatibility validation

**File:** `src/controller/MLDPPVXSController.cpp` (after route table build)

1. Build `name_to_produced` map: reader instance name → `PayloadTypeSet` (from `reader->producedPayloadTypes()`) + processor output name → `PayloadTypeSet` (from `processor->producedPayloadTypes()`)
2. For each route entry, for each reader in `from`:
   - If reader == "all": check ALL known readers' `producedPayloadTypes()` against writer's `acceptedPayloadTypes()`
   - Otherwise: resolve name → `PayloadTypeSet`, check intersection with writer's `acceptedPayloadTypes()`
   - If writer's set non-empty and intersection empty → throw with typed error listing mismatched `PayloadType` enumerators

---

## Phase 6: Remove all-to-all from wizard/CLI

| File | Change |
|---|---|
| `include/config/wizard.h` | Remove `bool routing_all_to_all` field |
| `src/config/wizard_panel.cpp` | Remove "All-to-All Routing" checkbox + Maybe conditional |
| `src/config/wizard.cpp:295` | Remove `!st.routing_all_to_all &&` condition; always emit routing |
| `src/config/wizard.cpp:599` | Remove `st.routing_all_to_all = false` |
| `src/config/edit.cpp:172` | Change to `if (st.routing.empty())` |
| `src/config/edit.cpp:365` | Remove `st.routing_all_to_all = true` |

---

## Phase 7: Fix tests and configs

**Integration tests needing `routing:` block added:**
- `mldppvxs_controller_blocking_queue_test.cpp`
- `mldppvxs_controller_ds_metadata_writer_integration_test.cpp`
- `mldppvxs_controller_epics_archiver_periodic_tail_integration_test.cpp`
- `mldppvxs_controller_hdf5_provenance_test.cpp`
- `mldppvxs_controller_mldp_writer_integration_test.cpp`
- `mldppvxs_controller_reader_lifecycle_test.cpp`
- `mldppvxs_controller_slac_calendar_integration_test.cpp`

**Unit tests to update:**
- `route_table_test.cpp`: rewrite all-to-all tests → verify empty routes rejects all
- `route_table_config_test.cpp`: `NoRoutingConfig_EmptyEntries` → expect throw
- `wizard_test.cpp`: remove `RoutingAllToAllOmitsRoutingBlock`, remove `routing_all_to_all` refs
- `edit_test.cpp`: update `routing_all_to_all` references

**New tests:**
- Controller throws on missing routing
- Type-compatibility validation (incompatible assignment throws)
- Empty `acceptedSourceTypes()` passes all types

**Example configs to update:** `config.yaml`, `config-hdf5-ingest.yaml`, `config-archiver-historical.yaml`

---

## Phase 8: Update `docs/guides/configuration.md`

The configuration reference must be rewritten to reflect mandatory routing and serve as a single point of truth where users find exactly what they need.

### 8.1 — Mark `routing:` as **required** (not optional)

**File:** `docs/guides/configuration.md` line 30

Change:
```yaml
routing:        # optional — selective reader-to-writer dispatch
```
To:
```yaml
routing:        # REQUIRED — explicit reader-to-writer dispatch (no default all-to-all)
```

Update the `## routing: Block` section (line 491-585):
- Remove "Optional" label, state: **Required. Every writer must have a routing entry.**
- Remove the "No routing: block → All-to-all dispatch" row from the behavior table (line 570)
- Change "Writer not listed → receives nothing (startup warning)" → "Writer not listed → **startup error, application exits**"
- Add note: `from: [all]` is still valid (accept all readers) but must be explicit

### 8.2 — Add accepted source types documentation per writer

After each writer section, add a "Compatible Sources" callout showing which reader/processor types can feed that writer:

**After `writer.mldp[]` section (~line 141):**
```markdown
**Compatible source types:** `epics-pvxs`, `epics-base`, `epics-archiver`, `hdf5-bsas-gen1`, `echo`, `linear-transform`, `python-script`
```

**After `writer.hdf5[]` section (~line 175):**
```markdown
**Compatible source types:** `epics-pvxs`, `epics-base`, `epics-archiver`, `hdf5-bsas-gen1`, `echo`, `linear-transform`, `python-script`
```

**After `writer.mldp-pv-metadata[]` section (~line 204):**
```markdown
**Compatible source types:** `epics-pvxs`, `epics-base`, `epics-ds-metadata`
```

**After `writer.mldp-configuration[]` section (~line 233):**
```markdown
**Compatible source types:** `slac-calendar`
```

### 8.3 — Add full default configuration example with anchors

Add a new section after "Example Configurations" (line 708) that provides a **complete annotated reference config** linking each block to its documentation section:

```markdown
## Full Reference Configuration

A complete configuration demonstrating all blocks with explicit routing. Each section links to its detailed documentation above.

→ [`docs/examples/full-reference-config.yaml`](../examples/full-reference-config.yaml)
```

Create **`docs/examples/full-reference-config.yaml`** — a self-contained annotated example with:
- All writer types (mldp, hdf5, mldp-pv-metadata, mldp-configuration)
- All reader types (epics-pvxs, epics-base, epics-archiver, epics-ds-metadata, slac-calendar)
- Processors (python-processor)
- Enrichers (static-metadata, shard-slot)
- Explicit routing wiring each writer to compatible readers
- Comments linking each block to its doc anchor: `# See: docs/guides/configuration.md#writer-mldp`

### 8.4 — Add quick-reference routing table

Insert a new subsection **"Routing Quick Reference"** inside the `routing:` block section showing which writer types accept which reader types at a glance:

```markdown
### Routing Compatibility Matrix

| Writer type | Compatible reader/processor types |
|---|---|
| `mldp` | `epics-pvxs`, `epics-base`, `epics-archiver`, `hdf5-bsas-gen1`, `echo`, `linear-transform`, `python-script` |
| `hdf5` / `hdf5-merge` | `epics-pvxs`, `epics-base`, `epics-archiver`, `hdf5-bsas-gen1`, `echo`, `linear-transform`, `python-script` |
| `mldp-pv-metadata` | `epics-pvxs`, `epics-base`, `epics-ds-metadata` |
| `mldp-configuration` | `slac-calendar` |

Assigning an incompatible reader to a writer causes a **startup error**.
```

### 8.5 — Add anchors for direct linking

Add HTML anchors to each writer/reader/processor/enricher section header so the full reference config and other docs can deep-link:

- `### writer.mldp[] — MLDP Ingestion Writer {#writer-mldp}`
- `### writer.hdf5[] — HDF5 Storage Writer {#writer-hdf5}`
- `### writer.mldp-pv-metadata[] {#writer-mldp-pv-metadata}`
- `### writer.mldp-configuration[] {#writer-mldp-configuration}`
- `### Common Reader Keys {#reader-epics}`
- `### epics-archiver Reader {#reader-epics-archiver}`
- `### epics-ds-metadata Reader {#reader-epics-ds-metadata}` (already has anchor)
- `### slac-calendar Reader {#reader-slac-calendar}` (already has anchor)
- `## processors: Block {#processors-block}` (already has anchor)
- `## routing: Block {#routing-block}`
- `### Global enrichers {#enrichers}`

### 8.6 — Document wizard usage for routing

Add a subsection under `## routing: Block` titled **"Configuring Routing via the Wizard"**:

```markdown
### Configuring Routing via the Wizard

The interactive wizard (`mldp_pvxs_driver config wizard`) includes a **Routing** panel in the sidebar tree. Since routing is mandatory, the wizard enforces that every writer has at least one routing entry before allowing save.

**Workflow:**
1. Add writers and readers first (they define the names available for routing)
2. Navigate to the **Routing** sidebar item
3. For each writer, specify which readers feed it
4. The wizard validates compatibility — incompatible reader→writer assignments show an error indicator
5. Save generates the `routing:` YAML block automatically

**CLI alternatives:**
```bash
# Add a routing entry interactively
mldp_pvxs_driver config add routing config.yaml

# Remove a routing entry by writer name
mldp_pvxs_driver config remove routing config.yaml --name mldp_main

# List current routing configuration
mldp_pvxs_driver config list config.yaml
```

**Validation:** Run `mldp_pvxs_driver config validate config.yaml` or `mldp_pvxs_driver --dry-run -c config.yaml` to verify routing is complete and compatible before starting the driver.
```

### 8.7 — Update existing example configs

All example files must include explicit routing:

| File | Routing to add |
|---|---|
| `docs/examples/config-mldp-only.yaml` | `routing: { <writer_name>: { from: [<reader_name>] } }` |
| `docs/examples/config-mldp-and-hdf5.yaml` | Route each writer to appropriate readers |
| `docs/examples/config-epics-archiver.yaml` | Route archiver reader to writer |
| `docs/examples/pvxs-hdf5-demo.yaml` | Already has routing — remove comment "Remove this section to use all-to-all routing instead" (line 44) |

---

## Verification

1. Build in devcontainer: `cmake --build build`
2. Run unit tests: `ctest --test-dir build`
3. `--dry-run` with config missing `routing:` → error exit
4. `--dry-run` with incompatible route assignment → error exit
5. `--dry-run` with valid explicit routing → success
6. Verify all doc anchors resolve (no broken `#` links in configuration.md)
7. Verify `full-reference-config.yaml` passes `--dry-run` validation
