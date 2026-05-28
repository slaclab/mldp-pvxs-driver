# Plan: EpicsDSMetadataReader

## Context

New reader class that queries the EPICS Directory Service (`ds`) via a single **pvxs** `client::Context::rpc()` call, parses the returned `NTTable`, and pushes `SourceMetadataPayload` records onto the `IDataBus`. This implements the fetch side as a proper driver reader (not a standalone CLI), so the metadata flows through the existing writer pipeline to the MLDP annotation service.

The codebase uses **pvxs** exclusively (`pvxs::client::Context`, `pvxs::server::Server`) — the old pvAccessCPP API (`ChannelProviderRegistry`, `ChannelRPC`, `ChannelRPCRequester`) is **not used** anywhere in this project and must not appear in this reader.

---

## SourceMetadataEntry Gap Analysis

DS NTTable columns from `ds-mock-data.jsonl`:
`channelName`, `hostName`, `iocName`, `owner`, `pvStatus`, `recordType`, `recordDesc`, `archived`, `archiveRate`, `tags`

Column discovery is **fully dynamic** — no column names are hardcoded in the reader. The config designates one column as the source name key; every other column becomes an `attributes` entry regardless of its label.

Mapping to `SourceMetadataEntry` (`include/util/bus/IDataBus.h:45`):

| DS column | `SourceMetadataEntry` field | Notes |
|---|---|---|
| configured `source-name-column` | map key (source name) | outer `SourceMetadataPayload` key; excluded from `attributes` |
| configured `tags-column` | `tags` (`vector<string>`) | split on `,`, whitespace-trimmed; excluded from `attributes`; `std::nullopt` if not configured |
| all other columns | `attributes[label] = value` | fully dynamic; whatever `ds` returns |
| — | `description` | left empty (`std::nullopt`) |
| — | `aliases` | left empty (`std::nullopt`) |
| — | `modified_by` | left empty (`std::nullopt`) |

**Verdict**: `SourceMetadataEntry` is sufficient. Exactly two columns are configured as special (`source-name-column`, `tags-column`). All remaining columns → `attributes` verbatim. No column names hardcoded in reader logic.

---

## New Files

```
include/reader/impl/epics_ds/
  EpicsDSMetadataReaderConfig.h
  EpicsDSMetadataReader.h
src/reader/impl/epics_ds/
  EpicsDSMetadataReaderConfig.cpp
  EpicsDSMetadataReader.cpp
test/mock/
  MockDSServer.h
  MockDSServer.cpp
test/reader/impl/epics_ds/
  epics_ds_metadata_reader_config_test.cpp
  epics_ds_metadata_reader_test.cpp
test/controller/
  mldppvxs_controller_ds_metadata_integration_test.cpp
```

---

## EpicsDSMetadataReaderConfig

Modelled on `EpicsArchiverReaderConfig` (`include/reader/impl/epics_archiver/EpicsArchiverReaderConfig.h`).

YAML:
```yaml
readers:
  - type: epics-ds-metadata
    name: ds-metadata-reader
    service: ds                      # pvAccess channel name of directory service, default "ds"
    query: "%"                       # wildcard pattern sent to ds, default "%"
    timeout-sec: 5.0                 # RPC wait timeout in seconds, default 5.0
    source-name-column: channelName  # NTTable column used as SourceMetadataPayload key, default "channelName"
    tags-column: tags                # NTTable column split into SourceMetadataEntry::tags, default "" (disabled)
    rescan-interval-sec: 300         # seconds to wait between scans; 0 = run once and exit, default 0
```

Private fields:
- `name_` — string, required
- `service_` — string, default `"ds"`
- `query_` — string, default `"%"`
- `timeout_sec_` — double, default `5.0`
- `source_name_column_` — string, default `"channelName"`
- `tags_column_` — string, default `""` (empty = no tags column)
- `rescan_interval_sec_` — double, default `0.0` (0 = single-shot, >0 = periodic)

`source-name-column` — column becomes the `SourceMetadataPayload` map key; excluded from `attributes`.

`tags-column` — column value is split on `,` (trimming whitespace) into `SourceMetadataEntry::tags`; excluded from `attributes`. If empty string or column not found in response, `tags` field is left as `std::nullopt`.

