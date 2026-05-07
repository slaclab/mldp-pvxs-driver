# Configuration Wizard — Design Plan

> **Status**: Draft  
> **Scope**: Interactive CLI wizard for generating valid `config.yaml` for `mldp_pvxs_driver`

---

## Problem Statement

The YAML configuration has significant complexity:

- Two nesting levels deep (e.g. `writer.mldp[].mldp-pool.credentials`)
- Conditional required fields (archiver `start-date` only required for `historical_once` mode)
- Two polymorphic forms for PV `option` (scalar string vs. structured map)
- Three credential forms for `mldp-pool` (`none` / `ssl` / custom TLS map)
- Cross-referencing: `routing` names must match declared writer/reader instance names
- Multiple instances of each type possible
- Some readers generate sources at runtime and carry no `pvs:` list — wizard must not require one

First-time operators (physicists, SRE) must manually consult docs to produce a valid file.

---

## Goals

1. Produce a valid, runnable `config.yaml` without reading docs
2. Validate cross-references (routing → writer/reader names)
3. Validate conditional constraints (mode-dependent required fields)
4. Support iterative editing: run wizard on existing file to amend it
5. Integrate with existing `--dry-run` flag for post-wizard validation

---

## Non-Goals

- Web UI (adds browser/server dependency; not worth it for a server-side driver)
- ncurses: system dep, breaks over some SSH/terminal configs
- Wizard replaces `--dry-run`: wizard complements it, does not replace

---

## Chosen Approach: Hybrid Wizard + Validator

Three new sub-commands under a `config` top-level command:

```
mldp_pvxs_driver config wizard   [--output PATH] [--from PATH]
mldp_pvxs_driver config validate PATH
mldp_pvxs_driver config template [--full | --minimal] [--output PATH]
```

The wizard is the primary deliverable. Validate and template are lower-effort utilities with high standalone value.

---

## Implementation Language: C++ (not Python)

**Rationale:**

1. **Always in sync**: wizard is compiled into the binary — config schema changes in the same PR as wizard changes. No drift between wizard script version and binary version.
2. **No runtime dep**: Python not guaranteed in all deployment targets (AppImage, standalone artifact, bare-metal hosts). C++ binary is self-contained.
3. **Single artifact**: operators run one binary, not `binary + script`. Simpler distribution, simpler docs.
4. **Validation reuse**: `validateConfig()` (Phase 0) is C++ — wizard can call it directly without subprocess roundtrip.

**Trade-off accepted**: schema changes require recompile (same as everything else in the driver — not a new burden).

---

## UI Library: FTXUI

