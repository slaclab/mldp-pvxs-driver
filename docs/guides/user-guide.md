# MLDP PVXS Driver — User Guide

> This guide is for **operators and physicists** who want to configure and run the driver.
> No C++ knowledge required. For internal architecture details see [architecture.md](../reference/architecture.md).

---

## What Does This Driver Do?

The MLDP PVXS Driver watches EPICS Process Variables (PVs) — live or historical — and
forwards their data to one or more destinations:

- **MLDP** — the Machine Learning Data Platform (over gRPC)
- **HDF5 files** — local disk storage for analysis

You describe *what to read* and *where to write* in a single YAML configuration file,
then run the driver. It handles everything in between.

```
EPICS PVs / Archiver
      │
   [Reader]          ← you choose the source type
      │
 [Controller]        ← routes data between readers and writers
      │
 [Writer(s)]         ← you choose the destinations
      │        │
    MLDP     HDF5
```

---

## Core Concepts

### Reader — Where Data Comes From

A **reader** connects to a data source and pushes updates into the driver.
You can run multiple readers at the same time.

| Reader type | Use when… |
|---|---|
| `epics-pvxs` | You want **live PV monitoring** (recommended for new deployments) |
| `epics-base` | You need **Channel Access (CA)** compatibility with older EPICS systems |
| `epics-archiver` | You want **historical data** from an Archiver Appliance |

> 📖 Details: [epics-pvxs reader](../readers/epics-pvxs-reader.md) · [epics-base reader](../readers/epics-base-reader.md) · [archiver reader](../readers/epics-archiver-reader.md)

### Writer — Where Data Goes

A **writer** receives data batches and stores or forwards them.
You can run multiple writers simultaneously — every batch goes to all of them.

| Writer type | Use when… |
|---|---|
| `mldp` | You want to send data to the MLDP ingestion service over gRPC |
| `hdf5` | You want one HDF5 file per PV, stored locally |
| `hdf5-merge` | You want all PVs in a single rotating HDF5 file |

> 📖 Details: [MLDP writer](../writers/mldp-writer.md) · [HDF5 writer](../writers/hdf5-writer.md)

### Controller — The Glue

The controller wires readers and writers together. You don't interact with it directly —
it is configured implicitly by your YAML file. By default every reader feeds every writer.
Use the optional `routing:` block to restrict which reader feeds which writer.

---

## Configuration File Structure

All configuration lives in a single YAML file passed to the driver at startup:

```bash
mldp_pvxs_driver --config my-config.yaml
```

The file has four top-level sections:

```yaml
writer:          # required — one or more output destinations
  ...

reader:          # required — one or more data sources
  ...

metrics:         # optional — Prometheus metrics endpoint
  ...

routing:         # optional — restrict which reader feeds which writer
  ...
```

---

## Example 1 — Live PVs → MLDP (simplest setup)

Monitor three live EPICS PVs and forward every update to MLDP.

```yaml
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: my_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

reader:
  - epics-pvxs:
      - name: pvxs_main
        pvs:
          - name: SITE:SYS:PRESSURE
          - name: SITE:SYS:TEMPERATURE
          - name: SITE:SYS:BEAM_CURRENT

metrics:
  endpoint: "0.0.0.0:9464"
```

**What happens:**
1. Driver subscribes to the three PVs via PVAccess (PVXS).
2. Every time a PV changes value, a data batch is created.
3. The batch is forwarded to the MLDP gRPC stream immediately.

---

## Example 2 — Live PVs → MLDP + local HDF5 backup

Same as Example 1, but also save every update to HDF5 files on disk.

```yaml
writer:
  mldp:
    - name: mldp_primary
      mldp-pool:
        provider-name: my_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5        # directory where files are written
      max-file-age-s: 3600         # rotate to a new file every hour
      max-file-size-mb: 512        # also rotate if file reaches 512 MiB

reader:
  - epics-pvxs:
      - name: pvxs_live
        pvs:
          - name: SITE:SYS:PRESSURE
          - name: SITE:SYS:TEMPERATURE

metrics:
  endpoint: "0.0.0.0:9464"
```

> ⚠️ HDF5 support requires the driver to be built with `-DMLDP_PVXS_ENABLE_HDF5=ON`.
> Check with your system administrator if unsure.

---

## Example 3 — Historical data from Archiver → MLDP

