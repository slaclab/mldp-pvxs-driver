# Task: Update README.md and hdf5-writer.md After Writer Split

## Depends On

`split-hdf5-writer-types.md` — complete that task first. This task updates docs to reflect the new two-type YAML schema.

---

## Goal

Two doc updates:

1. **`README.md`** — global config reference: replace `hdf5` single block with both `hdf5` and `hdf5-merge` blocks; remove `merge-root-sources` key.
2. **`docs/writers/hdf5-writer.md`** — full writer doc: reflect new class hierarchy, remove merge-mode section that references `merge-root-sources`, add `hdf5-merge` type documentation.

---

## README.md Changes

### Section: writer.hdf5 block

Replace current single `hdf5` block in the config table and YAML example:

**Before:**
```yaml
  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
      max-file-age-s: 3600
      max-file-size-mb: 512
      flush-interval-ms: 1000
      compression-level: 0
      merge-root-sources: false   # (if present)
```

**After — two separate blocks:**
```yaml
  # ========== HDF5 Per-Source Writer (one file per root_source) ==========
  hdf5:
    - name: hdf5_local                      # required; unique instance name
      base-path: /data/hdf5                 # required; directory for HDF5 output files
      max-file-age-s: 3600                  # optional; default: 3600; rotate after N seconds
      max-file-size-mb: 512                 # optional; default: 512; rotate after N MiB
      flush-interval-ms: 1000              # optional; default: 1000; flush thread period in ms
      compression-level: 0                  # optional; default: 0; DEFLATE level 0–9

  # ========== HDF5 Merge Writer (all sources share one file, one group per source) ==========
  hdf5-merge:
    - name: hdf5_merged                     # required; unique instance name
      base-path: /data/hdf5-merged          # required; directory for HDF5 output files
      max-file-age-s: 3600                  # optional; default: 3600
      max-file-size-mb: 512                 # optional; default: 512
      flush-interval-ms: 1000              # optional; default: 1000
      compression-level: 0                  # optional; default: 0
```

### Writer table

Add row for `hdf5-merge` type in the top-level writer key table:

| Key | Required | Description |
|-----|----------|-------------|
| `writer.hdf5` | no | HDF5 writer — one file per root_source |
| `writer.hdf5-merge` | no | HDF5 merge writer — all sources in one file, one HDF5 group per source |

---

## hdf5-writer.md Changes

### Overview section

Update registered types from one to two:

- `"hdf5"` → `HDF5WriterPerSource`
- `"hdf5-merge"` → `HDF5WriterMerge`

### Class hierarchy section

Replace:
```
HDF5Writer                  (factory wrapper — owns unique_ptr<IWriter> impl_)
```

With nothing (shim deleted). New hierarchy:
```
IWriter
└── HDF5WriterBase          (abstract — shared queue, threads, tabular buffers)
      ├── HDF5WriterPerSource   (type "hdf5" — one file per root_source via HDF5FilePool)
      └── HDF5WriterMerge       (type "hdf5-merge" — all sources share one H5::H5File)
```

### Configuration section

Replace single YAML block + `merge-root-sources` row with two separate YAML blocks (one per type), matching README style. Remove `merge-root-sources` from table.

**hdf5 (per-source):**
```yaml
writer:
  hdf5:
    - name: hdf5_local          # required
      base-path: /data/hdf5     # required
      max-file-age-s: 3600      # optional; default: 3600
      max-file-size-mb: 512     # optional; default: 512
      flush-interval-ms: 1000   # optional; default: 1000
      compression-level: 0      # optional; 0–9, default: 0
```

**hdf5-merge (merge):**
```yaml
writer:
  hdf5-merge:
    - name: hdf5_merged         # required
      base-path: /data/hdf5     # required
      max-file-age-s: 3600      # optional; default: 3600
      max-file-size-mb: 512     # optional; default: 512
      flush-interval-ms: 1000   # optional; default: 1000
      compression-level: 0      # optional; 0–9, default: 0
```

### Merge Mode section

- Remove the `merge-root-sources: true` enable snippet.
- Update heading from "Merge Mode" to "hdf5-merge Writer".
- Add note: use `writer.hdf5-merge` key to activate; no flag needed.
- Update the "Enable merge mode" snippet to new YAML form.

### Examples section

Update "Two readers, PV filter routing, single merged file" example:

**Before:**
```yaml
writer:
  hdf5:
    - name: hdf5_merged
      base-path: /data/merged
      merge-root-sources: true
```

**After:**
```yaml
writer:
  hdf5-merge:
    - name: hdf5_merged
      base-path: /data/merged
```

### Lifecycle table

Remove row: `HDF5Writer(config) — Selects HDF5WriterPerSource or HDF5WriterMerge based on mergeRootSources.`

Replace with two rows:
- `HDF5WriterPerSource(config)` — opens `HDF5FilePool`; one file per source.
- `HDF5WriterMerge(config)` — opens single shared merge file.

### Key Files table

Remove `HDF5Writer.h` row. Add note that each concrete class now carries its own `REGISTER_WRITER` macro.

### Config Migration Guide (new section at bottom)

Add section documenting before/after YAML for users upgrading:

```
## Config Migration

If upgrading from a build that used `merge-root-sources`:

Before:
  writer:
    hdf5:
      - name: hdf5_merged
        base-path: /data/hdf5
        merge-root-sources: true

After:
  writer:
    hdf5-merge:
      - name: hdf5_merged
        base-path: /data/hdf5
```

---

## Completion Criteria

- [ ] `README.md` writer table includes both `hdf5` and `hdf5-merge` rows.
- [ ] `README.md` YAML example shows both blocks; `merge-root-sources` absent.
- [ ] `hdf5-writer.md` class hierarchy shows no shim wrapper.
- [ ] `hdf5-writer.md` config section has two YAML blocks, no `merge-root-sources` key.
- [ ] `hdf5-writer.md` merge-mode example uses `writer.hdf5-merge`.
- [ ] `hdf5-writer.md` lifecycle table updated.
- [ ] `hdf5-writer.md` key files table updated.
- [ ] Migration guide present in `hdf5-writer.md`.
