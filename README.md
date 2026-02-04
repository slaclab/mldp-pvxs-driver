# MLDP PVXS Driver

![logo](./logos/SLAC-lab-hires.png)

This driver integrates PVXS-exposed EPICS process variables with the SLAC MLDP ingestion API (see [MLDP](https://github.com/osprey-dcs/dp-service.git)), translating PV updates into MLDP payloads and forwarding them over gRPC so downstream analysis pipelines receive timely ML measurements while remaining compatible with other data sources.

## Configuration

When running the controller/CLI orchestrator, the full config is a single YAML document. Every block shown is required
unless marked optional.

```yaml
controller_thread_pool: 2

mldp_pool:
  provider_name: pvxs_provider
  provider_description: "PVXS aggregate provider"   # optional
  url: https://ingest.example:443
  min_conn: 1
  max_conn: 4
  credentials: # optional
    pem_cert_chain: /etc/certs/client.crt
    pem_private_key: /etc/certs/client.key
    pem_root_certs: /etc/certs/ca.crt

reader:                                  # optional; omit to start with no readers
  # PVXS reader (event-driven, recommended for modern EPICS)
  - epics-pvxs:
      - name: pvxs_reader
        thread_pool_size: 2              # optional; conversion thread pool size
        column_batch_size: 50            # optional; max columns per batch for NTTable
        pvs:
          - name: MY:PV:NAME
          - name: BSA:TABLE:PV
            option:                      # optional; for SLAC BSAS NTTable with row timestamps
              type: slac-bsas-table
              tsSeconds: secondsPastEpoch
              tsNanos: nanoseconds

  # Base reader (polling-based, for legacy Channel Access)
  - epics-base:
      - name: base_reader
        thread_pool_size: 2              # optional; conversion thread pool size
        monitor_poll_threads: 2          # optional; number of polling threads
        monitor_poll_interval_ms: 5      # optional; polling interval in ms
        pvs:
          - name: LEGACY:PV:ONE
          - name: LEGACY:PV:TWO

metrics:                                 # optional; omit to disable Prometheus endpoint
  endpoint: 0.0.0.0:9464
```

### Supported Reader Types

| Reader Type | Description |
|-------------|-------------|
| `epics-pvxs` | Event-driven PVAccess reader using PVXS (recommended) |
| `epics-base` | Polling-based Channel Access reader for legacy systems |

For detailed reader documentation, see [Reader Types](docs/readers.md).

`mldp_pool` values mirror the driver's `provider_name` and target URL but add connection-pool sizing. Readers are defined
as sequences under `reader[]`, each with a `name` and an optional `pvs` list; if `pvs` is omitted, the reader will
start without predefined channels.

`mldp_pool.credentials` accepts:

- `none` (insecure, no TLS)
- `ssl` (TLS with system defaults)
- a map with optional `pem_cert_chain`, `pem_private_key`, `pem_root_certs` paths (TLS with explicit PEM files)

## Command-line interface

The driver is configured via a YAML file (see above) and is started from the command line.

### Usage

```bash
mldp_pvxs_driver [--help] [--version] [--config PATH] [--log-level LEVEL] [--metrics-output FILE] [--metrics-interval SECONDS]
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

### Examples

```bash
# Run with an explicit config file
./mldp_pvxs_driver --config ./config.yaml

# Enable debug logging
./mldp_pvxs_driver --config ./config.yaml --log-level debug

# Show help/version
./mldp_pvxs_driver --help
./mldp_pvxs_driver --version
```

For periodic metrics dumps and manual triggers (Ctrl+P, Ctrl+D, SIGUSR1/SIGQUIT), see `docs/metrics-export-guide.md`.

## Architecture

This project uses a pipeline-style architecture: PVXS clients feed PV updates into a bounded work queue; the core driver converts and enriches events and dispatches them to the MLDP ingestion service using a connection pool of gRPC channels; reader implementations consume and re-publish or transform events as needed.

### Documentation

- [Architecture Overview](docs/architecture.md) - System architecture, data flow, and design patterns
- [Reader Types](docs/readers.md) - Available reader implementations (EPICS Base, PVXS)
- [Implementing Custom Readers](docs/readers-implementation.md) - Guide to creating new reader types

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
- A standalone executable artifact (currently named `mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1`).
- An AppImage for easier distribution (currently named `mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage`).

### Builder image + build cache (for developers)

CI publishes a reusable **builder image** tagged by EPICS + PVXS versions:

```bash
docker pull ghcr.io/slaclab/mldp-pvxs-driver/build:epics-7.0.8.1-pvxs-1.4.1
```

You can also use it as a BuildKit cache source in local `docker buildx` builds (if the tag exists, it will be used; if it does not exist yet, the build still succeeds):

```bash
docker login ghcr.io

docker buildx build \
  -f .devcontainer/Dockerfile \
  --build-arg BASE_OS_IMAGE=ubuntu:noble \
  --build-arg EPICS_VERSION=R7.0.8.1 \
  --build-arg PVXS_VERSION=1.4.1 \
  --cache-from type=registry,ref=ghcr.io/slaclab/mldp-pvxs-driver/build:epics-7.0.8.1-pvxs-1.4.1 \
  -t mldp-pvxs-driver-dev:latest \
  --load \
  .
```

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
