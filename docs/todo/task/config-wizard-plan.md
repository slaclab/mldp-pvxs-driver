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
- Full TUI (ncurses/FTXUI): high impl cost, low marginal benefit over prompts
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

## Sub-command: `config wizard`

### Interaction Model

Sequential prompts in terminal. Pure stdin/stdout — no ncurses.  
Library: use existing C++ readline or a minimal prompt helper (no new heavy deps).  
Alternatively, implement wizard as a **Python script** (simpler, no recompile for changes).

> **Recommendation**: implement wizard as `tools/config_wizard.py` (Python 3, stdlib only).  
> Ship alongside binary. No new C++ build complexity.

### Wizard Flow (Ordered)

```
Phase 1 — Controller
  → controller name (optional, default: "default")

Phase 2 — Writers (repeat until done)
  → writer type: [mldp | hdf5 | hdf5-merge]
  IF mldp:
    → instance name
    → thread-pool (default: 1)
    → stream-max-bytes (default: 2097152)
    → stream-max-age-ms (default: 200)
    → mldp-pool.provider-name
    → mldp-pool.provider-description (opt)
    → mldp-pool.ingestion-url
    → mldp-pool.query-url (opt)
    → mldp-pool.min-conn (default: 1)
    → mldp-pool.max-conn (default: 4)
    → credentials type: [none | ssl | custom-tls]
      IF custom-tls:
        → pem-cert-chain path (opt)
        → pem-private-key path (opt)
        → pem-root-certs path (opt)
  IF hdf5 | hdf5-merge:
    → instance name
    → base-path
    → max-file-age-s (default: 3600)
    → max-file-size-mb (default: 512)
    → flush-interval-ms (default: 1000)
    → compression-level 0–9 (default: 0)
  → add another writer? [y/N]

Phase 3 — Readers (repeat until done)
  → reader type: [epics-pvxs | epics-base | epics-archiver]
  IF epics-pvxs | epics-base:
    → instance name
    → thread-pool (default: 2)
    → column-batch-size (default: 50)
    IF epics-base:
      → monitor-poll-threads (default: 2)
      → monitor-poll-interval-ms (default: 5)
    → PVs (loop):
        → PV name
        → option type: [none | scalar | slac-bsas-table]
          IF scalar: → option value string
          IF slac-bsas-table:
            → tsSeconds field name
            → tsNanos field name
        → add another PV? [y/N]
  IF epics-archiver:
    → instance name
    → hostname (host:port)
    → mode: [historical_once | periodic_tail]
    IF historical_once:
      → start-date (ISO 8601, required)
      → end-date (ISO 8601, optional)
    IF periodic_tail:
      → poll-interval-sec (required)
      → lookback-sec (default: poll-interval-sec)
    → connect-timeout-sec (default: 30)
    → total-timeout-sec (default: 300)
    → batch-duration-sec (default: 1)
    → tls-verify-peer (default: true)
    → tls-verify-host (default: true)
    → PVs (loop): same as above but no option
  → add another reader? [y/N]

Phase 4 — Metrics (optional)
  → enable Prometheus endpoint? [y/N]
  IF yes:
    → endpoint bind address (default: 0.0.0.0:9464)
    → scan-interval-seconds (default: 1)

Phase 5 — Routing (optional)
  → configure explicit routing? [y/N]  (default: all-to-all)
  IF yes:
    → for each writer instance (collected in Phase 2):
        → select source readers from list (collected in Phase 3)
        → add include glob patterns? [y/N]
        → add exclude glob patterns? [y/N]

Phase 6 — Review & Save
  → print generated YAML to stdout
  → confirm save? output path (default: config.yaml)
  → run --dry-run validation? [Y/n]
```

### Validation During Wizard

