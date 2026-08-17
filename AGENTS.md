# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Environment Strategy

This project requires EPICS Base, PVXS, gRPC, Protobuf, and supporting services (MongoDB, dp-ingestion, dp-query). The devcontainer provides all of these. Follow this priority order:

### 1. Check if devcontainer is already running

The devcontainer is managed by two compose files: `docker-compose.yml` + `.devcontainer/docker-compose.devcontainer.yml`. The service name is `devcontainer`.

```bash
# Check if the devcontainer service is running
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml ps --status running devcontainer
```

If running, execute build/test commands inside it:

```bash
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml \
  exec devcontainer bash -lc "cmake --build /workspace/build --parallel && ctest --test-dir /workspace/build --output-on-failure"
```

### 2. If not running — try to start it

Using the VS Code `devcontainer` CLI (if available):

```bash
# Check if devcontainer CLI exists
which devcontainer

# Start the devcontainer (builds image if needed)
devcontainer up --workspace-folder .
```

Or via docker-compose directly:

```bash
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml up -d
```

Then exec into it as shown above.

### 3. Fallback — local environment

If docker/devcontainer are unavailable, build locally. Requires EPICS Base, PVXS, gRPC, Protobuf, HDF5 installed on the host.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMLDP_PVXS_DRIVER_TESTS=ON \
  -DEPICS_BASE=/opt/local -DPVXS_BASE=/opt/local \
  -DEPICS_HOST_ARCH=linux-x86_64

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 4. If all fail — manual intervention needed

If none of the above work (no docker, no local EPICS/PVXS), inform the user and suggest they either install the devcontainer CLI (`npm install -g @devcontainers/cli`) or set up the local dependencies per README.md.

## Build Commands (once inside the environment)

```bash
# Configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMLDP_PVXS_DRIVER_TESTS=ON \
  -DEPICS_BASE=/opt/local -DPVXS_BASE=/opt/local \
  -DEPICS_HOST_ARCH=linux-x86_64

# Build everything
cmake --build build --parallel

# Build only the test binary
cmake --build build --target mldp_pvxs_driver_test

# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test by name pattern
ctest --test-dir build -R "EpicsPVXSReaderTest" --output-on-failure

# Run tests with a label filter (e.g. HDF5-only)
ctest --test-dir build -L hdf5 --output-on-failure

# Build + run via docker-compose (full CI environment with MongoDB, dp-ingestion, dp-query)
docker compose -f docker-compose-test.yml up --build ci
```

## Test Structure

- Main test executable: `mldp_pvxs_driver_test` (Google Test)
- Separate test binaries: `mldp_hdf5_writer_test`, `mldp_hdf5_bsas_gen1_reader_test`, `mldp_controller_hdf5_integration_test`, `queryable_factory_test`, `mldp_pvxs_driver_wizard_test`, `mldp_pvxs_driver_edit_test`
- Tests live in `test/` mirroring `src/` structure
- Mock IOC server (`test/mock/sioc.cpp`) provides live EPICS PVs for integration tests
- Test data directory defined via `MLDP_TEST_DATA_DIR` compile definition pointing to `data/`

## Code Style

- C++20, clang-format enforced (`.clang-format` in repo root)
- Indent: 4 spaces, Allman brace style (braces on new line)
- PointerAlignment: Left (`int* p`)
- ColumnLimit: 0 (no line length limit)
- Namespace: `mldp_pvxs_driver::` with inner indentation
- In `.cpp` files prefer `using namespace mldp_pvxs_driver::...;` instead of `namespace mldp_pvxs_driver::... { ... }`; only anonymous namespaces (`namespace { ... }`) should use block-style namespace declarations.
- License header required on all `.h`/`.cpp` files — run `./scripts/add-licenses-include-h.sh` before committing
- Every handwritten project class must have its own paired `.h` and `.cpp` files with the same class-oriented basename. Do not define standalone concrete classes inside another `.cpp`; extract them into the corresponding header/source tuple and list the `.cpp` in CMake. This rule excludes generated lexer/parser sources and their generated support types; data-only structs may remain grouped with their owning interface.
- Place each class tuple in the directory that matches its responsibility and existing subsystem layout: for example, query execution streams/states under `query/executor/`, MLDP-specific query implementations under `query/impl/mldp/`, parser code under `query/parser/`, and generic query contracts/utilities directly under `query/`. Do not choose a location based only on filename convenience. If no existing directory owns the class's responsibility, create a focused new subdirectory, move the related class series there, and register all new `.cpp` files in CMake.

