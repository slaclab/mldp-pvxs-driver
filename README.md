![logo](./logos/SLAC-lab-hires.png)
# MLDP PVXS Driver

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

reader:
  - epics:
      - name: epics_reader_a
        pvs:
          - name: pv1
            option: chan://one          # optional PVXS option string
          - name: pv2
      - name: epics_reader_b
        pvs:
          - name: pv3

metrics:                                 # optional; omit to disable Prometheus endpoint
  endpoint: 0.0.0.0:9464
```

`mldp_pool` values mirror the driver’s `provider_name` and target URL but add connection-pool sizing. Readers are defined
as sequences under `reader[].epics`, each with a `name` and an optional `pvs` list; if `pvs` is omitted, the reader will
start without predefined channels.

## Command-line interface

The driver is configured via a YAML file (see above) and is started from the command line.

### Usage

```bash
mldp_pvxs_driver [--help] [--version] [--config PATH] [--log-level LEVEL]
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

## Architecture

This project uses a pipeline-style architecture: PVXS clients feed PV updates into a bounded work queue; the core driver converts and enriches events and dispatches them to the MLDP ingestion service using a connection pool of gRPC channels; reader implementations consume and re-publish or transform events as needed. See the detailed diagram and design notes in [docs/architecture.md](docs/architecture.md).

For developer information and contribution guidelines see [CONTRIBUTING.md](CONTRIBUTING.md).

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

### AppImage notes

The AppImage is intended to bundle the driver plus its runtime shared libraries (including EPICS/PVXS) to reduce
host setup. It still requires a compatible Linux base system (kernel + glibc). Some distributions may also require
FUSE depending on how AppImages are executed.

#### Running the AppImage

1. Download the AppImage from the GitHub Release assets (example name:
  `mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage`).
2. Make it executable:
  `chmod +x ./mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage`
3. Run it (point at your config file):
  `./mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage --config config.yaml`

If the AppImage fails to start due to FUSE restrictions, you can extract and run it without mounting:

- `./mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1-pvxs-1.4.1-x86_64.AppImage --appimage-extract`
- `./squashfs-root/AppRun --config config.yaml`
