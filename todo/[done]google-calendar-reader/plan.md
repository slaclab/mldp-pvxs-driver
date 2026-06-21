# SLAC Calendar Reader — Implementation Plan

## JSON schema differences per experiment

### LCLS (`lcls.json`) — full schema
```
url            string        unique event URL (Google Calendar link)
program_name   string        e.g. "CXI 1013443 Bain"
calendar       string        e.g. "NC-CXI", "NC-qRIXS"
start          ISO 8601+TZ   e.g. "2026-05-28T06:00:00-07:00"
end            ISO 8601+TZ
description    string        e.g. "Deliver to CXI"
note           string|null   e.g. "13.213 GeV, 80 pC"
details        string|null   raw HTML anchor tag with proposal URL
tags           array|null    e.g. ["2nd"]
poc            string|null   e.g. "Minitti"
config         string|null   e.g. "15 keV", "711 eV, 10 Hz"
machine        string|null   e.g. "NC"
hutch          object|null   {name, color, line, text_color}  — ALL lcls events have hutch
```

### FACET (`facet.json`) — reduced schema
```
url            string
program_name   string        e.g. "Single bunch matching S20"
calendar       string        e.g. "FACET-MD", "FACET-USER"
start          ISO 8601+TZ
end            ISO 8601+TZ   some events use "Z" suffix (UTC)
description    string        often empty ""
note           null          always null in sample
details        null          always null in sample
```
FACET events have NO: `tags`, `poc`, `config`, `machine`, `hutch`

**Implementation rule**: guard every optional field with null/missing check. Never assume LCLS schema for all experiments.

---

## Date/time strategy — Option B + bootstrap

### First iteration
- `start-date` **present** → `start = start-date (T00:00:00)`, `end = now()`
  - Bootstrap: fetches full history from configured date up to this moment
- `start-date` **absent**  → `start = now - lookback-days`, `end = now + lookahead-days`
  - Pure sliding window from the start

### All subsequent iterations (always sliding window)
- `start = now - lookback-days`
- `end = now + lookahead-days`

Tracked via `bool first_run_` (reset only on process restart — no persistence).

**Rationale**: bootstrap gets full history once on startup without unbounded growth on each rescan.  
Idempotency: `client_activation_id = event.url` — same event re-published on re-scan is deduplicated downstream.

---

## Configuration YAML

```yaml
readers:
  - type: slac-calendar
    name: slac-calendar-reader
    base-url: "https://aosd.slac.stanford.edu/program_calendar"
    experiments:
      - lcls
      - facet
    lookahead-days: 30          # end = now + this in sliding window (required, > 0)
    lookback-days: 1            # start = now - this in sliding window (default: 1)
    start-date: "2026-05-01"    # optional: YYYY-MM-DD, bootstrap first iter to now()
    rescan-interval-sec: 3600   # 0 = run once and exit (default: 0)
    connect-timeout-sec: 30     # CURL connect timeout (default: 30)
    total-timeout-sec: 60       # CURL total timeout (default: 60)
    tls-verify-peer: true       # default: true
    tls-verify-host: true       # default: true
    event-limit: 1000           # max events per experiment per call (default: 1000)
```

---

## Bus payload mapping

For **each calendar event**, emit **two** `EventBatchStruct` pushes in order.

### Push 1 — `ConfigurationPayload` (upsert the named configuration)

