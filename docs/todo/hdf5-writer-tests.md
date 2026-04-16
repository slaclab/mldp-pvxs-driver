# TODO: HDF5Writer Tests

## Goal

Add unit and integration tests for `HDF5Writer` and `HDF5FilePool`, covering the full
write path from `EventBatch` → HDF5 file on disk.  Also update existing tests that
used the old mock-bus stubs (`querySourcesInfo` / `querySourcesData`) to confirm they
still compile and run cleanly after those stubs were removed.

---

## Tasks

### 1. Unit tests for `HDF5WriterConfig`
- File: `test/writer/hdf5/hdf5_writer_config_test.cpp`
- Parse valid YAML with `output_dir`, `max_file_size_mb`, `max_files`
- Confirm defaults when optional fields are omitted
- Confirm parse throws on missing required fields

### 2. Unit tests for `HDF5FilePool`
- File: `test/writer/hdf5/hdf5_file_pool_test.cpp`
- Construct pool with a temp directory; verify files are created
- Push multiple `EventBatch` objects; verify datasets grow
- Roll-over when `max_file_size_mb` is exceeded
- Eviction of oldest file when `max_files` is exceeded
- Thread-safety smoke test (push from N threads simultaneously)

### 3. Integration tests for `HDF5Writer` via `WriterFactory`
- File: `test/writer/hdf5/hdf5_writer_test.cpp`
- `WriterFactory::create("hdf5", node, nullptr)` returns non-null writer
- `writer->start()` / `writer->stop()` lifecycle is clean
- Push a batch of mixed-type frames; open the resulting HDF5 file with the C++
  API and assert dataset names, shapes, and spot-check values
- Push a batch for a PV with array columns; verify array dataset written correctly

### 4. Guard all HDF5 test targets with `MLDP_PVXS_ENABLE_HDF5`
- In `CMakeLists.txt`: wrap new test executables in
  `if(MLDP_PVXS_ENABLE_HDF5)` … `endif()`
- Add them to the `ctest` suite with label `hdf5`

### 5. Confirm existing tests compile without query-bus stubs
- `test/reader/impl/epics/base/epics_base_reader_test.cpp` — already cleaned;
  run `ctest` and assert green
- `test/reader/impl/epics/pvxs/epics_pvxs_reader_test.cpp` — same
- `test/pool/mldp_grpc_pool_test.cpp` — uses `MLDPQueryClient` directly; run
  and assert green

---

## Key files

| File | Role |
|---|---|
| `include/writer/hdf5/HDF5Writer.h` | Writer interface + config constructor |
| `include/writer/hdf5/HDF5WriterConfig.h` | Config struct |
| `src/writer/hdf5/HDF5Writer.cpp` | Write path implementation |
| `src/writer/hdf5/HDF5FilePool.cpp` | File pool (rotation, threading) |
| `test/mock/MockDataBus.h` | Shared test mock (push-only) |