Pull one day of historical data from an Archiver Appliance and push it to MLDP, then stop.

```yaml
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: archiver_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

reader:
  - epics-archiver:
      - name: archiver_historical
        hostname: archiver.example.com:11200
        mode: historical_once
        start-date: "2026-01-01T00:00:00Z"
        end-date:   "2026-01-02T00:00:00Z"
        pvs:
          - name: SITE:SYS:BEAM_ENERGY
          - name: SITE:SYS:PRESSURE
```

**What happens:**
1. Driver connects to the Archiver HTTP API.
2. Fetches all samples for the two PVs between the given timestamps.
3. Forwards to MLDP, then exits.

---

## Example 4 — Continuous archiver tail → MLDP

Continuously poll the Archiver for new data every 10 seconds and forward it to MLDP.

```yaml
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: archiver_tail_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

reader:
  - epics-archiver:
      - name: archiver_tail
        hostname: archiver.example.com:11200
        mode: periodic_tail
        poll-interval-sec: 10    # query every 10 seconds
        lookback-sec: 10         # fetch the last 10 seconds each time
        pvs:
          - name: SITE:SYS:BEAM_ENERGY
```

---

## Example 5 — Mixed sources with routing

Two readers (live PVXS + legacy CA), two writers (MLDP + HDF5).
Route live PVs to both writers, legacy PVs only to HDF5.

```yaml
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: my_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
      max-file-age-s: 3600

reader:
  - epics-pvxs:
      - name: pvxs_live
        pvs:
          - name: SITE:SYS:PRESSURE

  - epics-base:
      - name: base_legacy
        pvs:
          - name: LEGACY:CA:PV:1

routing:
  mldp_main:            # this writer receives only from pvxs_live
    from:
      - pvxs_live
  hdf5_local:           # this writer receives from both readers
    from:
      - pvxs_live
      - base_legacy
```

> If you omit the `routing:` block entirely, every writer receives from every reader.

---

## Example 6 — Source filtering within a routing rule

Same two readers, but filter which PV names reach each writer using glob patterns on the PV name.

```yaml
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: my_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        credentials: ssl

  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
      max-file-age-s: 3600

reader:
  - epics-pvxs:
      - name: pvxs_live
        pvs:
          - name: SITE:BPM:01:X
          - name: SITE:BPM:01:Y
          - name: SITE:TEST:01:X    # test PV — should not reach MLDP

routing:
  mldp_main:
    from:
      - pvxs_live
    include:
      - "SITE:BPM:*"               # only production BPMs reach MLDP
    exclude:
      - "SITE:TEST:*"              # belt-and-suspenders: drop test PVs
  hdf5_local:
    from:
      - pvxs_live                  # HDF5 gets everything (no include/exclude)
```

**Filter rules** (applied per PV name):

| PV name | include match | exclude match | Reaches MLDP? |
|---|---|---|---|
| `SITE:BPM:01:X` | ✅ | ✗ | **yes** |
| `SITE:BPM:01:Y` | ✅ | ✗ | **yes** |
| `SITE:TEST:01:X` | ✗ | ✅ | **no** |

> `*` matches `:` in EPICS PV names. Patterns are case-sensitive.
> `include` absent = accept all sources. `exclude` absent = drop nothing.

> If you omit the `routing:` block entirely, every writer receives from every reader.

---

## Routing and Source Filtering

### Reader-to-Writer Routing

By default every reader feeds every writer. Add a `routing:` block to change that.

Each writer lists which readers it accepts under `from:`. Use `"all"` to accept every reader.

```yaml
routing:
  mldp_main:
    from: [pvxs_live, archiver_tail]  # only these readers feed mldp_main
  hdf5_local:
    from: [all]                       # every reader feeds hdf5_local
```

> ⚠️ When `routing:` is present, any writer **not** listed receives nothing.

### Source Filtering (include / exclude)

Each routing entry can also filter by PV name using glob patterns:

```yaml
routing:
  mldp_main:
    from: [pvxs_live]
    include:
      - "SITE:BPM:*"    # accept only these PV names
      - "GUN:SOL:*"
    exclude:
      - "SITE:TEST:*"   # always drop test PVs (applied after include)
```