All remaining columns → `attributes[label] = value` verbatim.

---

## EpicsDSMetadataReader

Inherits: `reader::Reader` (`include/reader/IReader.h:34`)

Registration macro: `REGISTER_READER("epics-ds-metadata", EpicsDSMetadataReader)`

Constructor signature (matches `ReaderRegistrator` lambda at `include/reader/ReaderFactory.h:86`):
```cpp
EpicsDSMetadataReader(std::shared_ptr<util::bus::IDataBus> bus,
                      std::shared_ptr<metrics::Metrics>    metrics,
                      const config::Config&                cfg);
```

### Members

| Member | Type | Purpose |
|---|---|---|
| `config_` | `EpicsDSMetadataReaderConfig` | parsed config |
| `logger_` | `shared_ptr<ILogger>` | logging |
| `pva_context_` | `pvxs::client::Context` | pvxs RPC client (RAII) |
| `worker_thread_` | `std::thread` | fetch loop |
| `running_` | `atomic<bool>` | lifecycle flag |
| `worker_cv_` | `std::condition_variable` | interruptible sleep between scans |
| `worker_mutex_` | `std::mutex` | guards `worker_cv_` |
| `worker_error_` | `exception_ptr` | diagnostics |

### Lifecycle
- Constructor: parses config, builds `pva_context_`, starts worker thread
- Destructor: sets `running_ = false`, signals `worker_cv_`, joins thread; `pva_context_` destroyed by RAII
- `name()`: returns `config_.name()`

### Worker thread (`runWorker()`)

Uses pvxs synchronous RPC — no callback class needed.

```
pva_context_ = pvxs::client::Config::fromEnv().build()

do:
  // Build NTURI argument
  Value arg = TypeDef(TypeCode::Struct, "epics:nt/NTURI:1.0", {
      Member(TypeCode::String, "scheme"),
      Member(TypeCode::String, "path"),
      Member(TypeCode::Struct, "query", {
          Member(TypeCode::String, "name"),
      }),
  }).create();
  arg["scheme"]      = "pva";
  arg["path"]        = config_.service();
  arg["query.name"]  = config_.query();

  // Synchronous RPC (throws pvxs::Timeout or pvxs::RemoteError on failure)
  try:
    Value result = pva_context_.rpc(config_.service(), arg)
                                .timeout(std::chrono::duration<double>(config_.timeoutSec()))
                                .exec()
                                ->wait(std::chrono::duration<double>(config_.timeoutSec()));

    payload = parseNTTable(result)
    bus_->push(EventBatchStruct{ .reader_name=config_.name(), .root_source="", .metadata={}, .payload=payload })
  catch (std::exception& e):
    log error; continue

  if config_.rescanIntervalSec() <= 0.0: break   // single-shot

  // interruptible sleep: wakes early when running_ → false
  unique_lock lk(worker_mutex_)
  worker_cv_.wait_for(lk, chrono::duration<double>(config_.rescanIntervalSec()),
                      [&]{ return !running_.load(); })

while running_.load()
```

`rescan-interval-sec: 0` (default) → single fetch then exit.
`rescan-interval-sec: N` → fetch, sleep N seconds (interruptible), repeat until destructor signals.

### NTTable → SourceMetadataPayload parsing (`parseNTTable`)

Fully dynamic — no column names hardcoded. Uses pvxs `Value` API (same library used everywhere else in this project).

