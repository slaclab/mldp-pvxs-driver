# MLDP PVXS Driver

![logo](./logos/SLAC-lab-hires.png)

This project provides a generic driver architecture for ingesting real-time or historical samples into the MLDP ingestion API (see [MLDP](https://github.com/osprey-dcs/dp-service.git)). It separates source readers from batching/transport so multiple data-source implementations can publish normalized payloads to MLDP over gRPC.

[DOI Code - 10.11578/dc.20260305.3](https://doi.org/10.11578/dc.20260305.3)

## Configuration

The full config is a single YAML document passed via `--config`. Every block shown is required unless marked optional.

The top-level document is the **controller configuration** (`MLDPPVXSControllerConfig`). It has three top-level keys:

| Key | Required | Description |
|-----|----------|-------------|
| `name` | no | Controller instance name; used as Prometheus label `controller` (default: `"default"`) |
| `writer` | **yes (≥1)** | One or more writer instances (`mldp`, `hdf5`). Controller throws at startup if absent or empty. |
| `reader` | **yes (≥1)**  | One or more reader groups (`epics-pvxs`, `epics-base`, `epics-archiver`) |
| `metrics` | no | Prometheus exporter settings |
| `routing` | no | Explicit writer→reader routing table. Omit for all-to-all (every writer receives from every reader). |

```yaml
name: my_controller                         # optional; default: "default"; scopes Prometheus metrics label 'controller'

writer:                                     # required — at least one writer instance must be configured

  # ========== MLDP Ingestion Writer ==========
  mldp:
    - name: mldp_main                       # required; unique instance name
      thread-pool: 2                        # optional; default: 1; worker threads for gRPC ingestion
      stream-max-bytes: 2097152             # optional; default: 2097152; flush stream after this payload size
      stream-max-age-ms: 200               # optional; default: 200; flush stream after this many ms
      mldp-pool:                            # required; MLDP gRPC connection-pool settings
        provider-name: pvxs_provider        # required; provider name advertised to MLDP
        provider-description: "PVXS provider"  # optional; human-readable label
        ingestion-url: dp-ingestion:50051   # required; MLDP ingestion service address
        query-url: dp-query:50052           # optional; defaults to ingestion-url
        min-conn: 1                         # optional; default: 1; minimum open connections
        max-conn: 4                         # optional; default: 4; maximum connections in pool
        credentials: ssl                    # optional; "none", "ssl", or TLS map (see below)
        # credentials:                      # alternative: custom TLS PEM files
        #   pem-cert-chain: /etc/certs/client.crt
        #   pem-private-key: /etc/certs/client.key
        #   pem-root-certs: /etc/certs/ca.crt

  # ========== HDF5 Storage Writer (optional; requires -DMLDP_PVXS_HDF5_ENABLED=ON) ==========
  hdf5:
    - name: hdf5_local                      # required; unique instance name
      base-path: /data/hdf5                 # required; directory for HDF5 output files
      max-file-age-s: 3600                  # optional; default: 3600; rotate after N seconds
      max-file-size-mb: 512                 # optional; default: 512; rotate after N MiB
      flush-interval-ms: 1000              # optional; default: 1000; flush thread period in ms
      compression-level: 0                  # optional; default: 0; DEFLATE level 0–9

reader:                                     # optional; list of reader groups to start

  # ========== EPICS PVAccess Reader (real-time monitoring via PVXS) ==========
  - epics-pvxs:
      - name: pvxs_reader_a                 # required; unique instance name
        thread-pool: 1                      # optional; default: 2; event processing workers
        column-batch-size: 50               # optional; default: 50; max NTTable columns per batch push
        pvs:
          - name: "PV:NAME:1"
          - name: "PV:NAME:2"
            option: "VALUE"                 # optional; scalar channel option
          - name: "TABLE:PV"
            option:                         # optional; structured option for SLAC BSAS NTTable
              type: "slac-bsas-table"       # enable row-by-row NTTable conversion
              tsSeconds: "secondsFieldName" # NTTable field carrying per-row epoch seconds
              tsNanos: "nanosFieldName"     # NTTable field carrying per-row nanoseconds

  # ========== EPICS Base Reader (real-time monitoring via Channel Access) ==========
  - epics-base:
      - name: base_reader_a                 # required; unique instance name
        thread-pool: 1                      # optional; default: 2
        column-batch-size: 50               # optional; default: 50
        monitor-poll-threads: 2             # optional; default: 2; CA monitor queue poll threads
        monitor-poll-interval-ms: 5         # optional; default: 5; ms between queue polls when idle
        pvs:
          - name: "PV:NAME:1"
          - name: "PV:NAME:2"
            option: "VALUE"                 # optional; scalar channel option

  # ========== EPICS Archiver Reader (historical fetch or continuous tail) ==========
  - epics-archiver:
      - name: archiver_historical           # one-shot historical fetch
        hostname: "archiver.example.com:11200"  # required; archiver host:port
        mode: "historical_once"             # optional; default: historical_once
        start-date: "2026-01-01T00:00:00Z" # required for historical_once; ISO 8601
        end-date: "2026-01-31T23:59:59Z"   # optional; ISO 8601 end of window
        connect-timeout-sec: 30             # optional; default: 30
        total-timeout-sec: 300              # optional; default: 300 (0 = infinite)
        batch-duration-sec: 1               # optional; default: 1; sample-time span per output batch
        tls-verify-peer: true               # optional; default: true
        tls-verify-host: true               # optional; default: true
        pvs:
          - name: "SYSTEM:SENSOR:TEMPERATURE:MAIN"
          - name: "SYSTEM:ACTUATOR:PRESSURE:OUTLET"

      - name: archiver_tail                 # continuous tail polling
        hostname: "archiver.example.com:11200"
        mode: "periodic_tail"
        poll-interval-sec: 5               # required for periodic_tail; seconds between polls
        lookback-sec: 5                    # optional; defaults to poll-interval-sec
        batch-duration-sec: 1              # optional; default: 1
        pvs:
          - name: "SYSTEM:SENSOR:TEMPERATURE:MAIN"

metrics:                                    # optional; Prometheus exporter settings
  endpoint: 0.0.0.0:9464                    # required when block is present; bind host:port
  scan-interval-seconds: 1                  # optional; default: 1; system metrics scan interval

routing:                                    # optional; omit for all-to-all (every writer receives from every reader)
  mldp_main:                                # writer instance name (must match a writer[].name)
    from:                                   # required; list of reader instance names to route to this writer
      - pvxs_reader_a
      - base_reader_a
  hdf5_local:
    from:
      - pvxs_reader_a                       # use "all" as a single entry to accept from every reader
```

### Supported Writer Types

| Writer Type | Description |
|-------------|-------------|
| `mldp` | gRPC ingestion writer — forwards batches to the MLDP ingestion service |
| `hdf5` | HDF5 storage writer — writes batches to local HDF5 files (build flag required) |

Multiple instances of the same type are supported (each entry in the sequence is independent).

### Supported Reader Types

| Reader Type      | Description                                                       |
|------------------|-------------------------------------------------------------------|
| `epics-pvxs`     | Event-driven PVAccess reader using PVXS (recommended)             |
| `epics-base`     | Polling-based Channel Access reader for legacy systems            |
| `epics-archiver` | Historical and periodic-tail reader from EPICS Archiver Appliance |

`mldp-pool.credentials` accepts:

- `none` — insecure, no TLS
- `ssl` — TLS with system trust store
- a map with optional `pem-cert-chain`, `pem-private-key`, `pem-root-certs` file paths (custom TLS)

For the complete configuration reference including all keys, types, and defaults, see [Configuration Reference](docs/configuration.md).
Example YAML files are in [`docs/examples/`](docs/examples/).

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

For periodic metrics dumps and manual triggers (Ctrl+P, Ctrl+D, SIGUSR1/SIGQUIT), see the [metrics export guide](docs/metrics-export-guide.md).

## Architecture

This project uses a pipeline-style architecture: PVXS clients feed PV updates into a bounded work queue; the core driver converts and enriches events and dispatches them to the MLDP ingestion service using a connection pool of gRPC channels; reader implementations consume and re-publish or transform events as needed.

### Documentation

- [Architecture Overview](docs/architecture.md) - System architecture, data flow, and design patterns
- [Configuration Reference](docs/configuration.md) - Complete YAML schema with all keys, types, and defaults
- [Reader Types](docs/readers.md) - Available reader implementations (EPICS Base, PVXS, Archiver)
- [Implementing Custom Readers](docs/readers-implementation.md) - Guide to creating new reader types
- [Writers Overview](docs/writers-implementation.md) - Writer pattern, factory registration, new writer guide
- [MLDP Writer](docs/writers/mldp-writer.md) - gRPC ingestion writer details and configuration
- [HDF5 Writer](docs/writers/hdf5-writer.md) - HDF5 storage writer details and configuration
- [MLDP Query Client](docs/query-client.md) - Standalone out-of-band query API
- [Logging Abstraction Guide](docs/logging.md) - How `util::log` works and custom logger implementation
- [HTTP Transport Provider](docs/http-provider.md) - Shared `util/http` abstraction for HTTP-based readers
- [Metrics Export Guide](docs/metrics-export-guide.md) - Prometheus metrics and manual dump triggers
- [Metrics Extension Guide](docs/metrics-extension-guide.md) - How to add per-component metric classes (`ExtendedMetrics` hierarchy)

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
  - Published by the release workflow in [`.github/workflows/build-docker-image.yml`](/Users/bisegni/dev/github/slaclab/mldp-pvxs-driver/.github/workflows/build-docker-image.yml).
- `ghcr.io/slaclab/mldp-pvxs-driver/dev:rockylinux-9.3-builder-r7.0.8.1-1.4.1`
  - Latest reusable dev image for the active matrix variant.
  - Refreshed by `main` CI in [`.github/workflows/build-and-test.yml`](/Users/bisegni/dev/github/slaclab/mldp-pvxs-driver/.github/workflows/build-and-test.yml) when content changes.
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
