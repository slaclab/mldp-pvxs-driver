# TODO: PV-Driven Configuration Activation Tracker

## Goal

Monitor two PVs that emit enum states. On each state change:
1. Close current open `ConfigurationActivation` (set `endTime`)
2. Open new `ConfigurationActivation` for the new state (no `endTime`)

All using the driver's existing gRPC pool — no separate Python gRPC connection.

## PVs

| PV | Type | States |
|----|------|--------|
| `MPS:UNDS:3500:SXRSS_MODE` | mbbi | [0] Undefined, [1] SASE, [2] Seeded, [3] Delay Line, [4] SLIT |
| `MPS:UNDH:2850:HXRSS_MODE` | mbbi | [0] Undefined, [1] Seeded, [2] SASE/Phase Shift |

---

## Tasks

### 1. Add save methods to `MLDPAnnotationQueryClient`

**Files:**
- `include/query/impl/mldp/MLDPAnnotationQueryClient.h`
- `src/query/MLDPAnnotationQueryClient.cpp`

**Methods:**

```cpp
std::optional<std::string>
saveConfiguration(const dp::service::annotation::SaveConfigurationRequest& request);

std::optional<std::string>
saveConfigurationActivation(const dp::service::annotation::SaveConfigurationActivationRequest& request);

std::optional<dp::service::common::ConfigurationActivation>
getLatestActiveConfiguration(const std::string& configurationName);
```

**Notes:**
- Same pattern as existing read methods: `pool_->acquire()` → build request → `stub->saveXxx()` → check status → extract result
- `getLatestActiveConfiguration` uses `queryConfigurationActivations` with ANDed `ConfigurationNameCriterion` + `TimestampCriterion(now)` — single RPC, server-side filter
- No deadline (matches existing query methods)
- `saveConfigurationActivation` handles both create (new `clientActivationId`) and update/close (same `clientActivationId` with `endTime` set)

---

### 2. Create C++ → Python bridge for annotation queries

**Why:** Existing Python processors only emit payloads (fire-and-forget via bus). Close→open cycle needs read-then-write. Bridge exposes C++ client to embedded Python.

**New files:**
- `include/processor/MldpPythonAnnotationBridge.h`
- `src/processor/MldpPythonAnnotationBridge.cpp`

**Modify:**
- `src/processor/PythonScriptDirectoryLoader.cpp` — register `PyMethodDef` functions on `mldp` module in `ensurePythonReady()`
- `src/controller/MLDPPVXSController.cpp` — after queryable creation, pass `shared_ptr<MLDPAnnotationQueryClient>` to bridge singleton

**Python functions exposed:**

| Python | C++ | Returns |
|--------|-----|---------|
| `mldp.save_configuration(name, category, **kwargs)` | `saveConfiguration(req)` | config name `str` or raises |
| `mldp.save_configuration_activation(config_name, start_time, client_activation_id=None, end_time=None, **kwargs)` | `saveConfigurationActivation(req)` | activation ID `str` or raises |
| `mldp.get_active_configuration(config_name)` | `getLatestActiveConfiguration(name)` | `dict` or `None` |
| `mldp.query_configuration_activations(config_name)` | `queryConfigurationActivations(req)` | `list[dict]` |

**Thread safety:**
- Release GIL (`Py_BEGIN_ALLOW_THREADS`) before gRPC calls, reacquire after
- `MLDPAnnotationQueryClient` is thread-safe (pool-based)

**Lifetime:**
- Bridge holds `shared_ptr` to client — lives for process duration
- Set before any `compute()` runs

---

### 3. Write Python processor script

**File:** `scripts/processors/configuration_mode_tracker.py`

**Logic:**
```
on PV change:
  1. save_configuration(new_config_name)          — idempotent upsert
  2. get_active_configuration(prefix_configs...)   — find open activation
  3. save_configuration_activation(old, endTime)   — close it
  4. save_configuration_activation(new, startTime) — open new
```

**Config mapping:**
- `MPS:UNDS:3500:SXRSS_MODE` → prefix `sxrss-mode`, configs: `sxrss-mode-undefined`, `sxrss-mode-sase`, etc.
- `MPS:UNDH:2850:HXRSS_MODE` → prefix `hxrss-mode`, configs: `hxrss-mode-undefined`, `hxrss-mode-seeded`, etc.

**Edge cases:**
- Startup: no `_state` → first value always triggers new activation (query finds nothing → skip close, just open)
- Undefined (0): tracked as its own activation
- Duplicate `saveConfiguration`: idempotent (upsert by name)
- Restart: stateless — queries MLDP for current active on first trigger

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
```

---

### 5. Verification

- [ ] Build inside devcontainer — C++ compiles with new methods + bridge
- [ ] Unit test: mock gRPC stub for `saveConfiguration`, `saveConfigurationActivation`
- [ ] Integration: Python processor loads, PV change → configuration + activation created
- [ ] Restart test: processor restarts, queries current state, resumes correctly
- [ ] Timeline test: `queryConfigurationActivations` shows continuous coverage (no gaps, no overlaps)

---

## Architecture Flow

```
EPICS PV (mbbi) → pvxs reader → ChannelProcessor → PythonAlgorithm::compute()
                                                         │
                                                         ▼
                                                  Python script calls:
                                                  mldp.get_active_configuration()
                                                  mldp.save_configuration_activation()
                                                         │
                                                         ▼ (via PyMethodDef bridge)
                                                  MLDPAnnotationQueryClient
                                                         │
                                                         ▼ (gRPC, existing pool)
                                                  MLDP Annotation Service
```

## Open Design Choice

**How to find "current active for this prefix":**

Option A (recommended): Query all known config names for the prefix (finite set — known states). Stateless on restart.

Option B: Store last-opened activation ID in `_state`. Lost on restart — must query anyway as fallback.

→ Use **A**. Always query. Script is stateless.
