# Task: Split HDF5Writer into Two First-Class Writer Types

## Goal

Remove `HDF5Writer` (the factory shim) and `merge-root-sources` config flag.  
Promote `HDF5WriterMerge` and `HDF5WriterPerSource` to independent, directly-registered writer types with distinct YAML keys.

**New YAML keys:**
- `writer.hdf5` → `HDF5WriterPerSource` (one file per root source, current default)
- `writer.hdf5-merge` → `HDF5WriterMerge` (all sources in one file, current merge mode)

---

## Motivation

- `mergeRootSources` is a binary mode switch disguised as config — two fundamentally different behaviours in one class.
- Users cannot configure both modes simultaneously for different writer instances.
- The shim `HDF5Writer` + `impl_` indirection adds complexity with zero benefit.
- Removing the flag simplifies `HDF5WriterConfig` and eliminates an if-branch at construction.

---

## Affected Files

| File | Change |
|---|---|
| `include/writer/hdf5/HDF5WriterConfig.h` | Remove `mergeRootSources` field and `HDF5MergeRootSourcesKey` constant |
| `src/writer/hdf5/HDF5WriterConfig.cpp` | Remove `mergeRootSources` parse line |
| `include/writer/hdf5/HDF5Writer.h` | **Delete file** |
| `src/writer/hdf5/HDF5Writer.cpp` | **Delete file** |
| `include/writer/hdf5/HDF5WriterMerge.h` | Add `REGISTER_WRITER("hdf5-merge", HDF5WriterMerge)` + factory constructor |
| `src/writer/hdf5/HDF5WriterMerge.cpp` | Add factory constructor `(const config::Config&, shared_ptr<Metrics>)` |
| `include/writer/hdf5/HDF5WriterPerSource.h` | Add `REGISTER_WRITER("hdf5", HDF5WriterPerSource)` + factory constructor |
| `src/writer/hdf5/HDF5WriterPerSource.cpp` | Add factory constructor `(const config::Config&, shared_ptr<Metrics>)` |
| `include/writer/WriterConfig.h` | Add `WriterHdf5MergeKey[] = "hdf5-merge"` constant |
| `src/writer/WriterConfig.cpp` | Add `hdf5-merge` sequence validation block (mirrors `hdf5` block) |
| `src/controller/MLDPPVXSControllerConfig.cpp` | Enumerate `hdf5-merge` entries alongside `hdf5` entries |
| `src/cli/ConfigPrinter.cpp` | Print `hdf5-merge` block if present |
| `CMakeLists.txt` (hdf5 target) | Remove `HDF5Writer.cpp` from sources |
| `test/writer/hdf5/hdf5_writer_test.cpp` | Replace `HDF5Writer` usage with direct `HDF5WriterMerge` / `HDF5WriterPerSource` |
| `test/writer/hdf5/hdf5_writer_config_test.cpp` | Remove `merge-root-sources` tests; add type-string factory tests |
| `config.yaml` | Remove `merge-root-sources`; update comments |
| `docs/examples/config-mldp-and-hdf5.yaml` | Update example |
| `.vscode/config-hdf5-*.yaml` | Update any local test configs if present |

---

## Step-by-Step Plan

### Step 1 — Add factory constructors to concrete classes

Both `HDF5WriterMerge` and `HDF5WriterPerSource` need the factory-compatible constructor:

```cpp
explicit HDF5WriterXxx(const config::Config&             node,
                       std::shared_ptr<metrics::Metrics> metrics = nullptr);
```

Implementation: call `HDF5WriterConfig::parse(node)`, then delegate to existing typed constructor (`HDF5WriterBase(std::move(cfg), std::move(metrics))`).

Do this in both `.h` and `.cpp` files.

### Step 2 — Register each class directly

In `HDF5WriterPerSource.h` (inside class body):
```cpp
REGISTER_WRITER("hdf5", HDF5WriterPerSource)
```

