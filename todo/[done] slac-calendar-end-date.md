# Add `end-date` to SLAC Calendar Reader

## Context

Reader supports `start-date` for first-run fetch (start-date → now), then switches to lookback/lookahead. Need `end-date` so reader does a **one-shot** fetch of a fixed window (`start-date` → `end-date`) and stops — useful for importing historical experiment schedules.

## Changes

### 1. Config key + parsing — `src/reader/impl/slac_calendar/SlacCalendarReaderConfig.cpp`

- Add `kEndDateKey = "end-date"` (line ~23, next to `kStartDateKey`)
- Parse same way: optional, YYYY-MM-DD regex, stored in `end_date_`
- Validation:
  - `end-date` without `start-date` → throw
  - `end-date` < `start-date` → throw
  - `end-date` set AND any of `lookahead-days`, `lookback-days`, `rescan-interval-sec` present → throw error (these are mutually exclusive with fixed-window mode)

### 2. Config header — `include/reader/impl/slac_calendar/SlacCalendarReaderConfig.h`

- Add `std::optional<std::string> end_date_` member (next to `start_date_` ~line 56)
- Add `endDate()` const noexcept accessor (next to `startDate()` ~line 39)

### 3. Reader logic — `src/reader/impl/slac_calendar/SlacCalendarReader.cpp` `runWorker()` (lines 71-94)

Current first-run logic:
```cpp
if (first_run_.load() && config_.startDate().has_value()) {
    start_iso = startDateToIso(*config_.startDate());
    end_iso   = nowIso();
}
```

New:
```cpp
if (first_run_.load() && config_.startDate().has_value()) {
    start_iso = startDateToIso(*config_.startDate());
    end_iso   = config_.endDate().has_value()
              ? startDateToIso(*config_.endDate())
              : nowIso();
}
```

Force one-shot when both dates set (after fetch, lines 90-94):
```cpp
if ((config_.startDate() && config_.endDate()) || config_.rescanIntervalSec() <= 0.0) {
    signalCompleted();
    break;
}
```

Reuse existing `startDateToIso()` — sets time to 00:00:00 local. For end-date the API uses `end_time` as exclusive upper bound so beginning-of-day is fine (covers all events that end before that date).

### 4. Tests — `test/reader/impl/slac_calendar/slac_calendar_reader_config_test.cpp`

- `end-date` parsed correctly
- `end-date` without `start-date` throws
- `end-date` < `start-date` throws

## Example config

```yaml
reader:
  slac-calendar:
    - name: slac_calendar_import
      base-url: "https://host.docker.internal/program_calendar"
      experiments:
        - "lcls"
      start-date: "2024-01-01"
      end-date: "2025-01-01"
      lookahead-days: 7        # throw error when both dates set
      lookback-days: 1         # throw error when both dates set
      rescan-interval-sec: 300 # throw error when both dates set (one-shot)
```

## Verification

- Build in devcontainer: `cmake --build build`
- Run: `ctest --test-dir build -R slac_calendar`
- Manual: config with both dates → verify one-shot + correct URL time params in trace logs