```cpp
SourceMetadataPayload parseNTTable(const pvxs::Value& result) {
    // 1. labels
    auto labels = result["labels"].as<pvxs::shared_array<const std::string>>();
    const size_t ncols = labels.size();

    // 2. Locate configured columns by scanning labels
    size_t srcIdx  = 0;              // source-name-column; fallback col 0, warn if not found
    size_t tagsIdx = SIZE_MAX;       // tags-column; SIZE_MAX = disabled / not found

    for (size_t i = 0; i < ncols; ++i) {
        if (labels[i] == config_.sourceNameColumn()) srcIdx  = i;
        if (!config_.tagsColumn().empty() && labels[i] == config_.tagsColumn()) tagsIdx = i;
    }
    // warn if sourceNameColumn not found (srcIdx stays 0)

    // 3. Extract each column as shared_array<const std::string>
    //    Field name in value struct == labels[i] (pvxs uses the label string as field name)
    //    For non-string columns use columnAsString() helper (see below)

    // 4. For each row r:
    //    key  = colData[srcIdx][r]
    //    if tagsIdx != SIZE_MAX:
    //        entry.tags = split(colData[tagsIdx][r], ',')  // trim whitespace, skip empty tokens
    //    for each column i where i != srcIdx AND i != tagsIdx:
    //        entry.attributes[std::string(labels[i])] = colData[i][r]
    //    payload[key] = entry
}
```

`columnAsString(pvxs::Value col, size_t r)` helper:
- Attempt `col.as<pvxs::shared_array<const std::string>>()[r]` — works for `TypeCode::String` columns
- On type mismatch (`std::exception`): extract element via `col[r]`, stream via `std::ostringstream << val`, return string

All columns in `ds-mock-data.jsonl` are strings, so the fallback path is only needed for future schema changes.

---

## MockDSServer

pvxs RPC server that answers requests on a configurable channel name (default `"ds"`) by returning an NTTable built from `ds-mock-data.jsonl`. Enables unit and integration tests without a live DS instance.

Supports **runtime mutation** of individual rows so tests can verify that updated or new metadata propagates end-to-end through the reader → bus → writer pipeline on subsequent scans.

Pattern: mirrors `test/mock/sioc.cpp` (uses `pvxs::server::Server`, `pvxs::server::SharedPV`).

### New files
```
test/mock/MockDSServer.h
test/mock/MockDSServer.cpp
```

### Column schema (all `TypeCode::String`)
`channelName`, `hostName`, `iocName`, `owner`, `pvStatus`, `recordType`, `recordDesc`, `archived`, `archiveRate`, `tags`

### Row type alias
```cpp
using DsRow = std::unordered_map<std::string, std::string>;
```

### Interface
```cpp
class MockDSServer {
public:
    // channel: pvAccess channel name served (default "ds")
    // jsonlPath: path to ds-mock-data.jsonl; empty = use 30-row built-in dataset
    explicit MockDSServer(std::string channel = "ds",
                          std::string jsonlPath = "");
    ~MockDSServer();  // stops server

    std::string channelName() const;

    // ── Mutation API (thread-safe; effective on next RPC call) ──────────────

    // Update one attribute of an existing row identified by channelName.
    // No-op (and logs warning) if channelName not found.
    // Example: updateAttribute("BPMS:IN20:221:X", "pvStatus", "Inactive")
    void updateAttribute(const std::string& channelName,
                         const std::string& column,
                         const std::string& value);

    // Replace the entire tags string for an existing row.
    // Example: updateTags("BPMS:IN20:221:X", "physics,bpm,fast,survey,new-tag")
    void updateTags(const std::string& channelName,
                    const std::string& tags);

    // Append a brand-new row (new PV) to the dataset.
    // Subsequent RPC responses will include this row.
    void addRow(DsRow row);

    // Remove a row by channelName. Subsequent RPC responses will omit it.
    // No-op if not found.
    void removeRow(const std::string& channelName);

    // Replace the entire dataset atomically.
    void setRows(std::vector<DsRow> rows);

    // Return a snapshot of current rows (for test assertions).
    std::vector<DsRow> rows() const;

    // Return current row count.
    size_t rowCount() const;

private:
    void loadRows(const std::string& jsonlPath);
    pvxs::Value buildNTTableResponse() const;  // caller holds m_mutex

    std::string          m_channel;
    pvxs::server::Server m_server;

    mutable std::mutex       m_mutex;  // guards m_rows
    std::vector<DsRow>       m_rows;

    static constexpr std::array<const char*, 10> kColumns = {
        "channelName","hostName","iocName","owner","pvStatus",
        "recordType","recordDesc","archived","archiveRate","tags"
    };
};
```