In `HDF5WriterMerge.h` (inside class body):
```cpp
REGISTER_WRITER("hdf5-merge", HDF5WriterMerge)
```

### Step 3 — Strip `mergeRootSources` from `HDF5WriterConfig`

- Remove field `bool mergeRootSources{false}` from struct.
- Remove constant `HDF5MergeRootSourcesKey`.
- Remove parse line in `HDF5WriterConfig::parse()`.

### Step 4 — Delete `HDF5Writer` shim

- Delete `include/writer/hdf5/HDF5Writer.h`.
- Delete `src/writer/hdf5/HDF5Writer.cpp`.
- Remove from `CMakeLists.txt` sources list.

### Step 5 — Add `hdf5-merge` to `WriterConfig` and controller

**`WriterConfig.h`**: add constant:
```cpp
inline constexpr char WriterHdf5MergeKey[] = "hdf5-merge";
```

**`WriterConfig.cpp`**: add validation block for `writer.hdf5-merge` — identical structure to the `hdf5` block, calls `HDF5WriterConfig::parse()`.

**`MLDPPVXSControllerConfig.cpp`**: enumerate `hdf5-merge` entries and push `{"hdf5-merge", node}` into `writerEntries_`.

### Step 6 — Update `ConfigPrinter`

Add `hdf5-merge` block printing, guarded by `MLDP_PVXS_HDF5_ENABLED`.

### Step 7 — Update tests

- `hdf5_writer_test.cpp`: replace `HDF5Writer{cfg}` with `HDF5WriterPerSource{cfg}` or `HDF5WriterMerge{cfg}` as appropriate. Add factory round-trip test via `WriterFactory::create("hdf5", ...)` and `WriterFactory::create("hdf5-merge", ...)`.
- `hdf5_writer_config_test.cpp`: remove `merge-root-sources` round-trip tests. Add test that `merge-root-sources` key is now rejected (unknown key) or simply ignored, depending on config parser strictness.

### Step 8 — Update YAML examples and docs

- `config.yaml`: remove `merge-root-sources` key from hdf5 block; add commented `hdf5-merge` example section.
- `docs/examples/config-mldp-and-hdf5.yaml`: add `hdf5-merge` example.

---

## Config Migration Guide (for users)

**Before:**
```yaml
writer:
  hdf5:
    - name: hdf5_merged
      base-path: /data/hdf5
      merge-root-sources: true
```

**After:**
```yaml
writer:
  hdf5-merge:
    - name: hdf5_merged
      base-path: /data/hdf5
```

Per-source (default, unchanged):
```yaml
writer:
  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
```

---

## Invariants / Constraints

- `HDF5WriterBase`, `HDF5WriterDetail`, `HDF5FilePool`, `HDF5WriterMetrics` are **untouched** — shared infrastructure stays as-is.
- Both writer types still honour all other config keys (`max-file-age-s`, `max-file-size-mb`, `flush-interval-ms`, `compression-level`).
- `supports_multi_root_source()` returns `true` on both concrete classes (move override from shim).
- Build guard `MLDP_PVXS_HDF5_ENABLED` remains; both new keys are gated identically.
- No change to `HDF5WriterBase` API — only concrete class headers/sources change.

---

## Completion Criteria

- [ ] `HDF5Writer.h` and `HDF5Writer.cpp` deleted; project builds clean.
- [ ] `merge-root-sources` absent from all headers, sources, YAML, and tests.
- [ ] `WriterFactory::create("hdf5", ...)` instantiates `HDF5WriterPerSource`.
- [ ] `WriterFactory::create("hdf5-merge", ...)` instantiates `HDF5WriterMerge`.
- [ ] `WriterConfig::validate()` accepts both `writer.hdf5` and `writer.hdf5-merge` sequences.
- [ ] All existing tests pass; new factory round-trip tests added.
- [ ] Config migration guide accurate.
