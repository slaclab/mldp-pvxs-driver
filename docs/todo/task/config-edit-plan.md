# Config Edit Sub-commands — Design Plan

> **Status**: Draft  
> **Scope**: Three new `config` sub-commands for non-interactive file editing: `list`, `add`, `remove`

---

## Problem Statement

`config wizard` generates a full config from scratch.  
Operators also need to **inspect** and **mutate** an existing file without re-running the wizard or hand-editing YAML:

- List what is currently configured (quick audit, CI health check)
- Add a new reader/writer/routing entry to a live config
- Remove a named entry (decommission a PV source, swap a writer)

These operations must be scriptable (CI pipelines, Ansible playbooks) and produce a validated result.

---

## New CLI Surface

```
mldp_pvxs_driver config list   PATH
mldp_pvxs_driver config add    PATH (reader|writer|routing) [flags…]
mldp_pvxs_driver config remove PATH (reader|writer|routing) --name NAME
```

All three take `PATH` as the first positional argument (the YAML file to inspect or mutate).  
`PATH` defaults to `config.yaml` when omitted.

### Quick reference

| Command | Mutates file? | FTXUI needed? |
|---------|:---:|:---:|
| `config list` | No | No |
| `config add … --name … --type …` (non-interactive) | Yes | No |
| `config add …` (interactive, no `--name`) | Yes | Yes (`MLDP_WIZARD_ENABLED`) |
| `config remove … --name NAME` | Yes | No |

---

## Design Decisions

### YAML serialization strategy

**Decision: Option A — round-trip via `WizardState`**

Load file → `wizard_internal::loadFromConfig` → mutate `WizardState` → write back via `wizard_internal::generateYaml`.

| Criterion | Option A (WizardState round-trip) | Option B (ryml in-place tree mutation) |
|-----------|-----------------------------------|----------------------------------------|
| Implementation cost | Low — reuses tested path | High — ryml mutation API non-trivial |
| Comment preservation | None — comments lost on write | Preserved in untouched blocks |
| Ordering preserved | Writer/reader insertion order | Yes |
| Test coverage | Full — `generateYaml` already tested | New path needs own tests |
| Risk | Low | Medium |

**Accepted trade-off**: operators lose inline YAML comments on `add`/`remove`. Acceptable given that:
1. `config wizard` already owns the canonical serialization format
2. Operators using `add`/`remove` are scripting — they do not hand-annotate files
3. A `config validate` + diff workflow catches regressions

**`list` never writes** — no trade-off there.

### Backup before write

Write `<path>.bak` before overwriting on any mutation. Silent by default; `--no-backup` skips it.

### Routing `add` conflict

If `routing add` is called for a writer that already has an explicit entry: **merge** (append `from`/`include`/`exclude` to existing entry), not overwrite. Overwrite requires explicit `--replace` flag.

### Dry-run integration

All mutating commands accept `--dry-run`: print the resulting YAML to stdout without writing the file.

---

## Sub-command: `config list`

### Arguments

```
mldp_pvxs_driver config list [PATH]
```

### Output format

```
File: config.yaml

Writers
  mldp_main    mldp        thread-pool=2  ingestion-url=grpc://mldp-ingest.example.com:50051
  hdf5_local   hdf5        base-path=/data/hdf5  compression=1

Readers
  pvxs_main    epics-pvxs  3 PVs   thread-pool=4
  arch_tail    epics-archiver  0 PVs   mode=periodic_tail  poll-interval=10s

Routing      all-to-all
  -- or, for explicit routing: --
Routing
  mldp_main  ← pvxs_main  [include: SITE:BPM:*]  [exclude: SITE:BPM:OLD:*]
  hdf5_local ← all

Metrics      0.0.0.0:9464  scan-interval=5s
  -- or --
Metrics      disabled
```

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | File valid, table printed |
| 1 | File cannot be opened or parsed |

### Implementation notes

- Call `loadFromConfig` → walk `WizardState` fields, format with `std::left` / `std::setw` for alignment
- No `validateConfig` call — `list` is diagnostic, must work even on partially-invalid files
- Add `--json` flag (Phase 2 Polish) for machine-readable output; not in Phase 0

---

## Sub-command: `config remove`

### Arguments

```
mldp_pvxs_driver config remove PATH (reader|writer|routing) --name NAME [--no-backup] [--dry-run]
```

`--name` required. `kind` positional required.

### Behavior

1. Load `PATH` → `WizardState`
2. Locate entry by `kind` + `name`:
   - `writer` → search `mldp_writers` (by `name`) and `hdf5_writers` (by `name`)
   - `reader` → search `readers` (by `name`)
   - `routing` → search `routing` (by `writer_name`)
