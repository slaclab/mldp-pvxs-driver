# Plan: EpicsDSMetadataReader

## Context

New reader class that queries the EPICS Directory Service (`ds`) via a single pvAccessCPP `ChannelRPC` call, parses the returned `NTTable`, and pushes `SourceMetadataPayload` records onto the `IDataBus`. This implements the fetch side described in `review-pv-metadata-fetcher.md` as a proper driver reader (not a standalone CLI), so the metadata flows through the existing writer pipeline to the MLDP annotation service.

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
test/reader/impl/epics_ds/
  epics_ds_metadata_reader_config_test.cpp
  epics_ds_metadata_reader_test.cpp
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
| `worker_thread_` | `std::thread` | single-shot fetch |
| `running_` | `atomic<bool>` | lifecycle flag |
| `worker_error_` | `exception_ptr` | diagnostics |

### Lifecycle
- Constructor: parses config, starts worker thread
- Destructor: sets `running_ = false`, signals `worker_cv_`, joins thread
- `name()`: returns `config_.name()`

### Members (additions)
| Member | Type | Purpose |
|---|---|---|
| `worker_cv_` | `std::condition_variable` | interruptible sleep between scans |
| `worker_mutex_` | `std::mutex` | guards `worker_cv_` |

### Worker thread (`runWorker()`)

```
ClientFactory::start()

do:
  provider = ChannelProviderRegistry::clients()->getProvider("pva")
  channel = provider->createChannel(config_.service(), DefaultChannelRequester::build(), PRIORITY_DEFAULT)

  Build NTURI:
    uri["scheme"] = "pva"
    uri["path"]   = config_.service()
    uri["query"]["name"] = config_.query()

  pvRequest = CreateRequest::create()->createRequest("")
  req = make_shared<RpcRequester>()
  rpc = channel->createChannelRPC(req, pvRequest)

  if req->waitConnect(timeout):
    rpc->lastRequest()
    rpc->request(uri)
    req->waitDone(timeout)
    if req->response():
      payload = parseNTTable(req->response())
      bus_->push(EventBatchStruct{ config_.name(), "", {}, payload })

  channel->destroy()
  epicsThreadSleep(0.1)

  if config_.rescanIntervalSec() <= 0.0: break   // single-shot

  // interruptible sleep: wakes early if running_ set to false
  unique_lock lk(worker_mutex_)
  worker_cv_.wait_for(lk, chrono::duration<double>(config_.rescanIntervalSec()),
                      [&]{ return !running_.load(); })

while running_.load()

ClientFactory::stop()
```

`rescan-interval-sec: 0` (default) → single fetch then exit.  
`rescan-interval-sec: N` → fetch, sleep N seconds (interruptible), fetch again until destructor sets `running_ = false` and signals `worker_cv_`.

### RpcRequester inner class

Implements `ChannelRPCRequester`. Two `epics::pvData::Event` semaphores gate connect and response.

| Callback | Action |
|---|---|
| `channelRPCConnect(status, rpc)` | `m_connected = status.isSuccess()`, signal `m_connEvent` |
| `requestDone(status, rpc, response)` | store `m_response`, `m_done = status.isSuccess()`, signal `m_doneEvent` |

Public: `waitConnect(double timeout) → bool`, `waitDone(double timeout) → bool`, `response() → PVStructure::shared_pointer`

### NTTable → SourceMetadataPayload parsing (`parseNTTable`)

Fully dynamic — no column names hardcoded. Based on `review-pv-metadata-fetcher.md` parsing idiom (lines 219–255).

```cpp
// 1. Get labels array
PVStringArrayPtr labelsField = result->getSubField<PVStringArray>("labels");
PVStringArray::const_svector labels; labelsField->get(labels);

// 2. Get value sub-structure fields (one PVScalarArray per column)
PVStructurePtr valueStruct = result->getSubField<PVStructure>("value");
PVFieldPtrArray fields = valueStruct->getPVFields();
size_t nrows = (fields.empty() ? 0 : fields[0]->getLength());

// 3. Locate configured columns by scanning labels
size_t srcIdx  = 0;     // source-name-column; fallback to col 0, warn if not found
size_t tagsIdx = npos;  // tags-column; npos = disabled / not found

for (size_t i = 0; i < labels.size(); ++i) {
    if (labels[i] == config_.sourceNameColumn()) srcIdx  = i;
    if (!config_.tagsColumn().empty() && labels[i] == config_.tagsColumn()) tagsIdx = i;
}
// warn if sourceNameColumn not matched by name (srcIdx stays 0)

// 4. For each row r:
//    key = stringValue(fields[srcIdx], r)
//    if tagsIdx != npos:
//        entry.tags = split(stringValue(fields[tagsIdx], r), ',')  // trim whitespace, skip empty tokens
//    for each column i where i != srcIdx AND i != tagsIdx:
//        entry.attributes[labels[i]] = stringValue(fields[i], r)
//    payload[key] = entry
```

`stringValue(field, r)` helper:
- If `field->getScalarArray()->getElementType() == pvString`: cast to `PVStringArray`, `get(sv)`, return `sv[r]`
- Otherwise: `col->dumpValue(oss, r)`, return `oss.str()`

Only `srcIdx` and `tagsIdx` columns are treated specially. Every other column → `attributes[label] = value`. No other column names are interpreted.

---

## CMakeLists.txt Changes

Add to `lib${PROJECT_NAME}` sources block after line 505:
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.cpp"
```

Add to test sources block after line 647:
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/epics_ds/epics_ds_metadata_reader_config_test.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/epics_ds/epics_ds_metadata_reader_test.cpp"
```

No new link-library entries needed — `epics-pvaccess` and `epics-pvdata` already linked to `lib${PROJECT_NAME}` (CMakeLists.txt line 538–542).

---

## Dependencies

Already present in the build:
- `epics-pvaccess` — `ChannelProvider`, `Channel`, `ChannelRPC`, `ChannelRPCRequester`, `ClientFactory`, `DefaultChannelRequester`, `CreateRequest`
- `epics-pvdata` — `PVStructure`, `PVString`, `PVStringArray`, `PVScalarArray`, `getFieldCreate()`, `getPVDataCreate()`, `Event`

Headers (per `review-pv-metadata-fetcher.md:88`):
```cpp
#include <pv/pvAccess.h>
#include <pv/pvData.h>
#include <pv/lock.h>
#include <pv/event.h>
```

---

## Files Modified

| File | Change |
|---|---|
| `CMakeLists.txt` | +2 src entries, +2 test entries |
| `include/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h` | new |
| `src/reader/impl/epics_ds/EpicsDSMetadataReaderConfig.cpp` | new |
| `include/reader/impl/epics_ds/EpicsDSMetadataReader.h` | new |
| `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp` | new |
| `test/reader/impl/epics_ds/epics_ds_metadata_reader_config_test.cpp` | new |
| `test/reader/impl/epics_ds/epics_ds_metadata_reader_test.cpp` | new |

---

## Verification

1. Config tests: defaults, required-field error, YAML round-trip
2. Reader unit test: mock `IDataBus`, inject pre-built `PVStructure` NTTable, verify `push()` receives correct `SourceMetadataPayload`
3. Integration smoke: connect to live `ds` instance, confirm payload keys match PV names and attributes contain expected columns
