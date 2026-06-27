# HDF5 Storage Writer

> **Related:** [Writers Overview](writers-implementation.md) | [Architecture](../reference/architecture.md)

## Overview

Two HDF5 writer types store incoming event batches as HDF5 datasets on disk. Both are available only when CMake option `MLDP_PVXS_ENABLE_HDF5=ON` is used.

| Type | Class | Behaviour |
|------|-------|-----------|
| `"hdf5"` | `HDF5WriterPerSource` | One file per `root_source` via `HDF5FilePool` |
| `"hdf5-merge"` | `HDF5WriterMerge` | All sources share one file; each source gets its own HDF5 group |

## Build Option & Required Libraries

- **Build option:** `-DMLDP_PVXS_ENABLE_HDF5=ON`
- **Compile definition emitted by CMake:** `MLDP_PVXS_HDF5_ENABLED`
- **Required libraries/components:**
  - HDF5 C++ library target (`hdf5_cpp-static` / `hdf5_cpp-shared`)
  - zlib (enabled through HDF5 build flags)

## Internal Architecture

### Class hierarchy

Each concrete class registers itself directly in the writer factory via the `REGISTER_WRITER` macro:

```
IWriter
└── HDF5WriterBase          (abstract — shared queue, threads, tabular buffers)
      ├── HDF5WriterPerSource   (type "hdf5" — one file per root_source via HDF5FilePool)
      └── HDF5WriterMerge       (type "hdf5-merge" — all sources share one H5::H5File)
```

### hdf5 — HDF5WriterPerSource (one file per source)

```
push() → bounded MPSC deque
               ↓ (writerThread)
        writeFrameImpl()  →  HDF5FilePool.acquire(source)
                                       ↓
                               H5::H5File (per source)
                                       ↓
                      flushThread → doFlushAll() → pool->flushAll()
```

### hdf5-merge — HDF5WriterMerge (all sources, one file)

```
push() → bounded MPSC deque
               ↓ (writerThread)
        writeFrameImpl()  →  checkMergeRotation()
                                       ↓
                              lock mergeFileMutex_
                                       ↓
                           single shared H5::H5File
                           datasets under /<source>/ group
                                       ↓
                      flushThread → doFlushAll() → mergeFile_->flush()
```

### Shared base (HDF5WriterBase)

- **Writer thread**: drains bounded MPSC queue; calls `writeFrameImpl()` (virtual) per frame or `flushTabularBufferImpl()` (virtual) on end-of-batch-group.
- **Flush thread**: calls `doFlushAll()` (virtual) every `flush-interval-ms`.
- **Tabular buffers**: `TabularBuffer` per source accumulated entirely in the base; subclass called only at flush.
- **Back-pressure**: queue capped at `queue-capacity` (default 8192). `push()` **blocks indefinitely** until space is available — data is never dropped. Only returns `false` when the writer is stopping (double Ctrl+C / `forceStop()`).
- **Destructor safety**: `~HDF5WriterBase()` does **not** call `stop()` (pure-virtual `doStop()` would be invalid at base-dtor time). Each subclass destructor calls `stop()` while its vtable is still live.

### Pure-virtual hooks

| Hook | Called by | Purpose |
|------|-----------|---------|
| `writeFrameImpl(source, frame, batchSeq)` | `writerLoop` | Write one DataBatch frame |
| `flushTabularBufferImpl(source, buf)` | `writerLoop`, `accumulateTabularFrame` | Write accumulated NTTable rows |
| `doFlushAll()` | `flushLoop` | Flush open file(s) to disk |
| `doStart()` | `start()` | Open pool or merge file |
| `doStop()` | `stop()` | Close pool or merge file after threads join |

## HDF5 File Layout

Two layouts exist depending on PV type:

**Columnar (scalar / waveform PVs)** — flat root-level datasets:
```
/ (root)
├── timestamps        int64   ns-since-epoch   shape=(N,) unlimited+chunked
├── <col_name_0>      …       type from DataFrame column
└── …
```

**NTTable (`slac-bsas-table` mode)** — one HDF5 group per source:
```
/ (root)
└── <source>/
    ├── secondsPastEpoch   int64   [N_rows]   unlimited+chunked
    ├── nanoseconds        int64   [N_rows]   unlimited+chunked
    ├── <col_0>            typed   [N_rows]   unlimited+chunked
    └── …
```