```
configuration_name   = event["program_name"]
category             = event["calendar"]                   // "FACET-MD", "NC-CXI", etc.
description          = event["description"]  if non-empty  // optional
tags                 = event["tags"]         if present and non-null  // optional
attributes:
  "experiment"       = <experiment string>                 // always: "lcls" or "facet"
  "note"             = event["note"]         if not null
  "details"          = inner text of HTML anchor in event["details"]  if not null
                       // e.g. <a href="...">https://pswww.slac.stanford.edu/...</a> → extract inner text URL
                       // parse with libxml2 HTML parser (htmlReadMemory + XPath or node walk)
                       // store clean URL string in attributes["details"]
  "poc"              = event["poc"]          if not null
  "config"           = event["config"]       if not null
  "machine"          = event["machine"]      if not null
  "hutch_name"       = event["hutch"]["name"]   if hutch not null
  "hutch_color"      = event["hutch"]["color"]  if hutch not null
  "hutch_line"       = event["hutch"]["line"]   if hutch not null
  // hutch.text_color dropped (no operational value)
  // tags also stored as individual attributes for downstream filtering:
  "tag_0"            = event["tags"][0]  if tags not null and len > 0
  "tag_1"            = event["tags"][1]  if tags not null and len > 1
  // ... up to N tags
```

### Push 2 — `ConfigurationActivationPayload` (record time window)

```
client_activation_id = event["url"]                        // idempotency key — unique per event
configuration_name   = event["program_name"]
start_time           = parseBusTimestamp(event["start"])
end_time             = parseBusTimestamp(event["end"])
description          = event["description"]  if non-empty  // optional
tags                 = event["tags"]         if present and non-null
attributes:
  "experiment"       = <experiment string>
  "calendar"         = event["calendar"]
```

### Push sequence per iteration

```
for each experiment in config.experiments:
    body = fetchExperiment(experiment, start_iso, end_iso)  // CURL GET
    events = parse JSON array
    for each event in events:
        push EventBatchStruct{ reader_name, payload = ConfigurationPayload }
        push EventBatchStruct{ reader_name, payload = ConfigurationActivationPayload }
```

---

## URL construction

```
GET {base-url}/{experiment}/events.json
    ?non_program_events=false
    &start_time={ISO8601}
    &end_time={ISO8601}
    &limit={event-limit}
```

Example: `https://aosd.slac.stanford.edu/program_calendar/lcls/events.json?non_program_events=false&start_time=2026-05-01T00:00:00-07:00&end_time=2026-05-28T17:30:00-07:00&limit=1000`

ISO 8601 format for query params: UTC offset of the machine running the reader (use `localtime`).

---

## ISO 8601 → BusTimestamp

JSON timestamps come in two formats:
- `2026-05-28T06:00:00-07:00`  (explicit TZ offset)
- `2026-05-28T23:00:00Z`       (UTC)

Use POSIX `strptime` + manual offset parse + `timegm`:

```cpp
BusTimestamp parseBusTimestamp(const std::string& iso8601);
// 1. strptime("%Y-%m-%dT%H:%M:%S", ...) → struct tm (UTC fields)
// 2. check for trailing 'Z' or '±HH:MM'
// 3. subtract offset seconds to get UTC epoch
// 4. return BusTimestamp{epoch_seconds, 0}
// throws std::runtime_error on parse failure → event skipped + warning log
```

---

## File layout

```
include/reader/impl/slac_calendar/
    SlacCalendarReaderConfig.h
    SlacCalendarReader.h

src/reader/impl/slac_calendar/
    SlacCalendarReaderConfig.cpp
    SlacCalendarReader.cpp

test/reader/impl/slac_calendar/
    slac_calendar_reader_config_test.cpp
    slac_calendar_reader_test.cpp

test/controller/
    mldppvxs_controller_slac_calendar_integration_test.cpp
```

---

## Class design

### `SlacCalendarReaderConfig`

Pattern mirrors `EpicsDSMetadataReaderConfig`.

