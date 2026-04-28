# TODO: Merge Multiple Root-Sources into Single Writer Output

## Problem

Each root-source currently produces a separate writer output (e.g., one HDF5 file per root-source).
Some users need data from multiple root-sources stored together (e.g., one HDF5 file, one group per root-source).
Not all writers can support this; the feature must be opt-in and gracefully ignored when unsupported.

## Goal

Allow users to configure a writer to accept multiple root-sources and merge them into a single output object.
Writers that do not support merging continue to work unchanged.

## Requirements

- **Capability flag**: writer interface declares `supports_multi_root_source() -> bool`
- **Opt-in config**: `merge_root_sources: true` at writer config level (default: `false`)
- **Graceful fallback**: unsupported writer logs warning, ignores flag, uses 1-output-per-source behavior
- **HDF5 support**: merged mode maps each root-source to a separate HDF5 group within one file
- **Controller awareness**: router groups root-sources destined for same merged writer output
- **Backward compatible**: no behavior change when flag is absent or false

## Proposed Configuration

```yaml
writers:
  - type: hdf5
    merge_root_sources: true        # opt-in merge mode
    output_path: /data/merged.h5
    root_sources:
      - source_a
      - source_b
```

HDF5 file layout when merged:

```
merged.h5
├── source_a/
│   ├── dataset_1
│   └── dataset_2
└── source_b/
    ├── dataset_1
    └── dataset_2
```

## Implementation Plan

### Step 1 — Writer interface capability

- Add `virtual bool supports_multi_root_source() const { return false; }` to `IWriter`
- No existing writers break (default returns false)
- Writers that override to `true` **must** implement merge semantics: all root-sources arriving
  at that writer instance are collapsed into one output container (e.g. one HDF5 file, one stream).
  The writer is solely responsible for this aggregation; the controller dispatches batches
  unchanged — it only checks the flag to decide whether to warn.

### Step 2 — Writer config extension

- Add `merge_root_sources: bool` field to writer base config struct
- Parse from YAML; default false
- At startup, if `merge_root_sources: true` and `supports_multi_root_source()` returns false:
  log **error** and throw — misconfiguration must not silently produce split output

### Step 3 — HDF5 writer merge support

- Override `supports_multi_root_source()` → `true`
- When merge enabled: open single output file, create/reuse group per root-source name
- When merge disabled: existing behavior unchanged
- File rotation logic must account for all root-sources sharing the file

### Step 4 — Controller / router integration

- Before dispatching batches: check if target writer has merge enabled
- Group batches from multiple root-sources into single writer call (or tag batch with source name)
- Single-source path unchanged

### Step 5 — Tests

- Unit: `HDF5Writer` merge mode creates correct group hierarchy
- Unit: unsupported writer ignores flag, emits warning
- Integration: two root-sources → one HDF5 file, two groups, correct data in each
- Regression: existing single-source tests still pass

## Open Questions

1. File rotation with merged sources: rotate when any source triggers rotation, or all?
2. Concurrent writes: one mutex per file or per group?
3. Schema conflicts: what if two sources produce datasets with same name but different type?

## Related

- `docs/todo/reader-writer-routing.md` — routing infrastructure this feature depends on
- `docs/writers/writers-implementation.md` — writer interface reference
- Branch: `feature/merge-root-source-single-writer`
