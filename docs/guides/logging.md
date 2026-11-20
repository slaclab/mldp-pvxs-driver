# Logging Guide

How the driver logs: console output, per-component log files, rotation, and compression.

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Which File Gets Which Lines](#which-file-gets-which-lines)
- [File Naming](#file-naming)
- [Rotation, Compression and Retention](#rotation-compression-and-retention)
- [Examples](#examples)
- [Reading the Logs](#reading-the-logs)
- [Limitations](#limitations)
- [Implementation Notes](#implementation-notes)

---

## Overview

By default, with no `logging:` block, the driver logs to the **console only**, as it always has. File logging is opt-in. Each line can go to:

1. the **console** (on by default),
2. the **main log file** (`log-file`) with every line of the run,
3. the **component's own file**, in the same folder as `log-file`, when the line comes from a reader, writer or the controller. This is off by default; turn it on per category in `per-component`.

Files rotate by **size** and/or **age**. Rotated files are **gzip-compressed** in the background, and only the newest `max-files` are kept. With `per-run` on (the default), every run starts new files, and the previous run's files are compressed at startup.

The log level is always set by `--log-level` (`trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`). It applies to every destination.

---

## Quick Start

**One log file**, on the command line and without changing the YAML:

```bash
build/bin/mldp_pvxs_driver -c config.yaml --log-file logs/driver.log
```

or in the YAML:

```yaml
logging:
  log-file: logs/driver.log
```

Either one gives you console output plus `logs/driver.20261118-131542.log` (every line).

**One file per component** as well:

```yaml
logging:
  log-file: logs/driver.log
  per-component:
    readers: true
    writers: true
    controller: true
```

```
logs/driver.20261118-131542.log                                   # every line
logs/controller-default.20261118-131542.log
logs/reader-slac-calendar_slac_calendar_local.20261118-131542.log
logs/writer-configuration_mldp_config_main.20261118-131542.log
```

---

## Configuration

`logging:` is an optional top-level block. All keys are optional.

```yaml
logging:
  log-file: logs/driver.log    # main log file (every line); CLI --log-file overrides
  console: true                # keep console output
  per-component:               # own file per component, next to log-file (all default false)
    readers: false
    writers: false
    controller: false
  per-run: true                # run start time in file names
  rotation:
    max-size-mb: 100           # 0 = off
    max-age-hours: 0           # 0 = off
    compress: true             # gzip old files
    max-files: 10              # old files kept per log; 0 = unlimited
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `log-file` | path | — | Main log file; receives every line. Its folder is created if missing, and per-component files are written there too. A trailing `.log` is the extension; the rest of the name is the file stem. |
| `console` | bool | `true` | Also print to the console. Set `false` for background or batch runs. |
| `per-component.readers` | bool | `false` | One file per reader instance. Requires `log-file`. |
| `per-component.writers` | bool | `false` | One file per writer instance. |
| `per-component.controller` | bool | `false` | One file for the controller. |
| `per-run` | bool | `true` | Add the run start time to file names. `false` appends to the same file across runs. |
| `rotation.max-size-mb` | int ≥ 0 | `100` | Rotate before a write would make the file larger than this. `0` disables size rotation. |
| `rotation.max-age-hours` | int ≥ 0 | `0` | Rotate once the active file has been open this long. `0` disables age rotation. |
| `rotation.compress` | bool | `true` | gzip rotated files and earlier runs' files to `.log.gz`. |
| `rotation.max-files` | int ≥ 0 | `10` | Old files kept per stem (rotated + earlier runs); the oldest are deleted. The active file is not counted. `0` keeps everything. |

**When file logging is on:** only when `log-file` is set (in YAML or with `--log-file`). Otherwise the driver logs to the console only. All files, the main one and the per-component ones, go in the folder of `log-file`.

**Precedence:** `--log-file PATH` replaces `logging.log-file`. Every other key is set only in YAML, or with dotted `-c` overrides:

```bash
build/bin/mldp_pvxs_driver -c config.yaml \
  --log-file logs/driver.log \
  -c logging.per-component.readers=true \
  -c logging.rotation.max-size-mb=50 \
  -c logging.console=false
```

**Validation** fails startup, and `--dry-run` reports it too, when:
- a value is negative: `Error: logging: 'max-files' must be >= 0`;
- per-component files are on but `log-file` is not set: `logging: per-component files need 'log-file' (they are written in its folder)`;
- the old `combined-file` key is used: `logging: 'combined-file' was replaced by 'log-file: <path>'`;
- `directory` is used: `logging: 'directory' is not supported; per-component files are written next to 'log-file'`.

---

## Which File Gets Which Lines

Each component logs under a name, which is the second column of every log line. That name decides the file:

| Logger name | Category | Own file stem |
|-------------|----------|---------------|
| `controller.<name>` | controller | `controller-<name>` |
| `reader:<type>:<name>` | reader | `reader-<type>_<name>` |
| `<type>_writer:<name>` | writer | `writer-<type>_<name>` |
| `epics_pvxs_reader`, `epics_base_reader`, `hdf5_bsas_gen1_reader`, … | reader | `reader-<logger name>` (one per type) |
| `util:http:curl`, `mldp_grpc_*_pool`, `processor.*`, `mldp_pvxs_driver` | other | — (main log file only) |

Every line also goes to the console (if `console: true`) and the main log file (if `log-file` is set). A component file gets **only** that component's lines, so you can follow one reader without the rest of the noise.

Concrete names for the current components:

| Component | File stem |
|-----------|-----------|
| SLAC calendar reader `slac_calendar_local` | `reader-slac-calendar_slac_calendar_local` |
| EPICS archiver reader `arch1` | `reader-epics-archiver_arch1` |
| DS metadata reader `ds1` | `reader-epics-ds-metadata_ds1` |
| MLDP ingestion writer `mldp_main` | `writer-grpc_mldp_main` |
| MLDP configuration writer `mldp_config_main` | `writer-configuration_mldp_config_main` |
| MLDP PV metadata writer `meta` | `writer-pv_metadata_meta` |
| HDF5 writer `h5` | `writer-hdf5_h5` |
| Controller `default` | `controller-default` |

Characters other than `A-Z a-z 0-9 . _ -` become `_`.

---

## File Naming

| File | Name |
|------|------|
| Active (`per-run: true`) | `<stem>.<run-start>.log`, e.g. `log-file: logs/driver.log` → `logs/driver.20261118-131542.log` |
| Active (`per-run: false`) | `<stem>.log`, e.g. `logs/driver.log` |
| Rotated | `<active-without-.log>.<rotation-time>.log`, with `.N` added if that name exists |
| Compressed | same name + `.gz` |

Times are local time in `YYYYmmdd-HHMMSS` format. Example after two size rotations in one run:

```
reader-epics-archiver_arch1.20261118-131542.log                          # active
reader-epics-archiver_arch1.20261118-131542.20261118-150210.log.gz
reader-epics-archiver_arch1.20261118-131542.20261118-163955.log.gz
```

---

## Rotation, Compression and Retention

**When a file rotates.** Before each write, the sink checks:
- **size:** the active file is not empty and this line would push it past `max-size-mb`; or
- **age:** the active file has been open for at least `max-age-hours`.

If either is true, the active file is closed, renamed with the rotation time, and a new empty file is opened. The line goes to the new file. Lines are never split across files.

**Compression.** Rotated files are queued to one background thread that gzips them (`.log.gz`, level 6) and then deletes the original. Logging never waits for compression. On normal shutdown the driver waits until the queue is empty, so no half-compressed files are left. A file being compressed is written as `*.gz.tmp` and renamed when finished; leftover `.tmp` files are ignored.

**New run.** When a sink opens, any plain `.log` files of the same stem that are not the active file (from earlier runs or earlier crashes) are queued for compression.

**Retention.** After each compression job, files named `<stem>.*.log` / `<stem>.*.log.gz` other than the active file are sorted by modification time, and the oldest ones beyond `max-files` are deleted. Retention counts **across runs**, so `max-files: 10` keeps the 10 most recent old files of that log, whichever runs they came from.

**Disk use bound per log** (with size rotation): about `max-size-mb × (1 + max-files × gzip-ratio)`. Text logs usually compress 10–20×.

**Flushing.** Files are flushed every 2 seconds and on shutdown. If the process is killed (`SIGKILL`), the last ~2 seconds may be missing.

---

## Examples

### Long backfill, keep everything for a while

```yaml
logging:
  log-file: /data/logs/calendar-backfill/driver.log
  per-component:
    readers: true
    writers: true
  rotation:
    max-size-mb: 200
    max-files: 50
```

### Service running for days: daily files, two weeks kept

```yaml
logging:
  log-file: /var/log/mldp-driver/driver.log
  console: false
  rotation:
    max-size-mb: 500        # safety cap on busy days
    max-age-hours: 24       # new file every day
    max-files: 14
```

### Main file plus per-reader files only

```yaml
logging:
  log-file: logs/driver.log
  per-component:
    readers: true
```

`logs/` then holds `driver.….log` (every line) and one `reader-….log` per reader. Writers and the controller have no file of their own; their lines are in `driver.….log`.

### Same file names across runs (append, no run timestamp)

```yaml
logging:
  log-file: logs/driver.log
  per-run: false
```

---

## Reading the Logs

Line format (same as the console, without colors):

```
[2026-11-18 13:21:58.888] [configuration_writer:mldp_config] [error   ] MLDPConfigurationWriter saveConfigurationActivation ...
 timestamp                 logger name (32 chars)             level      message
```

Useful commands:

```bash
# errors from one writer, current run
grep -E '\[error' logs/writer-configuration_mldp_config_main.*.log

# search compressed history too
zgrep -h 'overlapping activation' logs/writer-configuration_*.log*

# follow one reader live
tail -F logs/reader-slac-calendar_slac_calendar_local.*.log

# whole run, all components, in time order
less logs/driver.20261118-131542.log
```

---

## Limitations

- **Lines before the config is loaded** (the version banner and `Loading configuration from …`) go to the console only, because file logging starts after the YAML is read.
- **Readers without an instance name** (`epics-pvxs`, `epics-base`, HDF5 BSAS readers) log under a fixed type name, so all instances of that type share one file.
- **Shared helpers** (HTTP client, gRPC pools, processors) have no owning component and go to the main log file only.
- **One folder:** every file goes in the folder of `log-file`; component files cannot go elsewhere.
- **Time-based rotation only happens on write**: a quiet log rotates when its next line arrives, not exactly at the boundary.
- **`--log-level` applies to every destination**; there is no separate level per file.
- `config wizard` / `config template` do not generate the `logging:` block yet; add it by hand.

---

## Implementation Notes

For developers.

| File | Role |
|------|------|
| `include/cli/LoggingConfig.h`, `src/cli/LoggingConfig.cpp` | `LoggingConfig::parse()`: YAML → settings, validation. |
| `include/cli/RotatingGzipFileSink.h`, `src/cli/RotatingGzipFileSink.cpp` | `RotatingGzipFileSink` (spdlog sink: size/age rotation) and `LogCompressor` (background gzip + retention). |
| `include/cli/FileLogging.h`, `src/cli/FileLogging.cpp` | `classifyLogger()` (name → file) and `installFileLogging()`, which swaps the default logger and installs the logger factory. |
| `src/cli/mldp_pvxs_driver_main.cpp` | `--log-file`, parse at startup, install before the controller starts, stop the compressor after `spdlog::shutdown()`. |
| `test/cli/file_logging_test.cpp` | Routing, config parsing, size/age rotation, gzip content, retention, per-run naming. |

- Library code is unchanged. Components keep calling `newLogger("<name>")`. Routing happens in the CLI's `setLoggerFactory`, so a new reader or writer that follows the existing naming (`reader:<type>:<name>`, `<type>_writer:<name>`) gets its own file automatically.
- Sinks are shared: one main-file sink for all loggers, and one component sink per file stem, cached so loggers with the same name share it. All sinks are thread-safe (`base_sink<std::mutex>`).
- Size is tracked in the sink, including bytes not yet flushed (`file_helper::size()` only sees flushed data).
- gzip uses zlib (`find_package(ZLIB REQUIRED)` in `CMakeLists.txt`).