One file per `root_source`. File name format:

```
<base-path>/<safe_source>_<YYYYMMDDTHHMMSSz>.h5
```

Characters outside `[A-Za-z0-9._-]` in the source name are replaced by `_`.

All datasets are 1-D, shape `(N,)`, `maxDims=H5S_UNLIMITED`, chunked at 1024 elements, optionally DEFLATE-compressed. Array (waveform) datasets are 2-D, shape `(N_samples, array_len)`.

### Scalar PVs

Each update produces one DataFrame → one new row appended to each dataset.

| PV type | HDF5 dataset name | HDF5 type |
|---------|-------------------|-----------|
| Bool | `<pv_name>` | `NATIVE_HBOOL` |
| Int8 / Int16 / Int32 | `<pv_name>` | `NATIVE_INT32` |
| UInt8 / UInt16 / UInt32 | `<pv_name>` | `NATIVE_INT32` (reinterpret) |
| Int64 | `<pv_name>` | `NATIVE_INT64` |
| UInt64 | `<pv_name>` | `NATIVE_INT64` (reinterpret) |
| Float32 | `<pv_name>` | `NATIVE_FLOAT` |
| Float64 | `<pv_name>` | `NATIVE_DOUBLE` |
| String | `<pv_name>` | variable-length string |

`timestamps` stores `epochSeconds × 10⁹ + nanoseconds` as `int64`.

Example for one scalar PV:
```
/
├── timestamps    int64   [N]
└── <pv_name>     double  [N]
```

### Scalar Array PVs (waveforms)

Each update produces one DataFrame with one `*ArrayColumn`. The writer creates a 2-D dataset `(N_samples, array_len)` where:
- `N_samples` grows by 1 per update
- `array_len` is fixed from `ArrayDimensions.dims[0]` on first write

**Constraint:** array length must be consistent across updates — the dataset shape is fixed at creation. For EPICS waveform records this is always true (`NELM` is constant).

| PV type | HDF5 dataset name | HDF5 type |
|---------|-------------------|-----------|
| Float64A | `<pv_name>` | `NATIVE_DOUBLE` shape `(N, len)` |
| Float32A | `<pv_name>` | `NATIVE_FLOAT` shape `(N, len)` |
| Int32A / UInt32A | `<pv_name>` | `NATIVE_INT32` shape `(N, len)` |
| Int64A / UInt64A | `<pv_name>` | `NATIVE_INT64` shape `(N, len)` |
| BoolA | `<pv_name>` | `NATIVE_HBOOL` shape `(N, len)` |

Example for a waveform PV with 256 elements, 10 updates:
```
/
├── timestamps    int64   [10]
└── <pv_name>     double  [10, 256]
```

### BSAS NTTable (`slac-bsas-table` mode)

Each NTTable update carries `rowCount` rows. All datasets live inside an HDF5 group named after the PV source. Timestamp columns (`tsSeconds` / `tsNanos` fields) are stored as `secondsPastEpoch` and `nanoseconds` datasets; all other columns are stored by their field name.

Supported BSAS column types: Float64, Float32, Int32. String columns are silently dropped.

Example for a BSAS table with columns `secondsPastEpoch`, `nanoseconds`, `PV_A` (Float64), `PV_B` (Int32), `PV_C` (Float32), PV source `test:bsas_table`:
```
/ (root)
└── test:bsas_table/
    ├── secondsPastEpoch   int64   [rowCount × updates]
    ├── nanoseconds        int64   [rowCount × updates]
    ├── PV_A               double  [rowCount × updates]
    ├── PV_B               int32   [rowCount × updates]
    └── PV_C               float   [rowCount × updates]
```

### Gen1 NTTable (`slac-bsas-table` mode, Gen1 BSAS)

Gen1 BSAS NTTable PVs use the same `slac-bsas-table` type and produce the same per-source group layout. Column names from the NTTable schema are used directly as dataset names (sanitized to valid HDF5 identifiers: non-`[A-Za-z0-9_]` characters replaced with `_`).

Example for a Gen1 CU-HXR table with source `CU-HXR`:
```
/ (root)
└── CU-HXR/
    ├── secondsPastEpoch        int64   [N_rows]
    ├── nanoseconds             int64   [N_rows]
    ├── ACCL_IN20_300_L0A_ACUHBR  double  [N_rows]
    ├── ACCL_IN20_300_L0A_PCUHBR  double  [N_rows]
    └── …  (one dataset per signal column)
```