```cpp
namespace mldp_pvxs_driver::reader::impl::slac_calendar {

class SlacCalendarReaderConfig {
public:
    struct Error : public std::runtime_error { using std::runtime_error::runtime_error; };
    explicit SlacCalendarReaderConfig(const config::Config& cfg);

    bool                            valid()              const noexcept;
    const std::string&              name()               const noexcept;
    const std::string&              baseUrl()            const noexcept;
    const std::vector<std::string>& experiments()        const noexcept;
    int                             lookaheadDays()      const noexcept;
    int                             lookbackDays()       const noexcept;
    const std::optional<std::string>& startDate()        const noexcept; // YYYY-MM-DD
    double                          rescanIntervalSec()  const noexcept;
    long                            connectTimeoutSec()  const noexcept;
    long                            totalTimeoutSec()    const noexcept;
    bool                            tlsVerifyPeer()      const noexcept;
    bool                            tlsVerifyHost()      const noexcept;
    int                             eventLimit()         const noexcept;

private:
    void parse(const config::Config& cfg);

    bool                     valid_{false};
    std::string              name_;
    std::string              base_url_;
    std::vector<std::string> experiments_;
    int                      lookahead_days_{30};
    int                      lookback_days_{1};
    std::optional<std::string> start_date_;
    double                   rescan_interval_sec_{0.0};
    long                     connect_timeout_sec_{30};
    long                     total_timeout_sec_{60};
    bool                     tls_verify_peer_{true};
    bool                     tls_verify_host_{true};
    int                      event_limit_{1000};
};

} // namespace
```

Validation:
- `name`, `base-url`, `experiments` (non-empty list): required
- `lookahead-days` > 0: required
- `lookback-days` >= 0: default 1
- `total-timeout-sec` >= `connect-timeout-sec`
- `start-date` if present: parseable `YYYY-MM-DD`

### `SlacCalendarReader`

Pattern mirrors `EpicsDSMetadataReader` (worker thread + interruptible CV sleep).

```cpp
class SlacCalendarReader final : public reader::Reader {
    REGISTER_READER("slac-calendar", SlacCalendarReader)
public:
    SlacCalendarReader(std::shared_ptr<util::bus::IDataBus>,
                       std::shared_ptr<metrics::Metrics>,
                       const config::Config&);
    ~SlacCalendarReader() override;
    std::string name() const override { return config_.name(); }

private:
    void        runWorker();
    void        fetchAndPublish(const std::string& startIso, const std::string& endIso);
    std::string fetchExperiment(const std::string& experiment,
                                const std::string& startIso,
                                const std::string& endIso);
    void        parseAndPush(const std::string& jsonBody, const std::string& experiment);
    void        pushEvent(const nlohmann::json& event, const std::string& experiment);
    BusTimestamp parseBusTimestamp(const std::string& iso8601);
    std::string  buildUrl(const std::string& experiment,
                          const std::string& startIso,
                          const std::string& endIso);
    std::string  nowOffsetIso(int offsetDays);           // now + offsetDays (negative = past)
    std::string  nowIso();                               // now() with no offset
    std::string  startDateToIso(const std::string& yyyymmdd); // "2026-05-01" → ISO 8601

    SlacCalendarReaderConfig             config_;
    std::shared_ptr<util::log::ILogger>  logger_;
    bool                                 first_run_{true};
    std::atomic<bool>                    running_{false};
    std::condition_variable              worker_cv_;
    std::mutex                           worker_mutex_;
    std::thread                          worker_thread_;
};
```

### `runWorker` pseudocode

```
loop:
    std::string start_iso, end_iso;

    if (first_run_ && config_.startDate().has_value()):
        start_iso = startDateToIso(*config_.startDate())  // YYYY-MM-DDT00:00:00±offset
        end_iso   = nowIso()                              // up to this moment only
    else:
        start_iso = nowOffsetIso(-config_.lookbackDays())
        end_iso   = nowOffsetIso(+config_.lookaheadDays())
    first_run_ = false

    fetchAndPublish(start_iso, end_iso)

    if config_.rescanIntervalSec() <= 0: break
    unique_lock lk(worker_mutex_)
    worker_cv_.wait_for(lk, rescan_interval_sec_, [this]{ return !running_; })

while (running_)
```

---

## CMakeLists.txt changes

