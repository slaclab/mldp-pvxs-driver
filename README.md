# MLDP PVXS Driver

![logo](./logos/SLAC-lab-hires.png)

This project provides a generic driver architecture for ingesting real-time or historical samples into the MLDP ingestion API (see [MLDP](https://github.com/osprey-dcs/dp-service.git)). It separates source readers from batching/transport so multiple data-source implementations can publish normalized payloads to MLDP over gRPC.

[DOI Code - 10.11578/dc.20260305.3](https://doi.org/10.11578/dc.20260305.3)

## Configuration Summary

The driver consumes one YAML document via `--config` with five top-level sections:

| Key | Required | Purpose |
|-----|----------|---------|
| `name` | no | Controller instance label (metrics scope) |
| `writer` | **yes (≥1)** | Output sinks (`mldp`, `hdf5`, `hdf5-merge`) |
| `reader` | **yes (≥1)** | Input sources (`epics-pvxs`, `epics-base`, `epics-archiver`) |
| `routing` | no | Reader-to-writer routing and optional source filters |
| `metrics` | no | Prometheus exporter settings |

Use these docs for full details:

- [Configuration Reference](docs/guides/configuration.md) — complete schema, defaults, validation rules
- [User Guide](docs/guides/user-guide.md) — practical end-to-end YAML examples
- [Reader Types](docs/readers/readers.md) — reader capabilities + build/dependency matrix
- [Writers Overview](docs/writers/writers-implementation.md) — writer model and extension points

## Architecture Summary

The architecture is reader → bus → controller → writer:

1. Readers collect live or historical EPICS data.
2. Readers push normalized batches onto `IDataBus`.
3. Controller partitions and routes batches.
4. Writers deliver to MLDP gRPC or HDF5 storage.

For detailed diagrams and threading/data-flow internals see [Architecture Overview](docs/reference/architecture.md).

## Command-line interface

The driver is configured via a YAML file (see above) and is started from the command line.

### Usage

```bash
mldp_pvxs_driver [--help] [--version] [--config PATH] [--log-level LEVEL] [--metrics-output FILE] [--metrics-interval SECONDS] [--print-config-startup] [--dry-run]
```

### Options

- `-h, --help`
  - Show the built-in help and exit.
- `-v, --version`
  - Print the version and exit.
- `-c, --config PATH`
  - Path to the YAML configuration file.
  - Default: `config.yaml`
- `-l, --log-level LEVEL`
  - Logging verbosity.
  - Accepted values: `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`
  - Default: `info`
  - Notes: value is case-insensitive; `warning` is accepted as `warn`, `err` as `error`, and `fatal` as `critical`.
- `-m, --metrics-output FILE`
  - Path to output file for periodic metrics dumps (JSON Lines format).
  - Default: `metrics.jsonl`
- `--metrics-interval SECONDS`
  - Interval in seconds for periodic metrics dumps.
  - Default: `5`
- `--print-config-startup` (alias: `--print-config`)
  - Print a compact, user-friendly summary of the effective startup configuration.
  - Default: disabled
- `--dry-run`
  - Load and validate config, then exit without starting driver/readers.
  - Default: disabled

### Examples

```bash
# Run with an explicit config file
./mldp_pvxs_driver --config ./config.yaml

# Enable debug logging
./mldp_pvxs_driver --config ./config.yaml --log-level debug

# Print effective config at startup (compact format)
./mldp_pvxs_driver --config ./config.yaml --print-config-startup

# Validate config and exit without starting runtime components
./mldp_pvxs_driver --config ./config.yaml --dry-run

# Validate + print effective config summary, then exit
./mldp_pvxs_driver --config ./config.yaml --print-config --dry-run

# Show help/version
./mldp_pvxs_driver --help
./mldp_pvxs_driver --version
```

For periodic metrics dumps and manual triggers (Ctrl+P, Ctrl+D, SIGUSR1/SIGQUIT), see the [metrics export guide](docs/metrics/metrics-export-guide.md).

## Architecture

> 🚀 **New to the driver?** Start with the **[User Guide](docs/guides/user-guide.md)** — annotated YAML examples covering every reader, writer, routing, and source-filter scenario. No C++ knowledge required.

Readers collect data from configurable sources, push normalized batches onto a shared bus, and the controller routes them to one or more writers — each writer delivering to a different storage or transport backend.

### Documentation

- [**User Guide**](docs/guides/user-guide.md) - Start here: annotated examples for operators and physicists (no C++ required)
- [Architecture Overview](docs/reference/architecture.md) - System architecture, data flow, and design patterns
- [Configuration Reference](docs/guides/configuration.md) - Complete YAML schema with all keys, types, and defaults
- [Reader Types](docs/readers/readers.md) - Available reader implementations (EPICS Base, PVXS, Archiver)
- [Implementing Custom Readers](docs/readers/readers-implementation.md) - Guide to creating new reader types
- [Writers Overview](docs/writers/writers-implementation.md) - Writer pattern, factory registration, new writer guide
- [MLDP Writer](docs/writers/mldp-writer.md) - gRPC ingestion writer details and configuration
- [HDF5 Writer](docs/writers/hdf5-writer.md) - HDF5 storage writer details and configuration
- [MLDP Query Client](docs/dev/query-client.md) - Standalone out-of-band query API
- [Logging Abstraction Guide](docs/dev/logging.md) - How `util::log` works and custom logger implementation
- [HTTP Transport Provider](docs/dev/http-provider.md) - Shared `util/http` abstraction for HTTP-based readers
- [Metrics Export Guide](docs/metrics/metrics-export-guide.md) - Prometheus metrics and manual dump triggers
- [Metrics Extension Guide](docs/metrics/metrics-extension-guide.md) - How to add per-component metric classes (`ExtendedMetrics` hierarchy)

For developer information and contribution guidelines see [CONTRIBUTING.md](CONTRIBUTING.md).

## External Software

- [EPICS Base](https://github.com/epics-base/epics-base) (default: R7.0.8.1) provides the core EPICS runtime and `libCom`.
- [PVXS](https://github.com/epics-base/pvxs) (default: 1.4.1) provides the PVAccess client used to subscribe to EPICS PVs.
- gRPC (system package; version per toolchain) provides the RPC transport to the MLDP ingestion service.
- Protocol Buffers (system package; version per toolchain) generates and serializes MLDP protobuf payloads.
- [dp-grpc proto definitions](https://github.com/osprey-dcs/dp-grpc) supply the MLDP ingestion API `.proto` files used at build time.
- OpenSSL (system library; version per OS/toolchain) provides TLS for gRPC credentials.
- [spdlog](https://github.com/gabime/spdlog) v1.16.0 provides structured logging.
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) v1.3.0 provides the metrics registry and HTTP exporter.
- [argparse](https://github.com/p-ranav/argparse) v3.2 provides CLI argument parsing.
- [rapidyaml](https://github.com/biojppm/rapidyaml) 0.10.0 (vendored in `ext/rapidyaml`) parses the YAML configuration.
- [BS::thread_pool](https://github.com/bshoshany/thread-pool) 5.0.0 (vendored in `ext/BS_thread_pool`) provides the controller worker thread pool.
- libevent (system library; required when statically linking PVXS) supplies PVXS' event loop dependencies in static builds.
- [CMake](https://cmake.org) 3.15+ configures and builds the project.

## Releases

Tagged releases (`vX.Y.Z`) publish:

- A container image (recommended way to run).
- A standalone executable artifact (currently named `mldp_pvxs_driver-rockylinux-9.3-epics-R7.0.8.1`).
- An AppImage for easier distribution (currently named `mldp_pvxs_driver-rockylinux-9.3-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage`).

### Builder image + build cache (for developers)

The current CI/CD publishes and refreshes four related builder/dev refs in GHCR:

- `ghcr.io/slaclab/mldp-pvxs-driver/build:epics-7.0.8.1-pvxs-1.4.1`
  - Shared builder image tagged only by EPICS + PVXS versions.
  - Published by the release workflow in [`.github/workflows/build-docker-image.yml`](.github/workflows/build-docker-image.yml).
- `ghcr.io/slaclab/mldp-pvxs-driver/dev:rockylinux-9.3-builder-r7.0.8.1-1.4.1`
  - Latest reusable dev image for the active matrix variant.
  - Refreshed by `main` CI in [`.github/workflows/build-and-test.yml`](.github/workflows/build-and-test.yml) when content changes.
- `ghcr.io/slaclab/mldp-pvxs-driver/rockylinux-9.3-builder-r7.0.8.1-1.4.1:buildcache`
  - Variant-specific builder image tag used to keep the registry cache warm.
- `ghcr.io/slaclab/mldp-pvxs-driver/rockylinux-9.3-builder-r7.0.8.1-1.4.1:buildkitcache`
  - Registry-backed BuildKit cache export used by both CI workflows.

For local `docker buildx` builds, use both the shared builder image and the variant-specific BuildKit cache as cache sources, which matches the current CI setup:

```bash
docker login ghcr.io

docker buildx build \
  -f .devcontainer/Dockerfile \
  --build-arg BASE_OS_IMAGE=rockylinux/rockylinux:9.3 \
  --build-arg EPICS_VERSION=R7.0.8.1 \
  --build-arg PVXS_VERSION=1.4.1 \
  --cache-from type=registry,ref=ghcr.io/slaclab/mldp-pvxs-driver/build:epics-7.0.8.1-pvxs-1.4.1 \
  --cache-from type=registry,ref=ghcr.io/slaclab/mldp-pvxs-driver/rockylinux-9.3-builder-r7.0.8.1-1.4.1:buildkitcache \
  -t mldp-pvxs-driver-dev:latest \
  --load \
  .
```

Notes:

- The shared `build:epics-...-pvxs-...` tag is the stable cross-branch builder reference.
- The `dev:<variant>` tag tracks the latest dev image for that OS/EPICS/PVXS matrix entry.
- The `:buildkitcache` ref is not a runnable image; it is a registry cache export for BuildKit.
- The `:<variant>:buildcache` tag is a pushed builder image that helps keep the cache hot on `main`.

#### EPICS/PVXS locations

In the builder/dev container image:

- EPICS Base source is cloned into `/opt/epics` and installed into `/opt/local`.
- PVXS source is cloned into `/opt/pvxs` and installed into `/opt/local`.
- The EPICS host architecture is recorded in `/etc/epics_host_arch` (e.g. `linux-x86_64`).

In the runtime/release container image:

- `/opt/local` is copied from the builder stage and contains EPICS Base + PVXS headers and libraries.
- `EPICS_BASE=/opt/local` and `PVXS_BASE=/opt/local` are set in the runtime image.

#### Where EPICS/PVXS versions are set

- Default build args are defined in [.devcontainer/Dockerfile](.devcontainer/Dockerfile) (`EPICS_VERSION`, `PVXS_VERSION`).
- CI/release versions are set by the workflow matrix in [.github/workflows/build-and-test.yml](.github/workflows/build-and-test.yml) and [.github/workflows/build-docker-image.yml](.github/workflows/build-docker-image.yml).


### Standalone executable runtime dependencies

The standalone executable artifact is **dynamically linked** (not a fully static binary). This means it requires
shared libraries to be present on the target host at runtime.

At a minimum, the binary depends on:

- gRPC + Protobuf runtime libraries (and their transitive deps like Abseil, c-ares, re2)
- OpenSSL (`libssl`, `libcrypto`)
- EPICS Base runtime (`libCom`)
- PVXS runtime (`libpvxs`)

In the release build environment, EPICS Base + PVXS are installed under `/opt/local`, and the binary is built with a
runtime search path pointing there. If you download and run the standalone artifact on a different host, you must
either:

- install compatible EPICS Base + PVXS and ensure they are discoverable by the dynamic loader (e.g., via
  `LD_LIBRARY_PATH` or a matching install prefix), and install the required gRPC/Protobuf/OpenSSL runtime packages, or
- run via the published Docker image, which includes the correct runtime environment.

## Legal

## Copyright Notice

COPYRIGHT © SLAC National Accelerator Laboratory. All rights reserved. This work is supported [in part] by the U.S. Department of Energy, Office of Basic Energy Sciences under contract DE-AC02-76SF00515.

## Usage Restrictions

Neither the name of the Leland Stanford Junior University, SLAC National Accelerator Laboratory, U.S. Department of Energy nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
