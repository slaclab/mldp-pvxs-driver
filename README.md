# MLDP PVXS Driver

![logo](./logos/SLAC-lab-hires.png)

This project provides a generic driver architecture for ingesting real-time or historical samples into the MLDP ingestion API (see [MLDP](https://github.com/osprey-dcs/dp-service.git)). It separates source readers from batching/transport so multiple data-source implementations can publish normalized payloads to MLDP over gRPC.

[DOI Code - 10.11578/dc.20260305.3](https://doi.org/10.11578/dc.20260305.3)

## Configuration

When running the CLI, the full config is a single YAML document. Every block shown is required
unless marked optional.

```yaml
controller-thread-pool: 2                   # worker threads for controller-side batching and send work
controller-stream-max-bytes: 2097152        # optional; flush a gRPC stream after this payload size
controller-stream-max-age-ms: 200           # optional; flush a gRPC stream after this many milliseconds

mldp-pool:                                  # MLDP gRPC provider and connection-pool settings
  provider-name: pvxs_provider              # provider name advertised to MLDP
  provider-description: "PVXS aggregate provider"   # optional; human-readable provider label
  ingestion-url: dp-ingestion:50051         # MLDP ingestion service address
  query-url: dp-query:50052                 # MLDP query service address
  min-conn: 1                               # minimum number of gRPC connections to keep ready
  max-conn: 4                               # maximum number of gRPC connections allowed in the pool
  credentials:                              # optional; TLS client credential files
    pem-cert-chain: /etc/certs/client.crt   # client certificate chain PEM file
    pem-private-key: /etc/certs/client.key  # client private key PEM file
    pem-root-certs: /etc/certs/ca.crt       # CA bundle PEM file used to verify the server

reader:                                     # optional; list of reader instances to start
  # ========== EPICS PVAccess Reader (Real-time Monitoring via PVXS) ==========
  - epics-pvxs:                             # PVAccess reader type using PVXS subscriptions
      - name: pvxs_reader_a                 # unique reader instance name for logs and metrics
        thread-pool: 1                      # optional; default: 1; conversion worker pool for processing PV updates
        column-batch-size: 50               # optional; default: 50; max NTTable columns processed per batch
        monitor-poll-threads: 2             # optional; default: 2; threads draining monitor events from the queue
        monitor-poll-interval-ms: 5         # optional; default: 5; sleep between monitor queue polls
        pvs:                                # PV list monitored by this reader
          - name: "PV:NAME:1"               # PV name to subscribe to
          - name: "PV:NAME:2"               # PV name to subscribe to
            option: "VALUE"                 # optional; scalar channel option passed to the monitor
          - name: "TABLE:PV"                # PV expected to publish NT Table data
            option:                         # optional; structured monitor option for NT Table handling
              type: "slac-bsas-table"       # enable SLAC BSAS row-by-row NT Table conversion
              tsSeconds: "secondsFieldName" # NT Table field containing per-row epoch seconds
              tsNanos: "nanosFieldName"     # NT Table field containing per-row nanoseconds

  # ========== EPICS Base Reader (Real-time Monitoring via Channel Access) ==========
  - epics-base:                             # Channel Access reader type for legacy EPICS deployments
      - name: base_reader_a                 # unique reader instance name for logs and metrics
        thread-pool: 1                      # optional; default: 1; conversion worker pool for processing PV updates
        column-batch-size: 50               # optional; default: 50; max NTTable columns processed per batch
        monitor-poll-threads: 2             # optional; default: 2; threads draining monitor events from the queue
        monitor-poll-interval-ms: 5         # optional; default: 5; sleep between monitor queue polls
        pvs:                                # PV list monitored by this reader
          - name: "PV:NAME:1"               # PV name to subscribe to
          - name: "PV:NAME:2"               # PV name to subscribe to
            option: "VALUE"                 # optional; scalar channel option passed to the monitor

  # ========== EPICS Archiver Reader (Historical Data Retrieval) ==========
  - epics-archiver:                         # archiver reader type for historical fetches or tail polling
      - name: archiver_reader_a             # unique reader instance name for logs and metrics
        hostname: "archiver.example.com:11200"  # archiver appliance host and port
        mode: "historical_once"             # optional; fetch a fixed historical window once
        start-date: "2026-01-01T00:00:00Z" # inclusive ISO 8601 start time for the query window
        end-date: "2026-01-31T23:59:59Z"   # optional; inclusive ISO 8601 end time for the query window
        tls-verify-peer: true               # optional; default: true; verify the server certificate chain
        tls-verify-host: true               # optional; default: true; verify the certificate hostname
        connect-timeout-sec: 30             # optional; default: 30; connection setup timeout in seconds
        total-timeout-sec: 300              # optional; default: 300; total request timeout in seconds
        pvs:                                # PV list to fetch from the archiver
          - name: "SYSTEM:SENSOR:TEMPERATURE:MAIN"   # archived PV name to retrieve
          - name: "SYSTEM:ACTUATOR:PRESSURE:OUTLET"  # archived PV name to retrieve

      - name: archiver_reader_tail_polling  # reader instance that repeatedly tails recent history
        hostname: "archiver.example.com:11200"  # archiver appliance host and port
        mode: "periodic_tail"               # continuously poll recent archiver data
        poll-interval-sec: 5                # required; seconds between polling cycles
        lookback-sec: 5                     # optional; trailing history window fetched each cycle
        batch-duration-sec: 1               # optional; split output batches by sample-time span in seconds
        pvs:                                # PV list to fetch from the archiver
          - name: "SYSTEM:SENSOR:TEMPERATURE:MAIN"   # archived PV name to retrieve

metrics:                                    # optional; Prometheus exporter settings
  endpoint: 0.0.0.0:9464                    # bind address for the metrics HTTP endpoint
```

### Supported Reader Types

| Reader Type      | Description                                                       |
|------------------|-------------------------------------------------------------------|
| `epics-pvxs`     | Event-driven PVAccess reader using PVXS (recommended)             |
| `epics-base`     | Polling-based Channel Access reader for legacy systems            |
| `epics-archiver` | Historical and periodic-tail reader from EPICS Archiver Appliance |

For detailed reader documentation, see [Reader Types](docs/readers.md).

`mldp-pool` values mirror the driver's `provider-name` and target URLs but add connection-pool sizing.

- `ingestion-url`: ingestion service address (DpIngestionService)
- `query-url`: query service address (DpQueryService)

Readers are defined
as sequences under `reader[]`, each with a `name` and an optional `pvs` list; if `pvs` is omitted, the reader will
start without predefined channels.

`mldp-pool.credentials` accepts:

- `none` (insecure, no TLS)
- `ssl` (TLS with system defaults)
- a map with optional `pem-cert-chain`, `pem-private-key`, `pem-root-certs` paths (TLS with explicit PEM files)

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
- [Reader Types](docs/readers.md) - Available reader implementations (EPICS Base, PVXS)
- [Implementing Custom Readers](docs/readers-implementation.md) - Guide to creating new reader types
- [Logging Abstraction Guide](docs/logging.md) - How `util::log` works and how to implement custom logger classes
- [HTTP Transport Provider](docs/http-provider.md) - Shared `util/http` abstraction and curl-backed implementation for HTTP-based readers

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
