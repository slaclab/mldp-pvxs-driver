# Step 07 — Rename bus_* metrics to writer_* and extend to annotation writers

**Files:**
- `include/metrics/Metrics.h`
- `src/metrics/Metrics.cpp`
- `src/writer/mldp/MLDPWriter.cpp`
- `src/writer/mldp_configuration/MLDPConfigurationWriter.cpp`
- `src/writer/mldp_pv_metadata/MLDPPVMetadataWriter.cpp`
- `docs/dashboards/mldp-pvxs-driver.json`

**Depends on:** none (stand-alone refactor + extension)

**Rationale:**
The five `bus_*` metrics are owned exclusively by `MLDPWriter` — the controller never
touches them.  Renaming to `writer_*` clarifies ownership.
`MLDPConfigurationWriter` and `MLDPPVMetadataWriter` share the same `Metrics`
object but emit no push/failure counters today; wiring them up makes all three
MLDP writer classes observable with the same metric set.

---

## Part A — `include/metrics/Metrics.h`

### A1. Rename public method declarations (lines 96–104)

| Old | New |
|---|---|
| `incrementBusPushes` | `incrementWriterPushes` |
| `incrementBusFailures` | `incrementWriterFailures` |
| `incrementBusPayloadBytes` | `incrementWriterPayloadBytes` |
| `setBusPayloadBytesPerSecond` | `setWriterPayloadBytesPerSecond` |
| `incrementBusStreamRotations` | `incrementWriterStreamRotations` |
| `busPushTotal` | `writerPushTotal` |
| `busFailuresTotal` | `writerFailuresTotal` |
| `busPayloadBytesTotal` | `writerPayloadBytesTotal` |
| `busPayloadBytesPerSecond` | `writerPayloadBytesPerSecond` |

Replace the entire bus metrics block (lines 95–104):

```cpp
// Writer metrics ---------------------------------------------------------
void   incrementWriterPushes(double value = 1.0, prometheus::Labels tags = {});
void   incrementWriterFailures(double value = 1.0, prometheus::Labels tags = {});
void   incrementWriterPayloadBytes(double value, prometheus::Labels tags = {});
void   setWriterPayloadBytesPerSecond(double value, prometheus::Labels tags = {});
void   incrementWriterStreamRotations(double value = 1.0, prometheus::Labels tags = {});
double writerPushTotal(prometheus::Labels tags = {}) const;
double writerFailuresTotal(prometheus::Labels tags = {}) const;
double writerPayloadBytesTotal(prometheus::Labels tags = {}) const;
double writerPayloadBytesPerSecond(prometheus::Labels tags = {}) const;
```

### A2. Rename private family member variables (lines 135–139)

```cpp
prometheus::Family<prometheus::Counter>* writer_push_family_{nullptr};
prometheus::Family<prometheus::Counter>* writer_failure_family_{nullptr};
prometheus::Family<prometheus::Counter>* writer_payload_bytes_family_{nullptr};
prometheus::Family<prometheus::Gauge>*   writer_payload_bytes_per_second_family_{nullptr};
prometheus::Family<prometheus::Counter>* writer_stream_rotations_family_{nullptr};
```

---

## Part B — `src/metrics/Metrics.cpp`

### B1. Rename family construction (lines 85–89)

Replace with:

```cpp
writer_push_family_ = &makeCounterFamily(
    *registry_,
    "mldp_pvxs_driver_writer_push_total",
    "Number of events/requests pushed by MLDP writers.",
    clabels);
writer_failure_family_ = &makeCounterFamily(
    *registry_,
    "mldp_pvxs_driver_writer_failure_total",
    "Number of push/write failures reported by MLDP writers.",
    clabels);
writer_payload_bytes_family_ = &makeCounterFamily(
    *registry_,
    "mldp_pvxs_driver_writer_payload_bytes_total",
    "Total protobuf payload bytes written to the MLDP ingestion stream.",
    clabels);
writer_payload_bytes_per_second_family_ = &makeGaugeFamily(
    *registry_,
    "mldp_pvxs_driver_writer_payload_bytes_per_second",
    "Bytes/second for the most recent successful ingestion batch.",
    clabels);
writer_stream_rotations_family_ = &makeCounterFamily(
    *registry_,
    "mldp_pvxs_driver_writer_stream_rotations_total",
    "Number of gRPC ingestion stream open/close cycles by reason.",
    clabels);
```

