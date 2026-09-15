# MLDP PVXS Driver — User Guide

> This guide is for **operators and physicists** who want to configure and run the driver.
> No C++ knowledge required. For internal architecture details see [architecture.md](../reference/architecture.md).

---

## What Does This Driver Do?

The MLDP PVXS Driver reads data from one or more sources and forwards it to one or more
destinations. Sources and destinations are pluggable — the built-in implementations target
EPICS control systems and MLDP/HDF5 storage, but the reader/writer architecture is
general-purpose and not limited to EPICS data.

- **Readers** — connect to a data source (live EPICS PVs, historical archiver data, metadata services, or custom sources)
- **Writers** — deliver data to a destination (MLDP gRPC, HDF5 files, or custom sinks)

You describe *what to read* and *where to write* as one accumulated configuration,
then run the driver. That effective configuration can come from one or more YAML files
plus dotted `PATH=VALUE` assignments passed with `-c`.

```
[Data Source(s)]
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

A **reader** connects to any data source and pushes updates into the driver.
You can run multiple readers at the same time, and mix different reader types.
The reader interface is pluggable — the built-in implementations target EPICS control
systems, but custom readers can be registered for any source (databases, message
brokers, REST APIs, etc.).

**Built-in reader types:**

| Reader type | Use when… |
|---|---|
| `epics-pvxs` | You want **live PV monitoring** (recommended for new deployments) |
| `epics-base` | You need **Channel Access (CA)** compatibility with older EPICS systems |
| `epics-archiver` | You want **historical data** from an Archiver Appliance |
| `epics-ds-metadata` | You want to fetch **PV metadata** (names, tags) from an EPICS Directory Service and persist it to MLDP |

> 📖 Built-in reader docs: [epics-pvxs reader](../readers/epics-pvxs-reader.md) · [epics-base reader](../readers/epics-base-reader.md) · [archiver reader](../readers/epics-archiver-reader.md) · [epics-ds-metadata reader](../readers/epics-ds-metadata-reader.md)
> 📖 Custom readers: [readers-implementation.md](../readers/readers-implementation.md)

### Writer — Where Data Goes

A **writer** receives data batches and stores or forwards them.
You can run multiple writers simultaneously — every batch goes to all of them.
Like readers, the writer interface is pluggable; custom writers can be registered
for any destination.

**Built-in writer types:**

| Writer type | Use when… |
|---|---|
| `mldp` | You want to send data to the MLDP ingestion service over gRPC |
| `hdf5` | You want one HDF5 file per source channel, stored locally |
| `hdf5-merge` | You want all source channels in a single rotating HDF5 file |

> 📖 Built-in writer docs: [MLDP writer](../writers/mldp-writer.md) · [HDF5 writer](../writers/hdf5-writer.md)
> 📖 Custom writers: [writers-implementation.md](../writers/writers-implementation.md)

### Controller — The Glue

The controller wires readers and writers together. You don't interact with it directly —
it is configured implicitly by your YAML file. By default every reader feeds every writer.
Use the optional `routing:` block to restrict which reader feeds which writer.

### Enricher — Adjusting Batches Before Delivery

An **enricher** is a named, reusable transformation applied just before a queued writer converts and stores a batch. Define enrichers globally and attach them to the writers that need them. This is useful for adding run metadata, attaching attributes to selected columns, correcting timestamps, or assigning MLDP shard slots without changing readers.

```yaml
enrichers:
  run-context:
    type: static-metadata
    metadata: {experiment_id: run-42}
writer:
  mldp:
    - name: mldp_main
      enrichers: [run-context]