Configuration:
```yaml
- name: CU-HXR
  option:
    type: slac-bsas-table
    tsSeconds: secondsPastEpoch
    tsNanos: nanoseconds
```

### Written vs. not-written summary

| DataFrame field | Written to HDF5 | HDF5 type |
|----------------|----------------|-----------|
| `doubleColumns` | ✅ | `NATIVE_DOUBLE` 1-D |
| `floatColumns` | ✅ | `NATIVE_FLOAT` 1-D |
| `int32Columns` | ✅ | `NATIVE_INT32` 1-D |
| `int64Columns` | ✅ | `NATIVE_INT64` 1-D |
| `boolColumns` | ✅ | `NATIVE_HBOOL` 1-D |
| `stringColumns` | ✅ | variable-length string 1-D |
| `doubleArrayColumns` | ✅ | `NATIVE_DOUBLE` 2-D `(N, len)` |
| `floatArrayColumns` | ✅ | `NATIVE_FLOAT` 2-D `(N, len)` |
| `int32ArrayColumns` | ✅ | `NATIVE_INT32` 2-D `(N, len)` |
| `int64ArrayColumns` | ✅ | `NATIVE_INT64` 2-D `(N, len)` |
| `boolArrayColumns` | ✅ | `NATIVE_HBOOL` 2-D `(N, len)` |

## Behavior With Pre-Existing Files

`HDF5FilePool` always opens new files with `H5F_ACC_TRUNC`. Consequences:

| Scenario | Result |
|----------|--------|
| Pre-existing `.h5` files in `base-path` | **Ignored** — pool has no scan/discovery on startup |
| Same source + same UTC second on restart | Existing file **truncated** (overwritten) |
| Rotation triggered (age or size) | Old entry closed; **new** timestamped file created; old file untouched |
| Two writer instances, same `base-path`, same source, same UTC second | Both open same filename with `TRUNC` → **data corruption** |

**No retention/cleanup policy**: files accumulate in `base-path` indefinitely. External cleanup (cron, logrotate, etc.) is the operator's responsibility.

**No append-to-existing**: restarting the writer always starts a fresh file, never continues writing into a file from a previous run.

## File Rotation

`HDF5FilePool` rotates a file when **either** threshold is exceeded:

| Condition | Config key | Default |
|-----------|-----------|---------|
| File age ≥ threshold | `max-file-age-s` | 3600 s (1 h) |
| Bytes written ≥ threshold | `max-file-size-mb` | 512 MiB |

### How rotation works

Rotation is performed **inside `acquire()`**, the only entry point for obtaining a file handle. The sequence is:

1. Writer calls `pool->acquire(sourceName)`.
2. `acquire()` locks the pool mutex, finds the current `FileEntry`, and calls `needsRotation()`.
3. If the age or size threshold is exceeded:
   - The old file is closed (under `fileMutex`) and renamed from its hidden temp path to its final visible path.
   - A new `FileEntry` with a fresh timestamped file is created and inserted into the pool map.
   - The new entry is returned to the caller.
4. If no threshold is exceeded, the existing entry is returned as-is.

### Why no data is lost during rotation

The writer **must** call `acquire()` before every write operation — there is no way to write to a file without first obtaining its `shared_ptr<FileEntry>`. This design guarantees correctness:

- **Tabular (NTTable) path:** column batches accumulate in a `TabularBuffer` (in-memory, not tied to any file). Only when the `end_of_batch_group` marker arrives does the writer call `acquire()` and then `flushTabularBuffer()`. If rotation triggers at this point, the accumulated rows are written to the **new** file — no partial data is left in the old file.
- **Columnar (scalar/waveform) path:** each `appendFrame()` call is preceded by `acquire()`. Rotation happens before the frame is written, so the frame lands in the new file.
- **Flush thread:** `flushAll()` calls `H5File::flush()` on all currently open files. It does not call `acquire()`, so it never triggers rotation. It only ensures OS/HDF5 buffers are synced to disk.