## Architecture

### Pipeline: Reader → Bus → Controller → Writer

The driver is a data pipeline: **Readers** collect samples, push normalized `EventBatch` payloads onto a shared **IDataBus**, the **Controller** (`MLDPPVXSController`) partitions/routes batches via a **RouteTable**, and **Writers** deliver to sinks.

### Factory Pattern (Static Self-Registration)

Readers and Writers self-register at static-init time via `REGISTER_READER("type", Class)` / `REGISTER_WRITER("type", Class)` macros. The CRTP `Factory<Derived, Product, Args...>` template in `include/util/factory/Factory.h` holds a Meyers-singleton registry.

**Critical linker requirement:** Because self-registration lives in static initializers, the library must be linked with `--whole-archive` (Linux) or `-force_load` (macOS) so the linker doesn't discard unused translation units. This is already handled in CMakeLists.txt for all test/main executables.

### Adding a New Reader

1. Create `src/reader/impl/<name>/` with `Reader.cpp` and `ReaderConfig.cpp`
2. Inherit from `mldp_pvxs_driver::reader::Reader`
3. Add `REGISTER_READER("type-string", ClassName)` in the class body or .cpp file
4. Add source files to `CMakeLists.txt` under `lib${PROJECT_NAME}`
5. See `docs/readers/readers-implementation.md` for full guide

### Adding a New Writer

1. Create `src/writer/<name>/` with `Writer.cpp` and `WriterConfig.cpp`
2. Implement `mldp_pvxs_driver::writer::IWriter` interface (start/push/stop)
3. Add `REGISTER_WRITER("type-string", ClassName)` in the class body
4. Add source files to `CMakeLists.txt`
5. See `docs/writers/writers-implementation.md` for full guide

### Key Interfaces

- `include/reader/IReader.h` — Reader base class (owns bus + metrics refs)
- `include/writer/IWriter.h` — Writer interface (start/push/stop lifecycle)
- `include/util/bus/IDataBus.h` — Event bus carrying `EventBatch` between readers and writers
- `include/controller/MLDPPVXSController.h` — Orchestrates readers, writers, routing
- `include/util/factory/Factory.h` — Generic CRTP factory template

### Configuration

YAML config parsed by `src/config/Config.cpp` using vendored rapidyaml (`ext/rapidyaml`). Top-level keys: `name`, `writer` (array), `reader` (array), `routing`, `metrics`.

### External Dependencies (FetchContent)

spdlog, argparse, prometheus-cpp, nlohmann/json, FTXUI (wizard), GoogleTest (tests), HDF5 (optional). EPICS Base and PVXS are found via system paths or `EPICS_BASE`/`PVXS_BASE` env/cmake vars.

### Optional Build Features

| CMake Option | Default | Purpose |
|---|---|---|
| `MLDP_PVXS_ENABLE_HDF5` | ON | HDF5 writer + reader support |
| `BUILD_ECHO_PROCESSOR` | ON | Debug pass-through processor |
| `BUILD_PYTHON_PROCESSOR` | ON | Python script processor (needs CPython) |
| `MLDP_WIZARD` | ON | Interactive TUI config wizard (FTXUI) |
| `MLDP_PVXS_DRIVER_COVERAGE` | OFF | gcov/llvm-cov instrumentation |

## Development Environment

Recommended: VS Code devcontainer (`.devcontainer/`). Brings up EPICS, PVXS, MongoDB, dp-ingestion, dp-query services automatically. Uses Rocky Linux 9.3 with Clang/LLVM toolchain.