```

When two writers use the same enricher name, they share one instance and therefore share its state. For full type, ordering, shard-slot, and Python-script details, see the [Payload Enricher Guide](../enrichers/enrichers.md).

---

## Generating Your Configuration

You have two options: use the **interactive wizard** (recommended for beginners) or write YAML manually.

### Option 1: Interactive Wizard (Easiest)

Run the configuration wizard to build a config file with a full-screen 3-panel terminal UI:

```bash
mldp_pvxs_driver config wizard
```

The wizard opens a persistent split-panel interface:

```
┌─ pvxs-driver config ──────────────────── config.yaml ─ [s]ave [v]alidate [q]uit ─┐
│ ▶ Controller          │  Basic Settings                  │  thread-pool            │
│ ▷ Writers             │  Name:         [mldp_main      ] │  Number of worker       │
│   └ mldp_main  (MLDP) │  Thread Pool:  [4              ] │  threads for ingestion. │
│ ▷ Readers             │                                  │  Min 1.                 │
│   └ pvxs_r0    (PVXS) │  MLDP Pool                       │                         │
│ ▷ Metrics             │  Ingestion URL:[dp-ingest:...  ] │  ✗ Must be positive int │
│ ▷ Routing             │                                  │                         │
├───────────────────────┴──────────────────────────────────┴─────────────────────────┤
│  Writers: 1 │ Readers: 1 │ Metrics: off │ [a]dd [d]del          Valid ✓  config.yaml│
└─────────────────────────────────────────────────────────────────────────────────────┘
```

**Navigation:**

| Key | Action |
|---|---|
| `↑` / `↓` | Move between items in the sidebar (left pane) |
| `→` | Enter the form pane for the selected item |
| `←` | Return to the sidebar from the form pane |
| `Tab` / `Shift+Tab` | Move to the next / previous field within the form |
| `a` | Add a new writer or reader (type chosen in pop-up modal) |
| `d` | Delete the currently selected item (confirmation required) |
| `s` | Save the config to disk |
| `v` | Validate the current config |
| `q` / `Esc` | Quit (prompts if there are unsaved changes) |

**Mouse support:** the wizard is fully mouse-aware. You can click on any sidebar
item to select it, click on a form field to focus it, toggle checkboxes with a
click, and scroll long forms with the scroll wheel.

The right panel shows **contextual help** that updates automatically:
- **Sidebar focused** — describes the selected tree item (Writers group, MLDP Writer,
  EPICS reader, Metrics, etc.)
- **Form focused** — shows documentation for the field that currently has focus;
  updates as you Tab between fields without requiring you to type anything.

The status bar shows a live count of writers/readers and validation state.

**To pre-populate from an existing config:**

```bash
mldp_pvxs_driver config wizard --from config.yaml
```

> **Note:** the writer type (MLDP, HDF5, HDF5-merge) and reader type
> (epics-pvxs, epics-base, epics-archiver) are chosen in the add modal and
> cannot be changed afterwards. Delete the entry and add a new one to change type.

**To open the wizard focused on adding a new entry:**

```bash
# Opens wizard with add-modal pre-triggered (PATH always last)
mldp_pvxs_driver config add config.yaml
mldp_pvxs_driver config add writer config.yaml
mldp_pvxs_driver config add reader config.yaml
```

**To remove a named entry without the wizard:**

```bash
mldp_pvxs_driver config remove writer config.yaml --name mldp_main
mldp_pvxs_driver config remove reader config.yaml --name pvxs_live
```

### Option 2: Start from a Template

If you prefer to hand-edit YAML, start with a template:

```bash
# Minimal template (MLDP writer + PVXS reader)
mldp_pvxs_driver config template --minimal > config.yaml

# Full template (both writers and all reader types)
mldp_pvxs_driver config template --full > config.yaml
```

Then edit `config.yaml` to fill in your hostnames, URLs, PV names, and credentials.

### Option 3: Validate and Inspect

After creating a config, always validate it before running the driver:

```bash
# Check for errors and warnings
mldp_pvxs_driver config validate config.yaml

# See a summary of what will run
mldp_pvxs_driver config list config.yaml

# Dry-run the startup without starting readers/writers
mldp_pvxs_driver -c config.yaml --dry-run

# Dry-run with extra dotted assignments merged on top
mldp_pvxs_driver \
  -c config.yaml \
  -c metrics.endpoint=0.0.0.0:9464 \
  -c reader.hdf5-bsas-gen1[0].file-path=/tmp/bsas.h5 \
  --dry-run
```

---

## Startup Configuration Inputs

At startup, each `-c` value contributes to one final effective configuration.

```bash
mldp_pvxs_driver -c my-config.yaml
```

Rules:

1. If `-c` points to an existing file, that YAML file is loaded.
2. If `-c` is not a file, it must be a dotted assignment like `metrics.endpoint=0.0.0.0:9464`.
3. All `-c` values are merged in the order provided.
4. Non-file `-c` values that are not valid dotted assignments are rejected.

Examples:

```bash
# Single file
mldp_pvxs_driver -c my-config.yaml

# Multiple files merged in order
mldp_pvxs_driver -c base.yaml -c site.yaml -c local.yaml

# File plus dotted assignments
mldp_pvxs_driver \
  -c base.yaml \
  -c metrics.endpoint=0.0.0.0:9464 \
  -c reader.hdf5-bsas-gen1[0].file-path=/tmp/bsas.h5
```

The resulting effective configuration has four top-level sections:

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

## Routing and Source Filtering

By default, every reader feeds every writer. Use the optional `routing:` block to change this behavior.

### Reader-to-Writer Routing

Each writer lists which readers it accepts under `from:`. Use `"all"` to accept every reader.

```yaml
routing:
  mldp_main:
    from: [pvxs_live, archiver_tail]  # only these readers feed mldp_main
  hdf5_local:
    from: [all]                       # every reader feeds hdf5_local