In all cases, the old file is fully flushed and closed before the new file is returned. Datasets are recreated lazily in the new file via `ensureDataset()` / `ensureDataset2D()`, which check `file.nameExists()` and create fresh chunked datasets on first access. Schema metadata (column names, types) lives in `TabularBuffer` and `HDF5Writer` state, not in `FileEntry`, so it survives rotation.

## Configuration

Under `writer.hdf5[i]` or `writer.hdf5-merge[i]`:

```yaml
writer:
  hdf5:                         # type "hdf5" — HDF5WriterPerSource
    - name: hdf5_local          # required — unique instance name
      base-path: /data/hdf5     # required — output directory
      max-file-age-s: 3600      # optional; default: 3600
      max-file-size-mb: 512     # optional; default: 512
      flush-interval-ms: 1000   # optional; default: 1000
      compression-level: 0      # optional; 0–9, default: 0 (off)
      queue-capacity: 8192      # optional; default: 8192

  hdf5-merge:                   # type "hdf5-merge" — HDF5WriterMerge
    - name: hdf5_merged         # required — unique instance name
      base-path: /data/hdf5     # required — output directory
      max-file-age-s: 3600      # optional; default: 3600
      max-file-size-mb: 512     # optional; default: 512
      flush-interval-ms: 1000   # optional; default: 1000
      compression-level: 0      # optional; 0–9, default: 0 (off)
      queue-capacity: 8192      # optional; default: 8192
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | Required. Unique writer instance name. |
| `base-path` | string | — | Required. Directory for HDF5 output files. |
| `max-file-age-s` | int | `3600` | Rotate file after this many seconds. |
| `max-file-size-mb` | uint64 | `512` | Rotate file after this size (MiB). |
| `flush-interval-ms` | int | `1000` | How often the flush thread calls `H5File::flush`. |
| `compression-level` | int | `0` | DEFLATE level 0–9 (0 = no compression). |
| `queue-capacity` | size_t | `8192` | Max queued batches before `push()` blocks. push blocks indefinitely — never drops data. |

## hdf5-merge Writer

`HDF5WriterMerge` (YAML key `writer.hdf5-merge`) writes all root-sources into a **single shared HDF5 file**. Each source's datasets live under a dedicated HDF5 group.

### File layout

```
merged_<YYYYMMDDTHHMMSSz>.hdf5
├── source_a/
│   ├── timestamps         int64   [N]
│   ├── <col_name_0>       …       [N]
│   └── …
└── source_b/
    ├── timestamps         int64   [M]
    ├── <col_name_0>       …       [M]
    └── …
```

Groups are created on the first write for each source and reused for all subsequent writes.

For NTTable (tabular) sources the layout under the group mirrors the standard per-source tabular layout:

```
merged_<suffix>.hdf5
└── <source>/
    ├── secondsPastEpoch   int64   [N_rows]
    ├── nanoseconds        int64   [N_rows]
    └── <col_name>         typed   [N_rows]
```

### Locking model

`HDF5WriterMerge` owns a single `mergeFileMutex_` that guards all access to the shared file handle. Writer and flush threads never touch the merge file concurrently without holding this mutex. Simpler than the per-source `fileMutex` model used in pool mode and sufficient because all writes are serialised through the single writer thread.

### Rotation policy

Rotation is triggered when **any** source's accumulated write causes either threshold to be exceeded:

| Condition | Config key | Default |
|-----------|-----------|---------|
| File age ≥ threshold | `max-file-age-s` | 3600 s |
| Bytes written ≥ threshold | `max-file-size-mb` | 512 MiB |

On rotation:
1. Current file is flushed, closed, and renamed to its final visible path.
2. A new file is opened with a fresh UTC timestamp suffix.
3. All previously seen source groups are immediately recreated in the new file so that subsequent writes from any source can proceed without needing a schema re-discovery.

No data is lost during rotation: write operations call `checkMergeRotation()` before acquiring the file lock, so each write lands entirely in either the old file or the new file.

### Schema-conflict behaviour

If two sources attempt to write a dataset with the same name but different HDF5 types under the same group, the writer logs an error and throws `std::runtime_error`. This is enforced by `ensureDataset()`, which opens the existing dataset and compares its committed type against the requested type.

### Enable hdf5-merge writer

```yaml
writer:
  hdf5-merge:
    - name: hdf5_merged
      base-path: /data/hdf5
