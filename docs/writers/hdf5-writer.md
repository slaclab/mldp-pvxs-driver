# HDF5 Storage Writer

> **Related:** [Writers Overview](../writers-implementation.md) | [Architecture](../architecture.md)

## Overview

`HDF5Writer` stores incoming event batches as HDF5 datasets on disk. It registers as type `"hdf5"` in the writer factory. Available only when the build option `MLDP_PVXS_HDF5_ENABLED` is set.

## Internal Architecture

```
push() → bounded MPSC deque
               ↓
         writerThread
               ↓
        appendFrame()  →  HDF5FilePool.acquire(source)
                                    ↓
                            H5::H5File (per source)
                                    ↓
                       flushThread (periodic flush)
```

- **Writer thread**: single thread drains the queue and calls `appendFrame()`.
- **Flush thread**: calls `HDF5FilePool::flushAll()` every `flush-interval-ms`.
- **File pool**: `HDF5FilePool` keeps one open `H5::H5File` per `root_source`; rotates on age or size threshold.
- **Back-pressure**: queue capped at `kQueueCapacity` (8192). `push()` returns `false` when full.

## HDF5 File Layout

```
/ (root)
├── timestamps        int64   ns-since-epoch   shape=(N,) unlimited+chunked
├── <col_name_0>      …       type from DataFrame column
└── …
```

One file per `root_source`. File name format:

```
<base-path>/<safe_source>_<YYYYMMDDTHHMMSSz>.h5
```

Characters outside `[A-Za-z0-9._-]` in the source name are replaced by `_`.

## File Rotation

`HDF5FilePool` rotates a file when **either** threshold is exceeded:

| Condition | Config key | Default |
|-----------|-----------|---------|
| File age ≥ threshold | `max-file-age-s` | 3600 s (1 h) |
| Bytes written ≥ threshold | `max-file-size-mb` | 512 MiB |

Rotation: close current file → open new file with fresh timestamp suffix.

## Configuration

Under `writer.hdf5[i]`:

```yaml
writer:
  hdf5:
    - name: hdf5_local          # required — unique instance name
      base-path: /data/hdf5     # required — output directory
      max-file-age-s: 3600      # optional; default: 3600
      max-file-size-mb: 512     # optional; default: 512
      flush-interval-ms: 1000   # optional; default: 1000
      compression-level: 0      # optional; 0–9, default: 0 (off)
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | — | Required. Unique writer instance name. |
| `base-path` | string | — | Required. Directory for HDF5 output files. |
| `max-file-age-s` | int | `3600` | Rotate file after this many seconds. |
| `max-file-size-mb` | uint64 | `512` | Rotate file after this size (MiB). |
| `flush-interval-ms` | int | `1000` | How often the flush thread calls `H5File::flush`. |
| `compression-level` | int | `0` | DEFLATE level 0–9 (0 = no compression). |

## Lifecycle

| Step | What happens |
|------|-------------|
| `start()` | Initialises `HDF5FilePool`; spawns writer and flush threads. |
| `push(batch)` | Enqueues batch; returns `false` if queue is at capacity. |
| `stop()` | Sets stop flag; joins threads; calls `HDF5FilePool::closeAll()`. |

## Thread-Safety Notes

- `HDF5FilePool` holds its mutex **only** during map lookup / rotation, not during HDF5 I/O.
- Concurrent I/O on **different** sources requires no contention.
- `HDF5Writer` itself uses a single writer thread, so `appendFrame()` never races.

## Key Files

| File | Purpose |
|------|---------|
| `include/writer/hdf5/HDF5Writer.h` | Class definition (guard: `MLDP_PVXS_HDF5_ENABLED`). |
| `include/writer/hdf5/HDF5WriterConfig.h` | Config struct, YAML keys, `parse()`. |
| `include/writer/hdf5/HDF5FilePool.h` | Per-source file pool, rotation, flush. |
| `src/writer/hdf5/HDF5Writer.cpp` | `appendFrame()`, `ensureDataset()`, thread loops. |

## Build Requirement

HDF5 writer compiles only when `MLDP_PVXS_HDF5_ENABLED` is defined. Pass `-DMLDP_PVXS_HDF5_ENABLED=ON` to CMake. The factory registration (`REGISTER_WRITER`) is inside the same guard, so the type `"hdf5"` is absent from the factory when the option is off.