**Chosen**: [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v5.x  
**License**: MIT  
**Integration**: `FetchContent` — no system install, no new build-time dependency beyond CMake

**Rationale over plain `<iostream>`:**
- Multi-select checkbox list for PV entry and routing reader selection — no index-typing errors
- Inline validation feedback rendered in the same screen region (red border on invalid input)
- Navigable menus for writer/reader type selection — arrow keys, no mistype
- Still pure ANSI sequences — works over SSH, no ncurses dep
- Single `FetchContent_Declare` block in `CMakeLists.txt`; FTXUI is header+source, ships in repo cache

**Where FTXUI is used vs plain prompts:**

| Step | UI |
|------|----|
| Writer / reader type selection | FTXUI `Menu` (arrow-key navigable) |
| PV list entry | FTXUI `Input` + `Button("Add PV")` loop |
| Routing reader selection | FTXUI `CheckboxList` (multi-select) |
| Glob pattern entry | FTXUI `Input` with live prefix-suggestion rendering |
| Bool questions (y/N) | FTXUI `Toggle` or plain prompt — either works |
| Scalar fields (name, URL, timeout) | FTXUI `Input` with validator callback |
| Phase progress header | FTXUI `Text` banner — "Phase 2 of 6 — Writers" |
| Phase 6 summary | FTXUI `Renderer` — formatted table before save confirmation |

**CMake integration:**

```cmake
include(FetchContent)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG        v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# Wizard target only — does not pollute driver runtime binary
target_link_libraries(mldp_pvxs_driver_wizard
  PRIVATE ftxui::screen ftxui::dom ftxui::component
)
```

> **Note**: wizard can be a separate CMake target (`mldp_pvxs_driver_wizard`) linked into the main binary via `config wizard` subcommand dispatch, OR compiled into the same binary with FTXUI as an optional dep (`-DMLDP_WIZARD=ON`, default ON). Either way the driver runtime itself does not link FTXUI.

---

## Sub-command: `config wizard`

### Interaction Model

FTXUI component-based TUI. Pure ANSI sequences — **no ncurses**, works over SSH.  
Each wizard phase renders a full-screen component; user navigates with arrow keys, Tab, Enter.  
Phases are sequential — no back-navigation (keep state machine simple).

Prompt engine: `src/config/wizard_ui.hpp` — thin wrappers around FTXUI `Input`, `Menu`, `CheckboxList`, `Renderer`.

### Wizard Flow (Ordered)

**Key principle**: Phases 2 and 3 are open loops — the user may add any number of instances of any type, including multiple instances of the same type (e.g. two `mldp` writers with different endpoints, three `epics-pvxs` readers for different PV groups). The wizard tracks all declared instances in memory for use in Phase 5.

```
Phase 1 — Controller
  → controller name (optional, default: "default")

Phase 2 — Writers (outer loop: repeat until user stops)
  ┌─ add a writer instance ─────────────────────────────────────────┐
  │ → writer type: [mldp | hdf5 | hdf5-merge]                      │
  │   (FTXUI Menu — arrow keys)                                      │
  │                                                                  │
  │ IF mldp:                                                         │
  │   → instance name  [validated: unique across ALL writers]        │
  │   → thread-pool (default: 1)                                     │
  │   → stream-max-bytes (default: 2097152)                          │
  │   → stream-max-age-ms (default: 200)                             │
  │   → mldp-pool.provider-name                                      │
  │   → mldp-pool.provider-description (opt)                         │
  │   → mldp-pool.ingestion-url                                      │
  │   → mldp-pool.query-url (opt)                                    │
  │   → mldp-pool.min-conn (default: 1)                              │
  │   → mldp-pool.max-conn (default: 4)                              │
  │   → credentials type: [none | ssl | custom-tls]                  │
  │     IF custom-tls:                                               │
  │       → pem-cert-chain path (opt)                                │
  │       → pem-private-key path (opt)                               │
  │       → pem-root-certs path (opt)                                │
  │                                                                  │
  │ IF hdf5 | hdf5-merge:                                            │
  │   → instance name  [validated: unique across ALL writers]        │
  │   → base-path                                                    │
  │   → max-file-age-s (default: 3600)                               │
  │   → max-file-size-mb (default: 512)                              │
  │   → flush-interval-ms (default: 1000)                            │
  │   → compression-level 0–9 (default: 0)                           │
  │                                                                  │
  │ → [Added: <type> "<name>"]  ← confirm echo after each instance  │
  └──────────────────────────────────────────────────────────────────┘
  → Add another writer? [y/N]
  [guard: at least one writer must exist before proceeding]

Phase 3 — Readers (outer loop: repeat until user stops)
  ┌─ add a reader instance ─────────────────────────────────────────┐
  │ → reader type: [epics-pvxs | epics-base | epics-archiver]       │
  │   (FTXUI Menu — arrow keys)                                      │
  │                                                                  │
  │ IF epics-pvxs | epics-base:                                      │
  │   → instance name  [validated: unique across ALL readers]        │
  │   → thread-pool (default: 2)                                     │
  │   → column-batch-size (default: 50)                              │
  │   IF epics-base:                                                 │
  │     → monitor-poll-threads (default: 2)                          │
  │     → monitor-poll-interval-ms (default: 5)                      │
  │   → Add PVs? [Y/n]   ← always optional; some deployments omit   │
  │     IF yes → PVs (inner loop):                                   │
  │       → PV name                                                  │
  │       → option type: [none | scalar | slac-bsas-table]           │
  │         IF scalar: → option value string                         │
  │         IF slac-bsas-table:                                      │
  │           → tsSeconds field name                                 │
  │           → tsNanos field name                                   │
  │       → Add another PV? [y/N]                                    │
  │                                                                  │
  │ IF epics-archiver:                                               │
  │   → instance name  [validated: unique across ALL readers]        │
  │   → hostname (host:port)                                         │
  │   → mode: [historical_once | periodic_tail]                      │
  │   IF historical_once:                                            │
  │     → start-date (ISO 8601, required)                            │
  │     → end-date (ISO 8601, optional)                              │
  │   IF periodic_tail:                                              │
  │     → poll-interval-sec (required)                               │
  │     → lookback-sec (default: poll-interval-sec)                  │
  │   → connect-timeout-sec (default: 30)                            │
  │   → total-timeout-sec (default: 300)                             │
  │   → batch-duration-sec (default: 1)                              │
  │   → tls-verify-peer (default: true)                              │
  │   → tls-verify-host (default: true)                              │
  │   → Add PVs? [Y/n]   ← always optional                          │
  │     IF yes → PVs (inner loop): name only, no option              │
  │                                                                  │
  │ → [Added: <type> "<name>" — N PVs]  ← "0 PVs" if none declared │
  └──────────────────────────────────────────────────────────────────┘
  → Add another reader? [y/N]

Phase 4 — Metrics (optional)
  → Enable Prometheus endpoint? [y/N]
  IF yes:
    → endpoint bind address (default: 0.0.0.0:9464)
    → scan-interval-seconds (default: 1)

Phase 5 — Routing
  → Routing mode: [all-to-all (default) | explicit]
  IF explicit:
    For each writer instance (in order declared in Phase 2):
    ┌─ configure routing for writer "<name>" (<type>) ───────────────┐
    │                                                                 │
    │ Wizard displays the full reader list collected in Phase 3:      │
    │ (FTXUI CheckboxList — Space to toggle, Enter to confirm)        │
    │   [ ] pvxs_reader_a   (epics-pvxs,   12 PVs)                  │
    │   [ ] base_reader_b   (epics-base,    5 PVs)                  │
    │   [ ] archiver_tail   (epics-archiver, 0 PVs — runtime)        │
    │                                                                 │
    │ → Select readers to route to this writer                        │
    │   (Space = toggle, Enter = confirm selection)                   │
    │                                                                 │
    │ IF writer type == mldp:                                         │
    │   [skip include/exclude — mldp ingests all, no PV filtering]   │
    │                                                                 │
    │ IF writer type == hdf5 | hdf5-merge:                            │
    │   → Add include glob filters? [y/N]                             │
    │                                                                 │
    │   For each selected reader that has declared PVs:               │
    │     Extract common prefixes from PV names                       │
    │     "Detected prefixes from <name>: SITE:BPM:, SITE:TEST:"     │
    │     "Suggested: SITE:BPM:*  — use? [Y/n]"                      │
    │   For each selected reader with 0 declared PVs:                 │
    │     "ℹ <name> has no declared PVs."                            │
    │     "  Enter glob manually, or leave empty to accept all."      │
    │   → Enter additional/custom glob pattern? (empty = done): _     │
    │   → Add another include pattern? [y/N]                          │
    │   → Add exclude glob filters? [y/N]  (same logic)               │
    │                                                                 │
    └─────────────────────────────────────────────────────────────────┘

Phase 6 — Review & Save
  → Print summary table:
    Writers : mldp_main (mldp), hdf5_local (hdf5)
    Readers : pvxs_reader_a (epics-pvxs, 12 PVs), archiver_tail (epics-archiver, 0 PVs)
    Routing : mldp_main ← all | hdf5_local ← pvxs_reader_a [include: SITE:BPM:*]
    Metrics : 0.0.0.0:9464
  → Print full generated YAML to stdout
  → Save to file? [Y/n]  output path (default: config.yaml)
  → Run --dry-run validation now? [Y/n]
```

### Validation During Wizard

| Check | When |
|-------|------|
| Instance name uniqueness (writers pool) | Each writer name entry |
| Instance name uniqueness (readers pool) | Each reader name entry |
| Non-empty name | Every name entry |
| Port in hostname for archiver | After hostname entry |
| ISO 8601 format for dates | After date entry |
| `start-date` required for `historical_once` | Before leaving archiver config |
| `poll-interval-sec` required for `periodic_tail` | Before leaving archiver config |
| `total-timeout-sec >= connect-timeout-sec` | After both timeout entries |
| `compression-level` in 0–9 | After entry |
| At least one writer before Phase 3 | End of Phase 2 loop guard |
| Reader index valid in routing selection | Phase 5 each selection |
| Glob pattern non-empty | Each include/exclude entry |
| hdf5 writer + reader with 0 PVs + no globs | Phase 5 info note (not error — valid to accept all) |

### `--from PATH` (Amend mode)

Load existing YAML → pre-fill all prompts with current values.  
User can accept (Enter) or override each.  
Useful for: adding a PV, changing a timeout, adding a second writer.

---

## Sub-command: `config validate`

```
mldp_pvxs_driver config validate config.yaml
```

- Parse YAML (reuse driver's ryml parser)
- Run all semantic checks (same list as wizard inline checks above)
- Report: file, line number where known, field path, error message
- Exit code 0 = valid, 1 = invalid

**Output format:**

```
ERROR  writer.mldp[0].mldp-pool.ingestion-url  missing required field
ERROR  reader[1].epics-archiver[0].start-date  required for mode=historical_once
WARN   routing.hdf5_local.from[0]              "unknown_reader" not in declared readers
OK     config.yaml — 2 errors, 1 warning
```

Implementation: standalone C++ function `validateConfig(ryml::Tree)` returning list of diagnostics.  
Call from both `config validate` subcommand and existing startup path (replace/augment current runtime throw).

---

## Sub-command: `config template`

```
mldp_pvxs_driver config template --full    > config-full.yaml
mldp_pvxs_driver config template --minimal > config-minimal.yaml
```

- `--full`: every key present, all defaults explicit, inline comments explaining each field
- `--minimal`: only required fields + one example PV per reader type

These are static strings in C++ (or generated from a template file).  
They already partially exist as `docs/examples/` YAML files — surface them via CLI so users don't need docs site access.

---

## Implementation Plan

### Phase 0 — Foundation (no new features, risk-free)

**0.1** Add `config` subcommand routing to `main.cpp` / argparse setup  
**0.2** Wire `config template --minimal` (output existing `docs/examples/config-mldp-only.yaml`)  
**0.3** Wire `config template --full` (output `docs/examples/config-mldp-and-hdf5.yaml`)  
**0.4** Extract `validateConfig()` function from existing startup throw logic  
**0.5** Wire `config validate PATH` using `validateConfig()`  
**0.6** Add FTXUI via `FetchContent` in `CMakeLists.txt`; gate behind `MLDP_WIZARD=ON` (default ON); verify build passes

Deliverable: `config validate` and `config template` working. FTXUI vendored. Zero wizard UI code yet.

---

### Phase 1 — Wizard Core (C++ + FTXUI)

**1.1** `src/config/wizard_ui.hpp` — FTXUI component wrappers
```cpp
// Wrappers around ftxui::Input, Menu, CheckboxList
ftxui::Component InputField(const std::string& label, std::string* value,
                             std::function<std::string(const std::string&)> validator = {});
ftxui::Component TypeMenu(const std::vector<std::string>& choices, int* selected);
ftxui::Component MultiSelectList(const std::vector<std::string>& items,
                                  std::vector<bool>* selected);
ftxui::Component PhaseHeader(const std::string& title, int phase, int total);
```

**1.2** `src/config/wizard.cpp` — wizard phases wired to FTXUI components  
- Phase 1: controller name (`InputField`)  
- Phase 2: writer outer loop — `TypeMenu` for type; `InputField` per field; loop until user stops  
- Phase 3: reader outer loop — same pattern; PV loop uses `InputField` + Add button  
- Phase 4: metrics toggle + endpoint `InputField`  
- Phase 5: routing — `MultiSelectList` for reader selection; glob suggestions rendered inline; `InputField` for custom patterns  
- Phase 6: `Renderer` summary table + save confirmation + dry-run invoke  

**1.3** `--from PATH` amend mode — parse existing YAML with ryml, pre-populate all `InputField` default values

---

### Phase 2 — Polish

- Color scheme: error fields red border, valid fields green, optional fields grey
- Progress bar / phase indicator rendered in header every phase
- `--non-interactive` mode: wizard reads answers from a response file (for CI/scripted use); bypasses FTXUI, uses plain stdin
- Shell completion for `config` subcommand

---

## File Layout

```
src/
  config/
    wizard_ui.hpp         ← FTXUI component wrappers (Phase 1.1)
    wizard.cpp            ← wizard phases (Phase 1.2)
    wizard.hpp
    validate.hpp          ← validateConfig() declaration
    validate.cpp          ← validateConfig() implementation (Phase 0.4)
    template.cpp          ← template string constants (Phase 0.3)
    subcommand.cpp        ← "config" subcommand dispatch (Phase 0.1)

docs/
  todo/task/
    config-wizard-plan.md ← this file
  config-wizard.md        ← user-facing wizard docs (write after Phase 1)
```

---

## Acceptance Criteria

| Criterion | How verified |
|-----------|-------------|
| Wizard produces YAML that passes `--dry-run` | End-to-end test per reader/writer type combo |
| All conditional required fields enforced | Unit tests per field + integration |
| Routing cross-ref errors caught | Unit test: routing names not in declared instances |
| Reader with 0 PVs: no `pvs:` block emitted, `config validate` exits 0 | Unit test |
| hdf5 writer + 0-PV reader + no globs: info note shown, config still valid | Unit test |
| `config validate` exits 0 on valid examples | CI: run against `docs/examples/*.yaml` |
| `config validate` exits 1 on injected errors | Unit tests per error type |
| `--from` round-trips: load → accept all → output matches input | Round-trip test |
| Wizard runs with no extra runtime deps | FTXUI linked statically via FetchContent; verify `ldd` shows no new .so |
| FTXUI renders correctly over SSH | Manual test: `ssh host mldp_pvxs_driver config wizard` |
| `--non-interactive` mode bypasses FTXUI | CI test with response file |

---

## Open Questions

1. **`config validate` error line numbers**: ryml provides byte offsets — worth mapping to line numbers for better UX?
2. **Wizard i18n**: not needed now, but avoid hardcoded English strings in `wizard_ui.hpp` if future need likely.
3. **HDF5 `merge-root-sources` flag** (in `configuration.md`) vs. separate `hdf5-merge` type (in `README.md`): two representations of same feature — reconcile before implementing Phase 1.2 writer loop.
4. **FTXUI version pin**: v5.0.0 current stable — check for breaking API changes if bumped.

---

## References

- `docs/configuration.md` — complete schema reference
- `docs/examples/` — existing YAML examples (used by `config template`)
- `docs/user-guide.md` — annotated operator examples
- `README.md` — top-level config summary and CLI options