```

`supports_multi_root_source()` returns `true` on `HDF5WriterMerge`; the constructor guard is satisfied and the writer starts normally.

## Examples

### Single writer, single reader (default)

```yaml
writer:
  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5

reader:
  - epics-pvxs:
      - name: reader_main
        pvs:
          - name: LINAC:BPM:01:X
          - name: LINAC:BPM:01:Y
```

Output: one file per source in `/data/hdf5/`.

```
/data/hdf5/
├── LINAC_BPM_01_X_20260501T120000z.hdf5   # timestamps + LINAC:BPM:01:X datasets
└── LINAC_BPM_01_Y_20260501T120000z.hdf5   # timestamps + LINAC:BPM:01:Y datasets
```

---

### Two readers, PV filter routing, single merged file

Two readers each covering a different PV namespace. Only matching sources pass the
filter. All accepted sources land in one merged HDF5 file — one group per source.

```yaml
writer:
  hdf5-merge:
    - name: hdf5_merged
      base-path: /data/merged

reader:
  - epics-pvxs:
      - name: reader_linac
        pvs:
          - name: LINAC:BPM:01:X
          - name: LINAC:BPM:01:Y
          - name: LINAC:TEST:01:X   # test PV — excluded below

  - epics-pvxs:
      - name: reader_gun
        pvs:
          - name: GUN:SOLENOID:01:I
          - name: GUN:SOLENOID:01:V

routing:
  hdf5_merged:
    from:
      - reader_linac
      - reader_gun
    include:
      - "LINAC:BPM:*"      # accept production BPMs only
      - "GUN:SOLENOID:*"   # accept all gun solenoids
    exclude:
      - "LINAC:TEST:*"     # drop test PVs even if matched by include
```

**Filter logic** (applied per batch, keyed on `root_source`):

| `root_source` | include match | exclude match | Result |
|---|---|---|---|
| `LINAC:BPM:01:X` | ✅ `LINAC:BPM:*` | ✗ | **pass → written** |
| `LINAC:BPM:01:Y` | ✅ `LINAC:BPM:*` | ✗ | **pass → written** |
| `LINAC:TEST:01:X` | ✅ `LINAC:BPM:*` no / skip | ✅ `LINAC:TEST:*` | **drop** |
| `GUN:SOLENOID:01:I` | ✅ `GUN:SOLENOID:*` | ✗ | **pass → written** |
| `GUN:SOLENOID:01:V` | ✅ `GUN:SOLENOID:*` | ✗ | **pass → written** |

Output: one file in `/data/merged/`:

```
/data/merged/merged_20260501T120000z.hdf5
├── LINAC:BPM:01:X/
│   ├── timestamps       int64 [N]
│   └── LINAC:BPM:01:X   double [N]
├── LINAC:BPM:01:Y/
│   ├── timestamps       int64 [N]
│   └── LINAC:BPM:01:Y   double [N]
├── GUN:SOLENOID:01:I/
│   ├── timestamps       int64 [M]
│   └── GUN:SOLENOID:01:I  double [M]
└── GUN:SOLENOID:01:V/
    ├── timestamps       int64 [M]
    └── GUN:SOLENOID:01:V  double [M]