### NTTable structure (built per-response, not cached)
```cpp
pvxs::Value MockDSServer::buildNTTableResponse() const {
    // called under m_mutex
    nt::NTTable builder;
    for (const char* col : kColumns)
        builder.add_column(TypeCode::String, col);
    pvxs::Value val = builder.build().create();

    // populate labels
    pvxs::shared_array<std::string> labels(kColumns.size());
    for (size_t i = 0; i < kColumns.size(); ++i) labels[i] = kColumns[i];
    val["labels"] = labels.freeze();

    // populate each column array from m_rows
    for (const char* col : kColumns) {
        pvxs::shared_array<std::string> colArr(m_rows.size());
        for (size_t r = 0; r < m_rows.size(); ++r) {
            auto it = m_rows[r].find(col);
            colArr[r] = (it != m_rows[r].end()) ? it->second : "";
        }
        val[std::string("value.") + col] = colArr.freeze();
    }
    return val;
}
```

Building per-response (not caching) ensures mutations applied between scans are immediately visible without any cache invalidation logic.

### RPC handler
```cpp
m_server = pvxs::server::Config::fromEnv().build();
auto rpcPV = pvxs::server::SharedPV::buildMailbox();
rpcPV.onRPC([this](pvxs::server::SharedPV&,
                   std::unique_ptr<pvxs::server::ExecOp>&& op,
                   pvxs::Value&&) {
    std::lock_guard<std::mutex> lk(m_mutex);
    op->reply(buildNTTableResponse());
});
m_server.addPV(m_channel, rpcPV);
m_server.start();
```

### Built-in dataset
When `jsonlPath` is empty, `MockDSServer` uses the 30 rows from `ds-mock-data.jsonl` as a static `std::string_view` constant so tests have zero filesystem dependency.

---

## Controller Integration Test

**New file:** `test/controller/mldppvxs_controller_ds_metadata_integration_test.cpp`

Pattern: mirrors `mldppvxs_controller_mldp_writer_integration_test.cpp`.

### Test cases

**`DsMetadataReaderPushesPayloadToBus`**
- Start `MockDSServer` on channel `"test:ds"`
- Build config: `reader.type: epics-ds-metadata`, `service: "test:ds"`, `rescan-interval-sec: 0`, `tags-column: tags`
- Inject `MockDataBus` directly (no writer config needed)
- Construct `EpicsDSMetadataReader(bus, nullptr, cfg)`; wait up to 5 s for `bus->snapshot()` size ≥ 1
- Assert `isSourceMetadata(batch)` == true
- Assert `asSourceMetadata(batch).size()` == 30
- Assert `payload.at("VPIO:IN20:111:PRES").attributes.at("hostName")` == `"cpu-li20-vac1"`
- Assert `payload.at("BPMS:IN20:221:X").tags.value()` contains `"physics"` and `"bpm"`

**`DsMetadataFlowsToMLDPPVMetadataWriter`** (end-to-end with mock gRPC annotation service)
- Start `MockDSServer` + mock gRPC `DpAnnotationService` (capture `savePvMetadata` calls)
- Build YAML with reader (`epics-ds-metadata`, service `"test:ds"`) + writer (`mldp-pv-metadata`)
- Create and start `MLDPPVXSController`; wait for gRPC calls (up to 10 s)
- Assert each `channelName` from mock data appears in a `savePvMetadata` call
- Assert attributes `hostName`, `owner`, `recordType` forwarded correctly
- Assert `tags` list non-empty for rows with tags

**`DsMetadataRescanPeriodicRefetch`**
- Configure `rescan-interval-sec: 0.2`
- Use `MockDataBus`; wait for `snapshot().size() >= 2`
- Assert each push is a full 30-row `SourceMetadataPayload`

**`DsMetadataUpdatedAttributeReflectedOnRescan`**
- Configure `rescan-interval-sec: 0.2`; use `MockDataBus`
- Wait for first push; assert `payload.at("BPMS:IN20:221:X").attributes.at("pvStatus")` == `"Active"`
- Call `mockServer.updateAttribute("BPMS:IN20:221:X", "pvStatus", "Inactive")`
- Wait for second push; assert same key now has `"pvStatus"` == `"Inactive"`
- Verifies attribute changes in DS are picked up by the periodic reader and forwarded to bus