3. Guard checks (error + exit 1, no write):
   - Name not found
   - `writer` removal would leave zero writers (invalid config)
4. Remove entry
5. Side-effect cleanup:
   - `writer` removed → remove any `routing` entry with matching `writer_name`
   - `reader` removed → remove that name from all `routing[*].from_readers`
   - `routing` removed → no side effects
6. Run `validateConfig` on result; print warnings but do not block write on warnings-only
7. Write `<path>.bak` (unless `--no-backup`)
8. Write mutated YAML to `PATH`
9. Print:
   ```
   Removed writer 'mldp_main' from config.yaml
   Backup: config.yaml.bak
   ```

### Edge cases

| Scenario | Behavior |
|----------|----------|
| Name not found | Exit 1: `ERROR  writer 'foo' not found in config.yaml` |
| Last writer removed | Exit 1: `ERROR  cannot remove last writer — config would be invalid` |
| Reader removed, routing dangling | Auto-clean `from_readers` list; warn if routing entry becomes empty |
| `routing` kind + writer name not in routing block | Exit 1: `ERROR  no routing entry for writer 'foo'` |
| `--dry-run` | Print resulting YAML, no write, no backup |

---

## Sub-command: `config add`

### Arguments

```
mldp_pvxs_driver config add PATH (reader|writer|routing) [--name NAME] [--type TYPE] [type-flags…]
                             [--no-backup] [--dry-run]
```

When `--name` is **omitted** → interactive FTXUI sub-flow (requires `MLDP_WIZARD_ENABLED`).  
When `--name` is **provided** → non-interactive, all fields from flags or defaults.

#### Writer flags (`--type mldp|hdf5|hdf5-merge`)

| Flag | Default | Notes |
|------|---------|-------|
| `--name NAME` | required | Must be unique across all writers |
| `--type TYPE` | required | `mldp`, `hdf5`, `hdf5-merge` |
| `--thread-pool N` | 1 (mldp) / n/a (hdf5) | |
| `--ingestion-url URL` | required for mldp | |
| `--provider-name NAME` | required for mldp | |
| `--query-url URL` | optional | |
| `--min-conn N` | 1 | |
| `--max-conn N` | 4 | |
| `--credentials TYPE` | ssl | `none`, `ssl`, `custom-tls` |
| `--base-path PATH` | required for hdf5/hdf5-merge | |
| `--compression-level N` | 0 | 0–9 |
| `--max-file-age-s N` | 3600 | |
| `--max-file-size-mb N` | 512 | |

#### Reader flags (`--type epics-pvxs|epics-base|epics-archiver`)

| Flag | Default | Notes |
|------|---------|-------|
| `--name NAME` | required | Must be unique across all readers |
| `--type TYPE` | required | `epics-pvxs`, `epics-base`, `epics-archiver` |
| `--thread-pool N` | 2 | |
| `--column-batch-size N` | 50 | |
| `--pvs PV1,PV2,…` | optional | Comma-separated; option=none for all |
| `--hostname HOST:PORT` | required for archiver | |
| `--mode MODE` | historical_once | archiver only |
| `--start-date DATE` | required for historical_once | ISO 8601 |
| `--end-date DATE` | optional | ISO 8601 |
| `--poll-interval-sec N` | required for periodic_tail | |
| `--connect-timeout-sec N` | 30 | |
| `--total-timeout-sec N` | 300 | |

#### Routing flags

| Flag | Default | Notes |
|------|---------|-------|
| `--writer NAME` | required | Must match existing writer |
| `--from R1,R2,…` | optional | Comma-separated reader names or `all` |
| `--include GLOB` | optional | Repeatable |
| `--exclude GLOB` | optional | Repeatable |
| `--replace` | false | Overwrite existing entry instead of merging |

### Non-interactive behavior

1. Load `PATH` → `WizardState`
2. Validate `--name` unique, `--type` valid, required flags present
3. Run field-level validators (same `wizard_internal::isPositiveInt` etc.)
4. Build struct from flags + defaults
5. Append to `WizardState`
6. Run `validateConfig` — exit 1 if errors, print diagnostics
7. Write backup + mutated YAML
8. Print:
   ```
   Added reader 'pvxs_extra' (epics-pvxs) to config.yaml
   Backup: config.yaml.bak
   ```

### Interactive behavior (FTXUI, `--name` omitted)