After line 508 (after `EpicsDSMetadataReaderConfig.cpp`):
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/slac_calendar/SlacCalendarReader.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/slac_calendar/SlacCalendarReaderConfig.cpp"
```

After line 653 (after `epics_ds_metadata_reader_test.cpp`):
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/slac_calendar/slac_calendar_reader_config_test.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/slac_calendar/slac_calendar_reader_test.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/test/controller/mldppvxs_controller_slac_calendar_integration_test.cpp"
```

---

## Test plan

### `slac_calendar_reader_config_test.cpp` — unit tests

| Test | Verifies |
|------|----------|
| `DefaultValues` | minimal config (name+base-url+experiments+lookahead-days) yields correct defaults |
| `AllFieldsParsed` | all optional fields parse correctly |
| `MissingNameThrows` | `Error` on missing `name` |
| `MissingBaseUrlThrows` | `Error` on missing `base-url` |
| `EmptyExperimentsThrows` | `Error` when `experiments` list is empty |
| `MissingLookaheadDaysThrows` | `Error` when `lookahead-days` absent |
| `ZeroLookaheadDaysThrows` | `Error` when `lookahead-days` = 0 |
| `NegativeLookaheadDaysThrows` | `Error` when `lookahead-days` < 0 |
| `NegativeLookbackDaysThrows` | `Error` when `lookback-days` < 0 |
| `TotalTimeoutLessThanConnectThrows` | `Error` when `total < connect` |
| `InvalidStartDateFormatThrows` | `Error` when `start-date` not `YYYY-MM-DD` |
| `ValidStartDateParsed` | `start-date` stored as string, `startDate()` returns value |
| `StartDateAbsentIsNullopt` | `startDate()` returns `std::nullopt` when not configured |
| `MultipleExperimentsParsed` | both `lcls` and `facet` in vector |
| `TlsDefaultsTrue` | `tlsVerifyPeer/Host` default true |
| `TlsDisableRoundtrip` | both flags can be set false |

### `slac_calendar_reader_test.cpp` — unit tests (mock HTTP)

Uses `MockCalendarHttpServer` (in-process HTTP server serving canned JSON, pattern from `MockArchiverPbHttpServer`).

| Test | Verifies |
|------|----------|
| `LclsEventMapsToConfigurationPayload` | program_name→configuration_name, calendar→category, description, tags, hutch_* attributes |
| `LclsEventMapsToActivationPayload` | url→client_activation_id, start/end→BusTimestamp, experiment attribute |
| `FacetEventMissingOptionalFields` | note/poc/config/machine/hutch absent → not in attributes; no panic |
| `FacetEmptyDescriptionSkipped` | empty `description` → `ConfigurationPayload.description` is nullopt |
| `EventWithTagsPopulated` | `tags` array → `ConfigurationPayload.tags` and `ActivationPayload.tags` |
| `EventWithNullTagsSkipped` | null `tags` → optional absent |
| `TwoPayloadsEmittedPerEvent` | for N events: bus receives exactly 2N pushes, alternating Config then Activation |
| `PushOrderCorrect` | for each event: ConfigurationPayload push precedes ConfigurationActivationPayload push |
| `MultipleExperimentsAllPushed` | two experiments configured → both fetched, all events pushed |
| `ParseBusTimestampNegativeOffset` | `2026-05-28T06:00:00-07:00` → correct UTC epoch |
| `ParseBusTimestampZSuffix` | `2026-05-28T23:00:00Z` → correct UTC epoch |
| `ParseBusTimestampPositiveOffset` | e.g. `+05:30` → correct UTC epoch |
| `ParseBusTimestampInvalidThrows` | malformed string → event skipped + warning logged (no crash) |
| `HttpErrorSkipsExperiment` | non-200 response for one experiment → other experiment still processed |
| `EmptyJsonArrayNoPayloads` | `[]` response → zero bus pushes |
| `SingleShotExitsAfterOneFetch` | `rescan-interval-sec: 0` → worker exits, no second fetch |
| `RescanFetchesAgain` | `rescan-interval-sec: 0.1` → at least 2 fetches within 500ms |
| `FirstRunWithStartDateUsesNowAsEnd` | `start-date` present → fetch called with start=configured_date, end≈now (not now+lookahead) |
| `SubsequentRunIgnoresStartDate` | second iter always uses sliding window regardless of `start-date` |
| `FirstRunWithoutStartDateUsesSlidingWindow` | no `start-date` → start=now-lookback, end=now+lookahead on first iter too |
| `ReaderNameSetCorrectly` | `EventBatchStruct.reader_name == config_.name()` |
| `ExperimentAttributeAlwaysSet` | `attributes["experiment"]` present in both payload types for every event |