| Check | When |
|-------|------|
| Instance name uniqueness | On each name entry |
| Non-empty name | On each name entry |
| Port in hostname for archiver | After hostname entry |
| ISO 8601 format for dates | After date entry |
| `start-date` required for `historical_once` | Before leaving archiver phase |
| `poll-interval-sec` required for `periodic_tail` | Before leaving archiver phase |
| `total-timeout-sec >= connect-timeout-sec` | After both timeout entries |
| `compression-level` in 0–9 | After entry |
| Routing names exist in declared writers/readers | Phase 5 (use collected lists) |
| At least one writer | End of Phase 2 |

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

Deliverable: `config validate` and `config template` working. Zero wizard code yet.

---

### Phase 1 — Wizard Core (Python, stdlib only)

**1.1** `tools/config_wizard.py` — prompt engine  
- `prompt(question, default, validator)` → string  
- `prompt_choice(question, choices)` → index  
- `prompt_bool(question, default)` → bool  
- `prompt_list(item_fn)` → list (loops until user stops)

**1.2** Phase 1: controller name  
**1.3** Phase 2: writer loop (mldp + hdf5 + hdf5-merge)  
**1.4** Phase 3: reader loop (epics-pvxs + epics-base + epics-archiver)  
**1.5** Phase 4: metrics  
**1.6** Phase 5: routing (select from collected names, glob pattern entry)  
**1.7** Phase 6: YAML generation + save + optional dry-run invoke  

**1.8** `--from PATH` amend mode (load YAML, pre-fill defaults)

---

### Phase 2 — C++ Integration (optional, post-MVP)

Wire `mldp_pvxs_driver config wizard` to invoke `tools/config_wizard.py` via `execvp` / `system()`.  
Alternatively: reimplement wizard prompts in C++ using same flow if Python not available in target env.

Target env (container image) has Python 3 — Phase 2 may not be needed.

---

### Phase 3 — Polish

- Color output (ANSI, disabled if `NO_COLOR` or non-tty)
- Progress indicator ("Phase 2 of 6 — Writers")
- `--non-interactive` mode: wizard reads answers from a response file (for CI/scripted use)
- Shell completion for `config` subcommand

---

## File Layout

```
tools/
  config_wizard.py          ← Phase 1 deliverable
  config_wizard_test.py     ← unit tests for validators and YAML output

src/
  config/
    validate.hpp            ← validateConfig() declaration
    validate.cpp            ← validateConfig() implementation (Phase 0.4)
    template.cpp            ← template string constants (Phase 0.3)
    subcommand.cpp          ← "config" subcommand dispatch (Phase 0.1)

docs/
  config-wizard-plan.md     ← this file
  config-wizard.md          ← user-facing wizard docs (write after Phase 1)
```

---

## Acceptance Criteria

| Criterion | How verified |
|-----------|-------------|
| Wizard produces YAML that passes `--dry-run` | `config_wizard_test.py` end-to-end |
| All conditional required fields enforced | Unit tests per field + integration |
| Routing cross-ref errors caught | Unit test: routing names not in declared instances |
| `config validate` exits 0 on valid examples | CI: run against `docs/examples/*.yaml` |
| `config validate` exits 1 on injected errors | Unit tests per error type |
| `--from` round-trips: load → accept all → output matches input | Round-trip test |
| Wizard runs with Python 3.8+ stdlib only | `python3 -c "import sys; assert sys.version_info >= (3,8)"` |

---

## Open Questions

1. **Python availability in AppImage/standalone artifact?** If not guaranteed, Phase 2 C++ rewrite becomes mandatory not optional.
2. **`config validate` error line numbers**: ryml provides byte offsets — worth mapping to line numbers for better UX?
3. **Wizard i18n**: not needed now, but avoid hardcoded English strings in prompt engine if future need likely.
4. **HDF5 `merge-root-sources` flag** (in `configuration.md`) vs. separate `hdf5-merge` type (in `README.md`): these appear to be two representations of same feature — need to reconcile before implementing wizard Phase 1.3.

---

## References

- `docs/configuration.md` — complete schema reference
- `docs/examples/` — existing YAML examples (used by `config template`)
- `docs/user-guide.md` — annotated operator examples
- `README.md` — top-level config summary and CLI options