### B2. Rename method implementations (lines 403–445)

Rename all five `Bus` → `Writer` in method names and swap `bus_*_family_` → `writer_*_family_`:

```cpp
void Metrics::incrementWriterPushes(double value, prometheus::Labels tags)
{
    writer_push_family_->Add(std::move(tags)).Increment(value);
}

void Metrics::incrementWriterFailures(double value, prometheus::Labels tags)
{
    writer_failure_family_->Add(std::move(tags)).Increment(value);
}

void Metrics::incrementWriterPayloadBytes(double value, prometheus::Labels tags)
{
    writer_payload_bytes_family_->Add(std::move(tags)).Increment(value);
}

void Metrics::setWriterPayloadBytesPerSecond(double value, prometheus::Labels tags)
{
    writer_payload_bytes_per_second_family_->Add(std::move(tags)).Set(value);
}

void Metrics::incrementWriterStreamRotations(double value, prometheus::Labels tags)
{
    writer_stream_rotations_family_->Add(std::move(tags)).Increment(value);
}

double Metrics::writerPushTotal(prometheus::Labels tags) const
{
    return writer_push_family_->Add(std::move(tags)).Value();
}

double Metrics::writerFailuresTotal(prometheus::Labels tags) const
{
    return writer_failure_family_->Add(std::move(tags)).Value();
}

double Metrics::writerPayloadBytesTotal(prometheus::Labels tags) const
{
    return writer_payload_bytes_family_->Add(std::move(tags)).Value();
}

double Metrics::writerPayloadBytesPerSecond(prometheus::Labels tags) const
{
    return writer_payload_bytes_per_second_family_->Add(std::move(tags)).Value();
}
```

---

## Part C — `src/writer/mldp/MLDPWriter.cpp` — update call sites

Rename every `incrementBus*` / `setBus*` call to `incrementWriter*` / `setWriter*`.
Add `{"writer", config_.name}` to each tag map so the stream writer is identifiable.

**All 9 call sites to update:**

| Line | Old call | New call |
|---|---|---|
| 190 | `m.incrementBusFailures(1.0, {{"source", rootSourceName}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", rootSourceName}})` |
| 262 | `m.incrementBusFailures(1.0, {{"source", "unknown"}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}})` |
| 271 | `m.incrementBusFailures(1.0, {{"source", "unknown"}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}})` |
| 276 | `m.incrementBusStreamRotations(1.0, {{"reason", reason}})` | `m.incrementWriterStreamRotations(1.0, {{"writer", config_.name}, {"reason", reason}})` |
| 302 | `m.incrementBusFailures(1.0, {{"source", "unknown"}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}})` |
| 317 | `m.incrementBusFailures(1.0, {{"source", "unknown"}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}})` |
| 423 | `m.incrementBusFailures(1.0, {{"source", item.root_source}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", item.root_source}})` |
| 435 | `m.incrementBusPushes(static_cast<double>(acceptedEvents), ...)` | `m.incrementWriterPushes(static_cast<double>(acceptedEvents), {{"writer", config_.name}, {"source", item.root_source}})` |
| 443 | `m.incrementBusPayloadBytes(...)` | `m.incrementWriterPayloadBytes(static_cast<double>(payloadBytes), {{"writer", config_.name}, {"source", item.root_source}})` |
| 453 | `m.setBusPayloadBytesPerSecond(bps, ...)` | `m.setWriterPayloadBytesPerSecond(bps, {{"writer", config_.name}, {"source", item.root_source}})` |
| 761 | `m.incrementBusFailures(1.0, {{"source", sourceName}})` | `m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", sourceName}})` |