1. Load `PATH` → `WizardState` (pre-loaded existing entries for uniqueness checks)
2. Dispatch to wizard sub-flow:
   - `add writer` → `phase2_add_one_writer(st)` (single iteration of phase 2 loop)
   - `add reader` → `phase3_add_one_reader(st)` (single iteration of phase 3 loop)
   - `add routing` → `phase5_add_one_routing_entry(st)` (routing config for one writer)
3. Append result to `WizardState`
4. `validateConfig` — display inline, confirm save
5. Write backup + YAML

**Implementation note**: wizard phase loops (`phase2_writers`, `phase3_readers`, `phase5_routing`) must be refactored to expose single-entry variants callable by `add` sub-flow. This is a clean extraction — the loop body becomes a standalone function; the phase function becomes a thin wrapper that loops over it.

---

## Implementation Plan

### Phase 0 — Foundation (no UI, risk-free)

**0.1** Add `list`, `add`, `remove` subparsers to `src/config/subcommand.cpp`  
**0.2** Create `include/config/edit.h` — declare `runList`, `runAdd`, `runRemove`  
**0.3** Create `src/config/edit.cpp` — stub implementations (print "not yet implemented")  
**0.4** Add `edit.cpp` to CMake `${PROJECT_NAME}` executable sources (no guard needed — no FTXUI)  
**0.5** `config list` fully implemented  
**0.6** `config remove` fully implemented  

Deliverable: `list` and `remove` working, tested, validated against `docs/examples/`.

---

### Phase 1 — Non-interactive `config add`

**1.1** Parse all `--type`-specific flags in `subcommand.cpp`; pass to `runAdd`  
**1.2** Implement `runAdd` non-interactive path in `edit.cpp`:
- Build `MldpWriterConfig` / `Hdf5WriterConfig` / `EpicsReaderConfig` / `RoutingEntry` from flags
- Validation using `wizard_internal::isPositiveInt`, `isValidIso8601`, etc.
- Uniqueness check against loaded state
- `validateConfig` gate before write  

**1.3** `--pvs` flag: comma-split, build `PvEntry` list (option=none for all)  
**1.4** `--from` flag for routing: comma-split, validate names exist  
**1.5** `--replace` flag for routing conflict resolution  

Deliverable: fully scriptable `add` path. No FTXUI dependency anywhere in `edit.cpp`.

---

### Phase 2 — Interactive `config add` (FTXUI)

**2.1** Refactor `src/config/wizard.cpp` phase functions:
- Extract `phase2_add_one_writer(WizardState&)` from `phase2_writers` loop body
- Extract `phase3_add_one_reader(WizardState&)` from `phase3_readers` loop body
- Extract `phase5_add_one_routing_entry(WizardState&, const std::string& writer_name)` from `phase5_routing`
- Expose in `wizard_internal.h`

**2.2** Create `src/config/edit_interactive.cpp` (gated `#ifdef MLDP_WIZARD_ENABLED`):
- `runAddInteractive(PATH, kind)` — loads state, dispatches to phase sub-flow, saves

**2.3** Wire in `subcommand.cpp`: when `--name` omitted and `MLDP_WIZARD_ENABLED` → call interactive path; else error "use --name for non-interactive add"

---

### Phase 3 — Tests

New file: `test/config/edit_test.cpp`

**List tests**

| Test | Checks |
|------|--------|
| `ListPrintsWriterNames` | stdout contains writer name for each writer |
| `ListPrintsReaderNames` | stdout contains reader name + type + PV count |
| `ListPrintsRoutingAllToAll` | shows `all-to-all` when no routing block |
| `ListPrintsExplicitRouting` | shows writer→reader mapping |
| `ListPrintsMetrics` | shows endpoint when enabled |
| `ListMetricsDisabled` | shows `disabled` |
| `ListInvalidFileExits1` | non-existent file → exit 1 |

**Remove tests**

| Test | Checks |
|------|--------|
| `RemoveWriterByName` | writer gone, YAML re-validates |
| `RemoveWriterCleansRouting` | routing entry for that writer also removed |
| `RemoveReaderByName` | reader gone, validates |
| `RemoveReaderCleansFromLists` | reader name purged from all `routing[*].from_readers` |
| `RemoveRoutingByName` | routing entry removed, writers/readers untouched |
| `RemoveLastWriterFails` | exit 1, file unchanged |
| `RemoveUnknownNameFails` | exit 1, file unchanged |
| `RemoveDryRun` | prints YAML, file unchanged |
| `RemoveWritesBackup` | `.bak` file created |
| `RemoveNoBackup` | `--no-backup` skips `.bak` |

