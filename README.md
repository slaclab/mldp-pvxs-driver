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

## Architecture

This project uses a pipeline-style architecture: PVXS clients feed PV updates into a bounded work queue; the core driver converts and enriches events and dispatches them to the MLDP ingestion service using a connection pool of gRPC channels; reader implementations consume and re-publish or transform events as needed. See the detailed diagram and design notes in [docs/architecture.md](docs/architecture.md).

```mermaid
graph
    subgraph "Driver Application"
        Config[Configuration]
        Runner[PVXSDPIngestionDriver Instance]
        Run[Await Changes]
    end

    subgraph "PVXSDPIngestionDriver"
        Driver[Constructor]
        Convert[Convert PV to DP format]
        Thread1[Threaded gRPC Call]
        Ingest[Ingest PV value]

        Ingest --> Convert
    end

    subgraph "PVXS Client Context"
        Monitor[Monitor Subscriptions]
        WorkQueue[PV Queue]
    end

    subgraph "MLDP API"
        Register[registerProvider]
        IngestData[ingestData]
    end

    subgraph "EPICS IOCs"
        PVServer[PV Channels]
    end

    Config -->|Sets Values| Runner
    Runner --> Driver
    Runner --> Run

    Driver -->|Registers Provider| Register

    Driver -->|Creates Context| Monitor
    Monitor -->|Subscribes to| PVServer

    PVServer -.->|Pushes to| WorkQueue

    WorkQueue -->|When popped from| Ingest

    Convert -->|Spawns| Thread1

    Thread1 -->|Calls| IngestData

    Run -.->|Pops from| WorkQueue
```

For developer information and contribution guidelines see [CONTRIBUTING.md](CONTRIBUTING.md).

## Releases

Tagged releases (`vX.Y.Z`) publish:
- A container image (recommended way to run).
- A standalone executable artifact (currently named `mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1`).

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

You can inspect the dependencies on your target system with `ldd ./mldp_pvxs_driver-ubuntu-noble-epics-R7.0.8.1`.