| Scenario | include | exclude | Result |
|---|---|---|---|
| No patterns set | — | — | all PVs pass |
| Include only | `SITE:BPM:*` | — | only matching PVs pass |
| Exclude only | — | `SITE:TEST:*` | all PVs pass except matching |
| Both | `SITE:BPM:*` | `SITE:TEST:*` | matching include AND NOT matching exclude |

> `*` matches `:` in EPICS PV names. Patterns are case-sensitive.

> 📖 Full details: [controller.md](../reference/controller.md#reader-to-writer-routing)

---

## Key Configuration Parameters

### Reader — `epics-pvxs` and `epics-base`

| Parameter | Required | Default | Description |
|---|---|---|---|
| `name` | ✅ | — | Unique name for this reader instance |
| `pvs` | ✅ | — | List of EPICS PV names to monitor |
| `thread-pool` | | 2 | Worker threads for data conversion |

### Reader — `epics-archiver`

| Parameter | Required | Default | Description |
|---|---|---|---|
| `name` | ✅ | — | Unique name for this reader instance |
| `hostname` | ✅ | — | Archiver host and port (e.g. `archiver.example.com:11200`) |
| `mode` | ✅ | — | `historical_once` or `periodic_tail` |
| `pvs` | ✅ | — | List of PV names to fetch |
| `start-date` | for `historical_once` | — | ISO 8601 start time |
| `end-date` | | — | ISO 8601 end time (omit = now) |
| `poll-interval-sec` | for `periodic_tail` | — | How often to query the Archiver |
| `lookback-sec` | | = poll-interval | How far back each poll fetches |
| `connect-timeout-sec` | | 30 | HTTP connection timeout (seconds) |
| `total-timeout-sec` | | 300 | Total HTTP timeout (0 = no limit) |

### Writer — `mldp`

| Parameter | Required | Default | Description |
|---|---|---|---|
| `name` | ✅ | — | Unique name for this writer instance |
| `mldp-pool.provider-name` | ✅ | — | Provider name registered in MLDP |
| `mldp-pool.ingestion-url` | ✅ | — | gRPC ingestion service address |
| `mldp-pool.credentials` | | none | `none`, `ssl`, or TLS certificate map |
| `thread-pool` | | 1 | Worker threads |
| `stream-max-bytes` | | 2097152 | Flush gRPC stream at this payload size |
| `stream-max-age-ms` | | 200 | Flush gRPC stream after this many ms |

### Writer — `hdf5` / `hdf5-merge`

| Parameter | Required | Default | Description |
|---|---|---|---|
| `name` | ✅ | — | Unique name for this writer instance |
| `base-path` | ✅ | — | Directory where HDF5 files are written |
| `max-file-age-s` | | 3600 | Rotate to a new file every N seconds |
| `max-file-size-mb` | | 512 | Rotate when file reaches N MiB |
| `flush-interval-ms` | | 1000 | Flush buffers to disk every N ms |
| `compression-level` | | 0 | DEFLATE compression level 0–9 (0 = off) |

### Metrics (optional)

```yaml
metrics:
  endpoint: "0.0.0.0:9464"      # Prometheus scrape endpoint
  scan-interval-seconds: 5
```

---

## Choosing the Right Reader

```
Need live data?
├── EPICS system uses PVAccess (PVA)?  → epics-pvxs  ✅ recommended
└── EPICS system uses Channel Access (CA) only?  → epics-base

Need historical data?
├── One-time backfill?  → epics-archiver (mode: historical_once)
└── Continuous near-real-time feed from Archiver?  → epics-archiver (mode: periodic_tail)
```

---

## Further Reading

| Topic | Document |
|---|---|
| Full YAML schema reference | [configuration.md](configuration.md) |
| PVXS reader details | [readers/epics-pvxs-reader.md](../readers/epics-pvxs-reader.md) |
| CA reader details | [readers/epics-base-reader.md](../readers/epics-base-reader.md) |
| Archiver reader details | [readers/epics-archiver-reader.md](../readers/epics-archiver-reader.md) |
| MLDP writer details | [writers/mldp-writer.md](../writers/mldp-writer.md) |
| HDF5 writer details | [writers/hdf5-writer.md](../writers/hdf5-writer.md) |
| Routing and source filtering | [controller.md](../reference/controller.md#reader-to-writer-routing) |
| Architecture and internals | [architecture.md](../reference/architecture.md) |
| Metrics and monitoring | [metrics-export-guide.md](../metrics/metrics-export-guide.md) |