---

## Part D — `src/writer/mldp_configuration/MLDPConfigurationWriter.cpp`

Wire `incrementWriterPushes` and `incrementWriterFailures` into
`doSaveConfiguration` and `doSaveConfigurationActivation`.

**D1. `doSaveConfiguration` (~line 201–215)**

After `const auto status = handle->stub->saveConfiguration(...)`, update:

```cpp
const auto status = handle->stub->saveConfiguration(&ctx, req, &resp);
if (!status.ok())
{
    errorf(*logger_,
           "MLDPConfigurationWriter saveConfiguration '{}': gRPC error {}: {}",
           cfg.configuration_name,
           static_cast<int>(status.error_code()),
           status.error_message());
    metric_call(metrics_, [&](auto& m) {
        m.incrementWriterFailures(1.0, {{"writer", config_.name}});
    });
    return;
}
metric_call(metrics_, [&](auto& m) {
    m.incrementWriterPushes(1.0, {{"writer", config_.name}});
});
```

The `catch` block also needs a failure counter:

```cpp
catch (const std::exception& ex)
{
    errorf(*logger_,
           "MLDPConfigurationWriter saveConfiguration '{}' exception: {}",
           cfg.configuration_name, ex.what());
    metric_call(metrics_, [&](auto& m) {
        m.incrementWriterFailures(1.0, {{"writer", config_.name}});
    });
}
```

**D2. `doSaveConfigurationActivation` (~line 269–283)**

Same pattern: increment failures on `!status.ok()` and on exception;
increment pushes on success.

```cpp
if (!status.ok())
{
    errorf(*logger_, ...);
    metric_call(metrics_, [&](auto& m) {
        m.incrementWriterFailures(1.0, {{"writer", config_.name}});
    });
    return;
}
metric_call(metrics_, [&](auto& m) {
    m.incrementWriterPushes(1.0, {{"writer", config_.name}});
});
// catch block:
metric_call(metrics_, [&](auto& m) {
    m.incrementWriterFailures(1.0, {{"writer", config_.name}});
});
```

**D3. Required include** — add to top of `.cpp` if not present:

```cpp
#include <metrics/Metrics.h>
```

(`metrics_` already stored as `std::shared_ptr<metrics::Metrics>` — no header change needed.)

---

## Part E — `src/writer/mldp_pv_metadata/MLDPPVMetadataWriter.cpp`

Wire same counters into `saveSourceMetadata` (~lines 207–221).

```cpp
const auto status = handle->stub->savePvMetadata(&ctx, req, &resp);
if (!status.ok())
{
    errorf(*logger_, ...);
    metric_call(metrics_, [&](auto& m) {
        m.incrementWriterFailures(1.0, {{"writer", config_.name}});
    });
    return;
}
metric_call(metrics_, [&](auto& m) {
    m.incrementWriterPushes(1.0, {{"writer", config_.name}});
});
// catch block:
metric_call(metrics_, [&](auto& m) {
    m.incrementWriterFailures(1.0, {{"writer", config_.name}});
});
```

**E1. Required include** — same as D3.

---

## Part F — `docs/dashboards/mldp-pvxs-driver.json`

### F1. Rename Prometheus metric names (global search-replace in file)

| Old name | New name |
|---|---|
| `mldp_pvxs_driver_bus_push_total` | `mldp_pvxs_driver_writer_push_total` |
| `mldp_pvxs_driver_bus_failure_total` | `mldp_pvxs_driver_writer_failure_total` |
| `mldp_pvxs_driver_bus_payload_bytes_total` | `mldp_pvxs_driver_writer_payload_bytes_total` |
| `mldp_pvxs_driver_bus_payload_bytes_per_second` | `mldp_pvxs_driver_writer_payload_bytes_per_second` |
| `mldp_pvxs_driver_bus_stream_rotations_total` | `mldp_pvxs_driver_writer_stream_rotations_total` |