```

**Behavior:**

- If `routing:` block is **absent**: all readers feed all writers (all-to-all, default).
- If `routing:` block is **present**: only writers listed in the block receive data. Unlisted writers receive nothing.

> ⚠️ When `routing:` is present, any writer **not** listed receives nothing.

### Source Filtering (include / exclude)

Each routing entry can also filter by PV name using glob patterns. Filters apply per PV name:

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

**Filter matching logic:**

| Scenario | include | exclude | Result |
|---|---|---|---|
| No patterns set | — | — | all PVs pass through |
| Include only | `SITE:BPM:*` | — | only matching PVs pass |
| Exclude only | — | `SITE:TEST:*` | all PVs pass except matching |
| Both | `SITE:BPM:*` | `SITE:TEST:*` | PVs matching include AND NOT matching exclude |

**Pattern rules:**

- `*` matches any characters including `:` (EPICS namespace separator).
- Patterns are **case-sensitive**.
- If `include:` is absent, all sources are accepted initially (then `exclude:` filters them).
- If `exclude:` is absent, nothing is dropped by the exclude step.

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

### Reader — `epics-ds-metadata`

| Parameter | Required | Default | Description |
|---|---|---|---|
| `name` | ✅ | — | Unique name for this reader instance |
| `service` | | `ds` | PVA service name to call via RPC |
| `query` | | `%` | Query pattern for the NTURI request |
| `timeout-sec` | | `5.0` | RPC call timeout in seconds |
| `source-name-column` | | `channelName` | NTTable column carrying the PV / source name |
| `tags-column` | | *(empty)* | NTTable column for comma-separated tags; empty = disabled |
| `rescan-interval-sec` | | `0.0` | Rescan interval; `0` = fetch once then stop |

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

### Periodic File Dumps (CLI flags)

In addition to the Prometheus endpoint, the driver can write metrics to a
[JSON Lines](https://jsonlines.org/) file at a regular interval.
These flags are passed on the command line, not in the YAML file:

| Flag | Default | Description |
|---|---|---|
| `--metrics-output FILE` | `metrics.jsonl` | Path for periodic JSONL metric exports |
| `--metrics-interval SECONDS` | `5` | Dump interval in seconds |

```bash
mldp_pvxs_driver -c config.yaml \
  --metrics-output /data/metrics.jsonl \
  --metrics-interval 30
```

> The dumper is **not started automatically** when these flags are present.
> You must press **Ctrl+D** in the foreground terminal to start it (see below).

### Runtime Controls

While the driver is running in the foreground terminal:

| Key / Signal | Action |
|---|---|
| **Ctrl+P** | Print a one-shot metrics snapshot to stdout |
| **Ctrl+D** | **Toggle** the periodic file dumper on/off |
| `kill -USR1 <pid>` | Same as Ctrl+P (signal-based) |
| `kill -QUIT <pid>` | Same as Ctrl+P (signal-based) |

**Ctrl+D toggle behaviour:**
- First press → starts the dumper, writing to `--metrics-output` every `--metrics-interval` seconds.
- Second press → stops the dumper (no more file writes until toggled on again).
- The output file is **appended to**, not overwritten, so data from previous runs is preserved.

> 📖 Full details: [metrics-export-guide.md](../metrics/metrics-export-guide.md)

---

## Choosing the Right Reader

The built-in readers all target EPICS control systems. If your data source is not EPICS,
implement a custom reader — see [readers-implementation.md](../readers/readers-implementation.md).

**For EPICS data sources:**

```
Need live data?
├── EPICS system uses PVAccess (PVA)?  → epics-pvxs  ✅ recommended
└── EPICS system uses Channel Access (CA) only?  → epics-base

Need historical data?
├── One-time backfill?  → epics-archiver (mode: historical_once)
└── Continuous near-real-time feed from Archiver?  → epics-archiver (mode: periodic_tail)

Need to populate MLDP with PV metadata (names, tags) from a Directory Service?
└── epics-ds-metadata  (pair with mldp-pv-metadata writer)
```

---

## Examples

Now that you understand the concepts, parameters, and routing options, here are practical examples.

### Example 1 — Live PVs → MLDP (simplest setup)

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

---

## Further Reading

| Topic | Document |
|---|---|
| Full YAML schema reference | [configuration.md](configuration.md) |
| PVXS reader details | [readers/epics-pvxs-reader.md](../readers/epics-pvxs-reader.md) |
| CA reader details | [readers/epics-base-reader.md](../readers/epics-base-reader.md) |
| Archiver reader details | [readers/epics-archiver-reader.md](../readers/epics-archiver-reader.md) |
| DS metadata reader details | [readers/epics-ds-metadata-reader.md](../readers/epics-ds-metadata-reader.md) |
| MLDP writer details | [writers/mldp-writer.md](../writers/mldp-writer.md) |
| HDF5 writer details | [writers/hdf5-writer.md](../writers/hdf5-writer.md) |
| Routing and source filtering | [controller.md](../reference/controller.md#reader-to-writer-routing) |
| Architecture and internals | [architecture.md](../reference/architecture.md) |
| Metrics and monitoring | [metrics-export-guide.md](../metrics/metrics-export-guide.md) |