**`DsMetadataNewRowAppearsOnRescan`**
- Configure `rescan-interval-sec: 0.2`; use `MockDataBus`
- Wait for first push; assert payload size == 30; assert `payload.count("NEW:PV:TEST:X")` == 0
- Call `mockServer.addRow({{"channelName","NEW:PV:TEST:X"},{"hostName","cpu-test"},{"owner","test"},{"pvStatus","Active"},{"recordType","ai"},{"recordDesc","New test PV"},{"archived","false"},{"archiveRate","0"},{"tags","test"}})`
- Wait for second push; assert payload size == 31; assert `payload.count("NEW:PV:TEST:X")` == 1
- Verifies that a newly registered PV appears in MLDP on the next scan

**`DsMetadataRemovedRowAbsentOnRescan`**
- Configure `rescan-interval-sec: 0.2`; use `MockDataBus`
- Wait for first push; assert `payload.count("VPIO:IN20:111:PRES")` == 1
- Call `mockServer.removeRow("VPIO:IN20:111:PRES")`
- Wait for second push; assert payload size == 29; assert `payload.count("VPIO:IN20:111:PRES")` == 0
- Verifies decommissioned PVs no longer appear in subsequent metadata batches

**`DsMetadataTagsUpdateReflectedOnRescan`**
- Configure `rescan-interval-sec: 0.2`; use `MockDataBus`
- Wait for first push; assert `payload.at("BPMS:IN20:221:X").tags` contains `{"physics","bpm","fast","survey"}`
- Call `mockServer.updateTags("BPMS:IN20:221:X", "physics,bpm,fast,survey,golden")`
- Wait for second push; assert tags for same key now contains `"golden"` in addition to prior tags

---

## CMakeLists.txt Changes

Add to `lib${PROJECT_NAME}` sources block after line 505:
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.cpp"
```

Add to test sources block after line 647:
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/test/mock/MockDSServer.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/epics_ds/epics_ds_metadata_reader_config_test.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/epics_ds/epics_ds_metadata_reader_test.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/controller/mldppvxs_controller_ds_metadata_integration_test.cpp"
```

No new link-library entries needed — `pvxs` is already linked to `lib${PROJECT_NAME}`.

---

## Dependencies

Already present in the build:
- `pvxs` — `pvxs::client::Context`, `pvxs::Value`, `pvxs::shared_array`, `pvxs::nt::NTTable`, `pvxs::server::Server`, `pvxs::server::SharedPV`

Headers:
```cpp
#include <pvxs/client.h>   // client::Context, rpc()
#include <pvxs/data.h>     // Value, TypeDef, Member, shared_array
#include <pvxs/nt.h>       // nt::NTTable (mock server only)
#include <pvxs/server.h>   // server::Server, SharedPV (mock server only)
```

---

## Files Modified

| File | Change |
|---|---|
| `CMakeLists.txt` | +2 src entries, +4 test/mock entries |
| `include/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h` | new |
| `src/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.cpp` | new |
| `include/reader/impl/epics_ds/EpicsDSMetadataReader.h` | new |
| `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp` | new |
| `test/mock/MockDSServer.h` | new |
| `test/mock/MockDSServer.cpp` | new |
| `test/reader/impl/epics_ds/epics_ds_metadata_reader_config_test.cpp` | new |
| `test/reader/impl/epics_ds/epics_ds_metadata_reader_test.cpp` | new |
| `test/controller/mldppvxs_controller_ds_metadata_integration_test.cpp` | new |

---

## Verification

1. Config tests: defaults, required-field error, YAML round-trip
2. Reader unit test: inject `MockDSServer`, use `MockDataBus`, verify `push()` receives correct `SourceMetadataPayload`
3. Controller integration test — static path: `DsMetadataReaderPushesPayloadToBus`, `DsMetadataFlowsToMLDPPVMetadataWriter`
4. Controller integration test — mutation path: all five mutation test cases (attribute update, new row, remove row, tags update, periodic refetch)
5. Manual smoke: point at live `ds` instance, confirm payload keys match PV names and attributes contain expected columns
