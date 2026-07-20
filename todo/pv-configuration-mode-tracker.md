# TODO: PV-Driven Configuration Activation Tracker

## Goal

Monitor two PVs that emit enum states. On each state change:
1. Close current open `ConfigurationActivation` (emit event with `endTime`)
2. Open new `ConfigurationActivation` for the new state (emit event without `endTime`)

Uses the existing data flow: **PVXS reader → routing → processor → bus → MLDPConfigurationWriter** (gRPC upsert).

## PVs

| PV | Type | States |
|----|------|--------|
| `MPS:UNDS:3500:SXRSS_MODE` | mbbi | [0] Undefined, [1] SASE, [2] Seeded, [3] Delay Line, [4] SLIT |
| `MPS:UNDH:2850:HXRSS_MODE` | mbbi | [0] Undefined, [1] Seeded, [2] SASE/Phase Shift |

---

## Architecture Flow

```
EPICS PV (mbbi)
    │
    ▼
EpicsPVXSReader (reads via pvxs monitor)
    │
    ▼ routing: connects reader → processor
    │
ChannelProcessor → PythonAlgorithm::compute()
    │
    ▼ Python script:
    │   1. mldp.query_configuration_activations(prefix)   ← read-only query bridge
    │   2. return [                                        ← emit events via bus
    │        mldp.configuration(name, category=...)        → ConfigurationPayload (upsert)
    │        mldp.configuration_activation(name, end_time=now, client_activation_id=old_id)  → close old
    │        mldp.configuration_activation(name, client_activation_id=new_id)                → open new
    │      ]
    │
    ▼ bus_->push() for each emitted event
    │
MLDPConfigurationWriter (gRPC saveConfiguration / saveConfigurationActivation = upsert)
```

---

## Tasks

### 1. Fix incomplete `configuration_activation` payload bridge

**Files:**
- `src/processor/impl/PythonAlgorithm.cpp` (lines 503–510)
- `src/processor/PythonScriptDirectoryLoader.cpp` (line 53)

**Problem:** The `"configuration_activation"` branch in `payloadFromPyObject()` currently ignores the `data` dict — only sets `configuration_name` and `start_time` (from `reference_time`). Cannot express close (needs `end_time`) or idempotent update (needs `client_activation_id`).

**Fix in `PythonAlgorithm.cpp`:**
Parse `data` dict fields:
- `client_activation_id` → `payload.client_activation_id`
- `start_time` (epoch float or dict `{seconds, nanos}`) → `payload.start_time` (override default)
- `end_time` (same format) → `payload.end_time`
- `description` → `payload.description`
- `tags` (list of str) → `payload.tags`
- `modified_by` → `payload.modified_by`
- Remaining string keys → `payload.attributes`

Pattern: follow `configurationPayloadFromDict()` (same file, lines 153–213).

**Fix in `PythonScriptDirectoryLoader.cpp`:**
Update the `mldp` module helper:
```python
def configuration_activation(source, **kwargs) -> _MldpPayload:
    return _MldpPayload("configuration_activation", source, kwargs)
```

---

### 2. Python query bridge (read-only)

**Why:** The processor must query MLDP to discover current active configurations before deciding what events to emit (close old → open new). This is read-only; writes go through bus → writer.

**New files:**
- `include/processor/MldpPythonQueryBridge.h`
- `src/processor/MldpPythonQueryBridge.cpp`

**Modify:**
- `src/processor/PythonScriptDirectoryLoader.cpp` — in `ensurePythonReady()`, register `PyMethodDef` functions on the `mldp` module

**Python script declares queryable dependency:**
```python
config = {
    "name": "configuration_mode_tracker",
    "sources": ["MPS:UNDS:3500:SXRSS_MODE", "MPS:UNDH:2850:HXRSS_MODE"],
    "queryable": "mldp-pv-metadata"  # triggers QueryableFactory usage
}
```

**Python functions exposed (read-only):**

| Python | C++ | Returns |
|--------|-----|---------|
| `mldp.get_active_configurations(timestamp=None)` | `MLDPAnnotationQueryClient::getActiveConfigurations(at)` | `list[dict]` |
| `mldp.query_configuration_activations(config_name)` | `MLDPAnnotationQueryClient::queryConfigurationActivations(req)` | `list[dict]` |
| `mldp.get_configuration(name)` | `MLDPAnnotationQueryClient::getConfiguration(name)` | `dict` or `None` |

**Thread safety:**
- Release GIL (`Py_BEGIN_ALLOW_THREADS`) before gRPC calls, reacquire after
- `MLDPAnnotationQueryClient` is thread-safe (pool-based)