### F2. Add `writer` label selector to impacted expressions

Update the 5 expressions in the "Bus" panels to include `writer=~"$writer"`:

```
sum(rate(mldp_pvxs_driver_writer_push_total{controller=~"$controller",writer=~"$writer"}[$__rate_interval])) by (controller, writer)
sum(rate(mldp_pvxs_driver_writer_failure_total{controller=~"$controller",writer=~"$writer"}[$__rate_interval])) by (controller, writer, source)
mldp_pvxs_driver_writer_payload_bytes_per_second{controller=~"$controller",writer=~"$writer"}
sum(rate(mldp_pvxs_driver_writer_payload_bytes_total{controller=~"$controller",writer=~"$writer"}[$__rate_interval])) by (controller, writer)
sum(rate(mldp_pvxs_driver_writer_stream_rotations_total{controller=~"$controller",writer=~"$writer"}[$__rate_interval])) by (controller, writer, reason)
```

### F3. Rename panel titles

| Old | New |
|---|---|
| `"MLDP Bus & Controller"` (row, line ~195) | `"MLDP Writer & Controller"` |
| `"Bus Push vs Failure Rate"` (line ~232) | `"Writer Push vs Failure Rate"` |
| `"Bus Payload Throughput"` (line ~262) | `"Writer Payload Throughput"` |
| `"Bus Stream Rotations by Reason"` (line ~352) | `"Writer Stream Rotations by Reason"` |

### F4. Add `$writer` template variable

Add after the existing `$controller` variable in the `"templating"` section:

```json
{
  "datasource": { "type": "prometheus", "uid": "${DS_PROMETHEUS}" },
  "definition": "label_values(mldp_pvxs_driver_writer_push_total{controller=~\"$controller\"}, writer)",
  "includeAll": true,
  "multi": true,
  "name": "writer",
  "query": {
    "query": "label_values(mldp_pvxs_driver_writer_push_total{controller=~\"$controller\"}, writer)",
    "refId": "StandardVariableQuery"
  },
  "label": "Writer",
  "refresh": 2,
  "sort": 1,
  "type": "query"
}
```

---

## Verify

```bash
cmake --build build 2>&1 | grep -i error
```

Scrape Prometheus endpoint and confirm:
- `mldp_pvxs_driver_bus_*` metrics no longer appear.
- `mldp_pvxs_driver_writer_push_total` appears with `writer` label from all three writers.
- `mldp_pvxs_driver_writer_failure_total` appears on gRPC errors from all three writers.
- `mldp_pvxs_driver_writer_payload_bytes_total` appears from `MLDPWriter` only (stream writer).

---

## Full metric set after steps 01–07

| Metric | Type | Writer | Measures |
|---|---|---|---|
| `writer_push_total` | Counter | MLDPWriter, ConfigWriter, PVMetaWriter | events/calls successfully pushed |
| `writer_failure_total` | Counter | MLDPWriter, ConfigWriter, PVMetaWriter | gRPC errors and exceptions |
| `writer_payload_bytes_total` | Counter | MLDPWriter only | protobuf encoded bytes, ingestion stream |
| `writer_payload_bytes_per_second` | Gauge | MLDPWriter only | encoded bps, most recent batch |
| `writer_stream_rotations_total` | Counter | MLDPWriter only | stream open/close cycles |
| `writer_data_bytes_total` | Counter | MLDPWriter (step 02+03) | raw DataBatch bytes pre-encoding |
| `writer_data_bytes_per_second` | Gauge | MLDPWriter (step 02+03) | raw bps |
| `reader_data_bytes_total` | Counter | PVXSReader, BaseReader (steps 04–06) | raw DataBatch bytes at ingestion |
| `reader_data_bytes_per_second` | Gauge | PVXSReader, BaseReader (steps 04–06) | reader bps per source |