```

`LINAC:TEST:01:X` never appears — dropped by the `exclude` filter before reaching the writer.

> **Note:** glob patterns use `fnmatch(3)` — `*` matches `:` in EPICS PV names.
> Matching is case-sensitive. Patterns with no `include:` block accept all sources.

## Lifecycle

| Step | What happens |
|------|-------------|
| `HDF5WriterPerSource(config)` | Opens `HDF5FilePool`; one file per source. |
| `HDF5WriterMerge(config)` | Opens single shared merge file. |
| `start()` | Calls `doStart()` (opens pool / merge file), then spawns writer and flush threads. |
| `push(batch)` | Enqueues batch under `queueMutex_`; **blocks** if queue at capacity until space available. Returns `false` only when stopping (double Ctrl+C). |
| `stop()` | Sets `stopping_`; joins both threads; calls `doStop()` (closes files). |

## Thread-Safety Notes

- `HDF5FilePool` holds its mutex **only** during map lookup / rotation, not during HDF5 I/O. Concurrent I/O on different sources requires no contention.
- In non-merge mode, a per-`FileEntry` `fileMutex` serialises writer thread and flush thread access to the same `H5::H5File`. This is required because HDF5 (without the thread-safe build) is not thread-safe.
- In merge mode, a single `mergeFileMutex_` serialises all access to the shared file. The writer thread holds it during `appendFrameMerge()` and `flushTabularBufferMerge()`; the flush thread holds it during `mergeFile_->flush()`.
- `lastTsBatchSeq_` and `tabularBuffers_` are accessed exclusively from the writer thread — no mutex needed.

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/hdf5/HDF5WriterBase.h` | Abstract base: shared state, queue/thread logic, pure-virtual hooks. |
| `include/writer/hdf5/HDF5WriterPerSource.h` | Per-source subclass header; `REGISTER_WRITER("hdf5", …)`. |
| `include/writer/hdf5/HDF5WriterMerge.h` | Merge subclass header; `REGISTER_WRITER("hdf5-merge", …)`. |
| `include/writer/hdf5/HDF5WriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `include/writer/hdf5/HDF5FilePool.h` | Per-source file pool, rotation, flush. |
| `src/writer/hdf5/HDF5WriterBase.cpp` | `writerLoop`, `flushLoop`, tabular accumulation, `ensureDataset`, `flushTabularBuffer`. |
| `src/writer/hdf5/HDF5WriterPerSource.cpp` | `appendFrame`, pool lifecycle, `writeFrameImpl`. |
| `src/writer/hdf5/HDF5WriterMerge.cpp` | Merge-file helpers, `appendFrameMerge`, rotation. |
| `src/writer/hdf5/HDF5WriterDetail.h` | Internal shared helpers: `append1D/2D`, `writeColumnsImpl`, `mapNativeType`, `fillValue`. |

## Build Requirement

HDF5 writer compiles only when `MLDP_PVXS_HDF5_ENABLED` is defined. Enable it via CMake option `-DMLDP_PVXS_ENABLE_HDF5=ON`. The factory registration (`REGISTER_WRITER`) is inside the same guard, so types `"hdf5"` and `"hdf5-merge"` are absent from the factory when the option is off.

---

## Metrics

When the HDF5 writer is constructed via the factory (i.e. a `shared_ptr<Metrics>` is passed),
it creates an `HDF5WriterMetrics` instance registered into the shared Prometheus registry.
All metrics appear automatically in the HTTP endpoint and JSONL export without any
additional wiring.

Constant labels on all families: `{controller=<controller_name>, writer=<instance_name>}`.

| Metric | Type | Extra label | Description |
|--------|------|-------------|-------------|
| `mldp_pvxs_driver_hdf5_batches_written_total` | Counter | — | EventBatches written (columnar or tabular) |
| `mldp_pvxs_driver_hdf5_rows_written_total` | Counter | `source` | Rows (samples) appended per PV source |
| `mldp_pvxs_driver_hdf5_bytes_written_total` | Counter | `source` | Bytes written per PV source (estimated) |
| `mldp_pvxs_driver_hdf5_queue_depth` | Gauge | — | Write-queue depth after each drain cycle |
| `mldp_pvxs_driver_hdf5_queue_drops_total` | Counter | — | Batches dropped on queue overflow |
| `mldp_pvxs_driver_hdf5_file_rotations_total` | Counter | `source` | File rotations (age or size threshold) |
| `mldp_pvxs_driver_hdf5_write_latency_ms` | Histogram | — | `appendFrame` / `flushTabularBuffer` latency (ms) |

Metrics are null-safe: if `Metrics` is not provided (unit-test constructor), no instrumentation
occurs and no registry access is attempted.

### Smoke-test

```bash
curl -s http://localhost:9464/metrics | grep mldp_pvxs_driver_hdf5
```

Expected families (when at least one batch has been written):
```
mldp_pvxs_driver_hdf5_batches_written_total{controller="...",writer="..."} N
mldp_pvxs_driver_hdf5_queue_depth{controller="...",writer="..."} 0
mldp_pvxs_driver_hdf5_write_latency_ms_bucket{...}
…
```

### Implementation

`HDF5WriterMetrics` inherits `WriterMetrics → ExtendedMetrics`.
See [metrics-extension-guide.md](../metrics/metrics-extension-guide.md) for the full pattern
used to create new per-component metric classes.

## Config Migration

If upgrading from a build that used `merge-root-sources: true`:

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

Per-source writer configuration is unchanged — `writer.hdf5` still works as before.