### `mldppvxs_controller_slac_calendar_integration_test.cpp` — integration tests

Wires: `MockCalendarHttpServer` → `SlacCalendarReader` → forwarding bus → `MLDPPVMetadataWriter` → fake gRPC annotation server (reuse `TestAnnotationSvc` pattern from `mldppvxs_controller_ds_metadata_writer_integration_test.cpp`).

| Test | Verifies |
|------|----------|
| `LclsEventConfigurationSaved` | LCLS event program_name saved as configuration via gRPC `saveConfiguration` |
| `LclsEventActivationSaved` | LCLS event activation saved with correct start/end epoch via `saveConfigurationActivation` |
| `FacetEventSavedWithoutOptionalFields` | FACET event saved; attributes contain only `experiment` and `calendar`, no hutch/poc/config keys |
| `MultipleEventsAllSaved` | N events in response → N saveConfiguration + N saveConfigurationActivation calls |
| `MultipleExperimentsAllSaved` | two experiments configured → events from both stored |
| `RescanPublishesUpdatedEvents` | after first scan, server returns new event → second scan stores it |
| `IdempotentRescan` | same events returned twice → `client_activation_id` allows downstream dedup (writer called again — test verifies no crash and call count increases) |
| `StartDateBootstrap` | `start-date` present → first fetch uses start=start-date, end≈now (verify URL params via MockCalendarHttpServer request log) |
| `SlidingWindowAfterBootstrap` | second fetch uses sliding window, not start-date again |
| `HttpFailureDoesNotCrash` | server returns 500 for one experiment → no crash, other experiment processed |
| `ShutdownClean` | reader + writer destroyed mid-rescan → no crash, thread joins |

---

## Implementation steps (ordered)

1. `SlacCalendarReaderConfig` — header + impl (`SlacCalendarReaderConfig.h/.cpp`)
2. `slac_calendar_reader_config_test.cpp` — config unit tests
3. `MockCalendarHttpServer` — in-process HTTP server for testing (in `test/mock/`)
4. `SlacCalendarReader` header + skeleton (ctor/dtor, thread lifecycle, `name()`)
5. `parseBusTimestamp`, `nowOffsetIso`, `nowIso`, `startDateToIso`, `buildUrl` helpers
6. `fetchExperiment` — CURL GET with TLS/timeout config (mirror `EpicsArchiverReader`)
7. `pushEvent` — JSON → `ConfigurationPayload` push, JSON → `ConfigurationActivationPayload` push
8. `parseAndPush` — iterate array, call `pushEvent`, skip+warn on error
9. `runWorker` — date logic + bootstrap + interruptible sleep
10. `slac_calendar_reader_test.cpp` — all unit tests
11. CMakeLists.txt — register sources + tests
12. `mldppvxs_controller_slac_calendar_integration_test.cpp` — integration tests

---

## Open questions (decide before step 6)

- **`nlohmann/json` in tree?** Grep CMakeLists — expected yes (used elsewhere). If not, add dependency.
- **`details` HTML content** — store raw in `attributes["details"]` (simple) or strip HTML tags (clean)? Suggest raw for now.
- **`hutch.text_color`** — drop (no operational value). Confirm.
- **`non_program_events` param** — hardcoded `false` per requirement. Confirm.
