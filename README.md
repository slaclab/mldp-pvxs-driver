# MLDP PVXS Driver

## Architecture

```mermaid
graph
    subgraph "Driver Application"
        Main[main]
        Config[Parse Configuration]
        Main --> Config
    end

    subgraph "PVXSDPIngestionDriver"
        Driver[Constructor]
        Run[Monitor PVs for changes]
        Convert[Convert PV to DP format]
        Thread1[Threaded gRPC Call]
        Ingest[Ingest PV value]

        Driver --> Run
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

    Main -->|Creates| Driver
    Config -->|Reads PV Names,<br/>Server Address,<br/>Credentials| Driver

    Driver -->|Registers Provider| Register

    Driver -->|Creates Context| Monitor
    Monitor -->|Subscribes to| PVServer
    Monitor -->|Stores in| WorkQueue

    PVServer -.->|PV Updates| WorkQueue

    WorkQueue -->|Calls| Ingest

    Convert -->|Spawns| Thread1

    Thread1 -->|Calls| IngestData

    Run -.->|Pushes to| WorkQueue
```

## Building

`PROTO_PATH` and `PVXS_BASE` are required to either be set as environment variables or passed to the CMake configuration
step. The former should be the path to the parent directory of MLDP's protobuf definitions. The latter should be the
directory containing the pvxs library.
