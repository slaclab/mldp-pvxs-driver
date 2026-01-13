# Developer Guide

This document explains the project architecture, how to build and test locally, how the reader abstraction works, and common troubleshooting steps for developers working on the `mldp-pvxs-driver` repository.

**Key source locations**
- Core library: `libmldp_pvxs_driver` (CMake target in `CMakeLists.txt`)
- Public headers: `include/`
- Reader implementations: `include/reader/impl/` and `src/reader/impl/`
- Factory: `include/reader/ReaderFactory.h` / `src/reader/ReaderFactory.cpp`

## Overview

`mldp-pvxs-driver` ingests PV updates (via PVXS) and forwards them to an MLDP ingestion service. The code is split into a small core ingestion driver and a reader abstraction that allows multiple input/output adapters to be plugged in. The reader abstraction isolates protocol-specific logic (EPICS, file replay, simulation) behind a tiny interface so the ingestion pipeline can remain unchanged.

## Architecture and reader details

Detailed architecture and reader abstraction documentation has moved to [docs/architecture.md](docs/architecture.md). Please consult that file for diagrams, the connection-pool design, reader contract, registration macro, and examples (including `EpicsReader`).

## Build and run (local)

Prerequisites
- `cmake` (3.15+), a C++20-capable compiler, `protoc`/gRPC toolchain, and the `pvxs`/EPICS headers and libraries.

Standard build
```bash
cmake -S . -B build \
  -DPROTO_PATH=/path/to/protos \
  -DPVXS_BASE=/path/to/pvxs \
  -DEPICS_BASE=/path/to/epics \
  -DEPICS_HOST_ARCH=linux-x86_64
cmake --build build --parallel
```

Run the driver (example)
```bash
# from repository root
./build/bin/mldp_pvxs_driver --config path/to/config.yaml
```

Notes
- If using the bundled devcontainer, `EPICS_BASE` and `PVXS_BASE` may already be available under `/opt/local`. Adjust `-DEPICS_HOST_ARCH` to match the container's installed EPICS arch.

## Testing

- Enable tests by configuring with `-DMLDP_PVXS_DRIVER_TESTS=ON`.
```bash
cmake -S . -B build -DMLDP_PVXS_DRIVER_TESTS=ON -DPROTO_PATH=/path/to/protos -DPVXS_BASE=/path/to/pvxs
cmake --build build --target mldp_pvxs_driver_test
ctest --test-dir build --output-on-failure
```
- Unit tests use GoogleTest (pulled via CMake FetchContent when tests are enabled).

## Debugging and IDEs

- clangd: The repository contains a `.clangd` file that references the `build` compilation database and adds common include directories. If you see include-not-found errors in the editor, regenerate the build (`cmake -S . -B build`) to refresh `compile_commands.json`. If the header is only used by an uncompiled TU, add include hints to `.clangd`.
- LLDB (inside devcontainer): Set the LLDB DAP executable to `/usr/bin/lldb-dap-18` (see `.vscode` settings or `devcontainer.json`).

## Troubleshooting (common issues)

- "'bus/IEventBusPush.h' file not found" in the editor: This is usually an editor/clangd include-path issue. Confirm `build/compile_commands.json` contains the TU that includes the header or add the project `include/` path to `.clangd` via the `Add:` flags.
- Linker errors for `pvxs` or `epics`: Ensure `PVXS_BASE` and `EPICS_BASE` match the headers/libraries used at build time. Use `-DPVXS_BASE` and `-DEPICS_BASE` to point to the correct installs.

## Contribution & Workflow

- Branching: branch from `main` for new work and open a PR back to `main` with a clear description and testing steps.
- Commits: keep changes focused and atomic. Separate config changes from logic changes when feasible.
- Code review: include rationale for design choices, and add unit tests for new logic where possible.

**Adding License Headers**

- **Purpose:** Add or update license headers in project headers under include/ and other files that require the project's license header.
- **Script:** Run the provided script to apply license headers across the repository:

```bash
./scripts/add-licenses-include-h.sh
```

- **VS Code Task:** Alternatively run the workspace task `nwa: add license headers (include/**/*.h)` via the editor `Run Task...` command, which executes the same script.
- **When to run:** Run the script when you add new header/source files or before opening a PR to ensure all files have the correct license header.
- **Behavior:** The script is idempotent and safe to run multiple times. It invokes the `nwa` tool with the repository's `.nwa-config.yaml` to insert headers consistently.
- **Tool used:** The script invokes the [`nwa`](https://github.com/B1NARY-GR0UP/nwa) tool. The repository script calls `/usr/local/bin/nwa config -c add .nwa-config.yaml` (see `scripts/add-licenses-include-h.sh`), so ensure `nwa` is installed or run the script inside the devcontainer where `nwa` is available.

## Where to look in code

- Factory: `include/reader/ReaderFactory.h`, `src/reader/ReaderFactory.cpp`
- Bus interface: `include/bus/IEventBusPush.h`
- Core driver: `include/mldp_pvxs_driver.h`, `src/mldp_pvxs_driver.cpp`
- Reader implementations: `include/reader/impl/` and `src/reader/impl/`

## Appendix: Quick commands

```bash
# configure and build
cmake -S . -B build -DPROTO_PATH=/path/to/protos -DPVXS_BASE=/path/to/pvxs
cmake --build build

# run tests
cmake --build build --target test
ctest --test-dir build --output-on-failure

# format changed C++ files
clang-format -i $(git diff --name-only --diff-filter=ACMRTUXB HEAD | grep -E '\.cpp$|\.h$|\.hpp$' || true)
```
