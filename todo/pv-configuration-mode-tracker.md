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

### 6. Mock PV for mode tracker integration test

**Goal:** Add two mock mbbi PVs (`MPS:UNDS:3500:SXRSS_MODE`, `MPS:UNDH:2850:HXRSS_MODE`) to the existing `PVServer` mock, with random state changes. Write an integration test verifying each PV state change produces a new `ConfigurationActivation`.

**Files to modify:**
- `test/mock/sioc.h` — add two `pvxs::server::SharedPV` members for mode PVs
- `test/mock/sioc.cpp` — register PVs as `Int32` NTScalar, update in the background thread with random enum cycling

**Mock PV behaviour:**
```cpp
// In PVServer constructor — register two mode PVs as Int32 (mbbi is integer-valued)
m_pvSxrssMode = server::SharedPV::buildReadonly();
m_pvSxrssMode.open(nt::NTScalar{TypeCode::Int32}.create());
m_server.addPV("MPS:UNDS:3500:SXRSS_MODE", m_pvSxrssMode);

m_pvHxrssMode = server::SharedPV::buildReadonly();
m_pvHxrssMode.open(nt::NTScalar{TypeCode::Int32}.create());
m_server.addPV("MPS:UNDH:2850:HXRSS_MODE", m_pvHxrssMode);

// In update thread — cycle state randomly each iteration
static std::mt19937 rng(42);  // deterministic seed for reproducibility
{
    auto pv = m_pvSxrssMode.fetch();
    static int sxrss_state = 0;
    int new_state;
    do { new_state = std::uniform_int_distribution<int>(0, 4)(rng); } while (new_state == sxrss_state);
    sxrss_state = new_state;
    pv["value"] = sxrss_state;
    pv["timeStamp.secondsPastEpoch"] = seconds;
    pv["timeStamp.nanoseconds"] = nanos;
    m_pvSxrssMode.post(pv);
}
{
    auto pv = m_pvHxrssMode.fetch();
    static int hxrss_state = 0;
    int new_state;
    do { new_state = std::uniform_int_distribution<int>(0, 2)(rng); } while (new_state == hxrss_state);
    hxrss_state = new_state;
    pv["value"] = hxrss_state;
    pv["timeStamp.secondsPastEpoch"] = seconds;
    pv["timeStamp.nanoseconds"] = nanos;
    m_pvHxrssMode.post(pv);
}
```

**New integration test file:** `test/controller/mldppvxs_controller_configuration_mode_tracker_test.cpp`

**Test structure (follows existing `mldp_configuration_writer_test.cpp` pattern):**

```cpp
// 1. Start PVServer mock (provides SXRSS_MODE + HXRSS_MODE with random changes)
// 2. Start fake gRPC annotation service (TestAnnotationService) capturing requests
// 3. Configure MLDPPVXSController with:
//    - reader: epics-pvxs monitoring both mode PVs
//    - processor: python-script pointing to configuration_mode_tracker.py
//    - writer: mldp-configuration connected to fake annotation service
//    - routing: reader → processor → writer
//    - queryable: mldp-pv-metadata (pointed at same fake annotation service)
// 4. Start controller, wait for N PV updates (N >= 4)
// 5. Stop controller
// 6. Assert:
//    - save_configuration_activation_count >= N (each PV change → at least 1 new activation)
//    - Each captured SaveConfigurationActivationRequest has a unique client_activation_id
//    - For consecutive activations of the same prefix:
//      * previous activation has end_time set (closed)
//      * new activation has no end_time (open)
//    - Configuration names match expected pattern: "{prefix}-{state_name}"

constexpr std::string_view kModeTrackerControllerConfig = R"(
writer:
  mldp-configuration:
    - name: config_writer
      mldp-annotation-pool:
        provider-name: test-provider
        ingestion-url: 127.0.0.1:{annotation_port}
        query-url: 127.0.0.1:{query_port}
        annotation-url: 127.0.0.1:{annotation_port}
        min-conn: 1
        max-conn: 1
reader:
  epics-pvxs:
    - name: mode_pvs
      pvs:
        - name: "MPS:UNDS:3500:SXRSS_MODE"
        - name: "MPS:UNDH:2850:HXRSS_MODE"
processors:
  - type: python-script
    script-path: {script_path}
routing:
  configuration_mode_tracker:
    from: [mode_pvs]
  config_writer:
    from: [configuration_mode_tracker]
queryable:
  - type: mldp-pv-metadata
)";

TEST(ConfigurationModeTrackerIntegrationTest, EachPVChangeGeneratesNewActivation)
{
    // 1. Start mock PV server
    PVServer pvServer;

    // 2. Start fake annotation gRPC service
    TestAnnotationService annotationService;
    grpc::ServerBuilder builder;
    int annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &annotation_port);
    builder.RegisterService(&annotationService);
    auto grpcServer = builder.BuildAndStart();
    ASSERT_TRUE(grpcServer);

    // 3. Build controller config (substitute ports)
    // ... format kModeTrackerControllerConfig with actual ports ...

    // 4. Create and start controller
    auto controller = MLDPPVXSController(config);
    controller.start();

    // 5. Wait for at least 4 activation RPCs (2 PVs × 2 changes each minimum)
    ASSERT_TRUE(waitForCount(annotationService.save_configuration_activation_count, 4,
                             std::chrono::seconds(10)));

    controller.stop();

    // 6. Verify each state change produced a new activation
    std::lock_guard<std::mutex> lock(annotationService.captured_mutex);
    auto& activations = annotationService.captured_activation_requests;

    // All activation IDs must be unique
    std::set<std::string> activation_ids;
    for (auto& req : activations)
    {
        EXPECT_TRUE(activation_ids.insert(req.clientactivationid()).second)
            << "Duplicate activation ID: " << req.clientactivationid();
    }

    // At least one activation per PV (proves both PVs are tracked)
    bool has_sxrss = std::any_of(activations.begin(), activations.end(),
        [](auto& r) { return r.configurationname().find("sxrss-mode") != std::string::npos; });
    bool has_hxrss = std::any_of(activations.begin(), activations.end(),
        [](auto& r) { return r.configurationname().find("hxrss-mode") != std::string::npos; });
    EXPECT_TRUE(has_sxrss);
    EXPECT_TRUE(has_hxrss);
}
```

**Key points:**
- Reuses existing `PVServer` mock (same pattern as `mldppvxs_controller_mldp_writer_integration_test.cpp`)
- Random state changes guarantee value differs each cycle → triggers processor
- Test asserts count (each change = new activation) + uniqueness (no duplicate IDs) + coverage (both PVs tracked)
- Deterministic seed (`std::mt19937(42)`) makes test reproducible

---

### 7. Verification

- [ ] Build inside devcontainer — C++ compiles with fixed payload bridge
- [ ] Unit test: Python `configuration_activation(source, end_time=..., client_activation_id=...)` produces correct `ConfigurationActivationPayload`
- [ ] Unit test: mock gRPC stub receives correct `saveConfigurationActivation` with `end_time` set (close) and without (open)
- [ ] Integration: Python processor loads, PV change → events emitted → writer persists
- [ ] Integration: mock PV random state cycling → each change generates unique `ConfigurationActivation`
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