**Add non-interactive tests**

| Test | Checks |
|------|--------|
| `AddMldpWriterNonInteractive` | writer present in output, validates |
| `AddHdf5WriterNonInteractive` | hdf5 writer present, base-path correct |
| `AddEpicsPvxsReaderNonInteractive` | reader present, 0 PVs |
| `AddReaderWithPvList` | `--pvs A,B,C` → 3 PVs in output |
| `AddArchiverHistoricalOnce` | mode + start-date emitted, validates |
| `AddArchiverPeriodicTail` | mode + poll-interval emitted |
| `AddRoutingNonInteractive` | routing entry present with correct from/include |
| `AddRoutingMerge` | existing entry has new reader appended |
| `AddRoutingReplace` | `--replace` overwrites existing entry |
| `AddDuplicateNameFails` | exit 1, file unchanged |
| `AddMissingRequiredFlagFails` | exit 1 with field name in message |
| `AddValidationFailureFails` | exit 1 when result fails `validateConfig` |
| `AddDryRun` | prints YAML, file unchanged |

---

## File Layout Delta

```
include/config/
  edit.h                        ← runList, runAdd, runRemove

src/config/
  edit.cpp                      ← list, remove, add-non-interactive (no FTXUI)
  edit_interactive.cpp          ← add-interactive FTXUI sub-flows (#ifdef MLDP_WIZARD_ENABLED)

test/config/
  edit_test.cpp                 ← all edit tests (no FTXUI needed)
```

`wizard.cpp` refactored to expose single-entry add functions via `wizard_internal.h` (Phase 2).

---

## CMake Changes

```cmake
# edit.cpp — always compiled into main executable
"${CMAKE_CURRENT_SOURCE_DIR}/src/config/edit.cpp"

# edit_interactive.cpp — compiled only when MLDP_WIZARD=ON
if(MLDP_WIZARD AND MLDP_PVXS_DRIVER_MAIN)
    target_sources(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/config/edit_interactive.cpp"
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE MLDP_WIZARD_ENABLED)
endif()

# edit_test target — mirrors mldp_pvxs_driver_wizard_test pattern
add_executable(mldp_pvxs_driver_edit_test
    "${CMAKE_CURRENT_SOURCE_DIR}/test/config/edit_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config/validate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config/edit.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/ryml_single_header_translation_unit.cpp"
)
target_link_libraries(mldp_pvxs_driver_edit_test
    PRIVATE gtest_main mldp_pvxs_driver_wizard_lib
    -Wl,--whole-archive lib${PROJECT_NAME} -Wl,--no-whole-archive)
gtest_discover_tests(mldp_pvxs_driver_edit_test)
```

---

## Acceptance Criteria

| Criterion | How verified |
|-----------|-------------|
| `list` shows all writers, readers, routing, metrics | Unit + manual |
| `list` exits 0 on all `docs/examples/*.yaml` | CI |
| `remove` by name leaves valid config | Unit: `validateConfig` on result |
| `remove` last writer exits 1, no write | Unit |
| `remove` cleans routing side-effects | Unit |
| `add` non-interactive produces valid config | Unit: round-trip |
| `add` duplicate name exits 1 | Unit |
| `add` missing required flag exits 1 with field name | Unit |
| backup `.bak` written before mutation | Unit: file exists check |
| `--dry-run` never writes file | Unit: mtime/existence check |
| `add` interactive FTXUI runs without crash | Manual: `ssh host config add … reader` |
| All 252 existing tests still pass | CI |

---

## Open Questions

1. **Comment preservation**: accept loss (Option A) or invest in ryml tree mutation (Option B) for `add`/`remove`?
2. **`add routing` with no existing routing block**: auto-create `routing:` section, switching from implicit all-to-all to explicit. Must warn operator that unlisted writers now receive nothing.
3. **`--pvs` for `add reader`**: comma-separated string or `--pv NAME` repeatable flag? (Repeatable is more shell-friendly for long lists.)
4. **`list --json`**: defer to Phase 2 Polish or include in Phase 0?
5. **Atomic write**: write to `<path>.tmp` then `rename(2)` to avoid partial writes on crash?

---

## References

- `src/config/wizard.cpp` — `loadFromConfig`, `generateYaml`, phase functions
- `src/config/wizard_internal.h` — internal API exposed for tests
- `src/config/validate.cpp` — `validateConfig` used as write gate
- `src/config/subcommand.cpp` — dispatcher to extend
- `docs/configuration.md` — schema reference
- `docs/todo/task/config-wizard-plan.md` — wizard design (parent plan)
