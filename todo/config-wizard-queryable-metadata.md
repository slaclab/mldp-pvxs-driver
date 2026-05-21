# TODO: Config Wizard — Queryable and Metadata Support

## Summary

The interactive config wizard (`src/config/wizard.cpp`) does not yet handle two areas added in the `feature/pv-metadata` branch:

1. **`queryable:` block** — `MLDPQueryClient` and `MLDPAnnotationQueryClient` pool configuration
2. **Reader `metadata:` fields** — reader-level and per-PV static metadata key-value pairs

---

## 1. Queryable Block

### What needs to be added

**State struct** (`wizard_internal.h`): add a `queryable` sub-state with fields for `mldp` and `mldp-annotation` pool configs (URLs, min/max connections).

**YAML serialiser** (`serializeToYaml` in `wizard.cpp`): emit the `queryable:` block when any queryable is configured:

```yaml
queryable:
  mldp:
    mldp-pool:
      ingestion-url: grpc://ingest:50051
      query-url:     grpc://query:50052
      min-conn: 1
      max-conn: 2
  mldp-annotation:
    mldp-annotation-pool:
      annotation-url: grpc://annotation-host:50053
      min-conn: 1
      max-conn: 2
```

**YAML deserialiser** (`loadFromConfig` in `wizard.cpp`): parse the `queryable:` child of the root config into the new state fields.

**UI panels** (`wizard_panel.cpp` / `wizard_ui.hpp`): add a "Queryable" step that lets the user configure each client type (toggle enabled, set URLs and pool sizes).

### Key files

| File | Change needed |
|---|---|
| `src/config/wizard_internal.h` | Add `QueryableState` struct; include in `WizardState` |
| `src/config/wizard.cpp` | Serialise/deserialise `queryable:` block |
| `src/config/wizard_panel.cpp` | Add queryable configuration panel |
| `src/config/wizard_ui.hpp` | Wire new panel into wizard step list |

---

## 2. Reader Metadata Fields

### What needs to be added

**Reader state struct** (`wizard_internal.h`): add `metadata` map (`std::unordered_map<std::string,std::string>`) at the reader level and inside `PVEntry`.

**YAML serialiser** (`wizard.cpp`): emit `metadata:` block under the reader and under each PV entry when non-empty:

```yaml
reader:
  - epics-pvxs:
      name: my_reader
      metadata:
        facility: lcls
        subsystem: bpms
      pvs:
        - name: MY:PV
          metadata:
            signal_type: scalar
            subsystem: override_bpms
```

**YAML deserialiser** (`wizard.cpp`): parse `metadata:` from the reader node and from each PV node into the state maps.

**UI panels** (`wizard_panel.cpp`): add an editable key-value table for reader-level metadata and per-PV metadata in the PV editor step.

### Key files

| File | Change needed |
|---|---|
| `src/config/wizard_internal.h` | Add `metadata` map to `ReaderState` and `PVEntry` |
| `src/config/wizard.cpp` | Serialise/deserialise reader and per-PV `metadata:` |
| `src/config/wizard_panel.cpp` | Add metadata editor UI (key-value table component) |

---

## Reference

- Queryable config schema: `docs/guides/configuration.md` § Queryable block
- Queryable client API: `docs/dev/query-client.md`
- Reader metadata semantics: `docs/guides/configuration.md` § EPICS reader — merge rules
- Architecture: `docs/reference/architecture.md` § QueryableFactory
