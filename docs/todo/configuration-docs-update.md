# TODO: Configuration Documentation Update

## Goal

Document the complete YAML configuration schema for all components added or changed
since the original docs were written, including the `writer:` block, `MLDPQueryClient`
pool config, and HDF5 writer options.

---

## Tasks

### 1. Create `docs/configuration.md`
Top-level YAML schema reference covering every supported key.

Sections:

#### `name`
- Type: string — logical name of the driver instance

#### `pvs`
- List of PV entries (`name`, optional `option.type`)

#### `writer:` block
```yaml
writer:
  grpc:
    enabled: true          # default true
    # … mldp-pool / controller-thread-pool keys (same as root-level grpc config)
  hdf5:
    enabled: false         # default false; requires MLDP_PVXS_ENABLE_HDF5=ON build
    output_dir: /data/hdf5
    max_file_size_mb: 256
    max_files: 10
```
- Document `WriterConfig` fields and defaults
- Document `MLDPGrpcWriterConfig` fields (endpoint, pool sizes, timeouts)
- Document `HDF5WriterConfig` fields (output_dir, max_file_size_mb, max_files)
- Note: if `writer:` is absent the driver defaults to gRPC writer using root-level
  pool config (backward-compat path)

#### `reader:` / per-reader config
- Cross-reference `docs/readers.md` for per-reader YAML keys
- Note `ReaderFactory` type strings: `epics-pvxs`, `epics-base`, `epics-archiver`

#### Metrics / Prometheus
- Cross-reference `docs/metrics-export-guide.md`

### 2. Annotate `WriterConfig` header with YAML key comments
- File: `include/writer/WriterConfig.h`
- Add `/// YAML key: writer.grpc.enabled` style comments to each field

### 3. Annotate `HDF5WriterConfig` header
- File: `include/writer/hdf5/HDF5WriterConfig.h`
- Same doc-comment treatment

### 4. Annotate `MLDPGrpcWriterConfig` header
- File: `include/writer/grpc/MLDPGrpcWriterConfig.h`
- Document each YAML key and its default

### 5. Add example YAML files to `docs/examples/`
- `docs/examples/config-grpc-only.yaml` — minimal gRPC-only config
- `docs/examples/config-grpc-and-hdf5.yaml` — dual-writer config
- `docs/examples/config-epics-archiver.yaml` — archiver reader with gRPC writer

---

## Key files

| File | Role |
|---|---|
| `include/writer/WriterConfig.h` | Top-level writer config |
| `include/writer/grpc/MLDPGrpcWriterConfig.h` | gRPC writer config |
| `include/writer/hdf5/HDF5WriterConfig.h` | HDF5 writer config |
| `src/controller/MLDPPVXSControllerConfig.cpp` | YAML parsing logic |
| `docs/readers.md` | Existing reader config docs |