**Lifetime:**
- Bridge holds `shared_ptr<MLDPAnnotationQueryClient>` created from `QueryableFactory::create<MLDPAnnotationQueryClient>()`
- Initialized once during `ensurePythonReady()` if `mldp-pv-metadata` queryable is prepared

---

### 3. Python processor script

**File:** placed in configured `script-dir` or referenced by `script-path`

**Logic:**
```python
import mldp

config = {
    "name": "configuration_mode_tracker",
    "sources": ["MPS:UNDS:3500:SXRSS_MODE", "MPS:UNDH:2850:HXRSS_MODE"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "queryable": "mldp-pv-metadata"
}

PV_CONFIG = {
    "MPS:UNDS:3500:SXRSS_MODE": {
        "prefix": "sxrss-mode",
        "states": ["undefined", "sase", "seeded", "delay-line", "slit"]
    },
    "MPS:UNDH:2850:HXRSS_MODE": {
        "prefix": "hxrss-mode",
        "states": ["undefined", "seeded", "sase-phase-shift"]
    }
}

def compute(snapshot):
    results = []
    for pv_name, pv_data in snapshot.items():
        cfg = PV_CONFIG.get(pv_name)
        if not cfg:
            continue
        state_index = int(pv_data["value"])
        new_config_name = f"{cfg['prefix']}-{cfg['states'][state_index]}"

        # 1. Upsert configuration (idempotent)
        results.append(mldp.configuration(
            new_config_name,
            category="undulator-mode"
        ))

        # 2. Query current active for all states of this prefix
        all_config_names = [f"{cfg['prefix']}-{s}" for s in cfg['states']]
        for name in all_config_names:
            activations = mldp.query_configuration_activations(name)
            for act in activations:
                if act.get("end_time") is None:
                    # 3. Close active activation
                    results.append(mldp.configuration_activation(
                        name,
                        client_activation_id=act["client_activation_id"],
                        end_time=snapshot.timestamp
                    ))

        # 4. Open new activation
        results.append(mldp.configuration_activation(
            new_config_name,
            client_activation_id=f"{new_config_name}-{snapshot.timestamp}"
        ))

    return results
```

**Edge cases:**
- Startup: no prior state → query finds nothing → skip close, just open
- Undefined (0): tracked as its own activation
- Duplicate `configuration`: idempotent (upsert by name via writer)
- Restart: stateless — always queries MLDP for current active

---

### 4. YAML configuration

```yaml
reader:
  epics-pvxs:
    - name: mode_pvs
      pvs:
        - name: "MPS:UNDS:3500:SXRSS_MODE"
        - name: "MPS:UNDH:2850:HXRSS_MODE"

processors:
  - type: python-script
    script-path: /opt/scripts/processors/configuration_mode_tracker.py

writer:
  mldp-configuration:
    - name: config_writer

routing:
  configuration_mode_tracker:
    from: [mode_pvs]
  config_writer:
    from: [configuration_mode_tracker]

queryable:
  - type: mldp-pv-metadata
```

---

### 5. Validate MLDPConfigurationWriter upsert semantics

**No code change needed.** Document findings:

- `MLDPConfigurationWriter::doSaveConfiguration()` calls gRPC `saveConfiguration` RPC — upsert by `configuration_name`
- `MLDPConfigurationWriter::doSaveConfigurationActivation()` calls gRPC `saveConfigurationActivation` RPC — upsert by `client_activation_id`
- Both insert new records or update existing ones
- `client_activation_id` is the idempotency key: same ID + new `end_time` = close an activation
- Writer accepts both `ConfigurationPayload` and `ConfigurationActivationPayload` variants from bus

---

### 6. Verification

- [ ] Build inside devcontainer — C++ compiles with fixed payload bridge
- [ ] Unit test: Python `configuration_activation(source, end_time=..., client_activation_id=...)` produces correct `ConfigurationActivationPayload`
- [ ] Unit test: mock gRPC stub receives correct `saveConfigurationActivation` with `end_time` set (close) and without (open)
- [ ] Integration: Python processor loads, PV change → events emitted → writer persists
- [ ] Restart test: processor restarts, queries current state, resumes correctly
- [ ] Timeline test: activations show continuous coverage (no gaps, no overlaps)

---

## Design Choices

**Why no write bridge?**
The bus → writer path already handles writes. Adding PyMethodDef write functions would bypass the bus, lose routing/enrichment, and duplicate the gRPC logic. Processors emit events; writers persist them.

**Why a read-only query bridge?**
The processor needs to know *what's currently active* before deciding to close or open. This is a read (not a write) — it queries the annotation service. The `MLDPAnnotationQueryClient` (via `QueryableFactory`) provides this.

**How to find "current active for this prefix":**
Query all known config names for the prefix (finite set — known states). Always query. Script is stateless. Same as original Option A.
