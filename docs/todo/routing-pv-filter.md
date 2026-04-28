# TODO: Per-Route PV Name Filtering

## Problem

Routing controls which readers feed which writers, but has no intra-route filtering.
When a reader-writer route is active, every PV from that reader reaches that writer.
Operators need to narrow which signals a writer receives without splitting into separate readers.

## Goal

Allow each route entry to declare optional `include` and/or `exclude` glob patterns matched
against `EventBatch.root_source`. Batches that do not pass the filter are silently dropped
before writer dispatch. Routes with no patterns preserve existing all-pass behaviour. In routing "all' option is the default. so if no entry is pecified for a writer this mean that the write get all sorucercees from all. readers

## Requirements

- **Glob patterns** (`fnmatch`-style: `*`, `?`). No regex. No extra dependencies.
- **`include` list** — batch passes if `root_source` matches at least one pattern. Empty = accept all.
- **`exclude` list** — batch dropped if `root_source` matches any pattern. Applied after include.
- **Precedence**: include first, then exclude (exclude wins).
- **Backward compatible**: routes without `include`/`exclude` behave identically to today.
- **All-to-all routes** (no `routing:` block) unaffected.

## Proposed Configuration

```yaml
routing:
  mldp_main:
    from:
      - pvxs_reader_a
    include:                       # optional; glob patterns; default: accept all
      - "SYSTEM:SENSOR:*"
      - "LINAC:BPM:??"
    exclude:                       # optional; applied after include; default: exclude none
      - "SYSTEM:SENSOR:TEST:*"

  hdf5_local:
    from:
      - pvxs_reader_a
    # no include/exclude = pass all (unchanged behaviour)
```

Filter logic per batch:

```
pass = (include empty OR root_source matches any include pattern)
     AND (root_source matches NO exclude pattern)
```

## Implementation Plan

### Step 1 — Extend `RouteEntry` type

**File:** `include/controller/MLDPPVXSControllerConfig.h`

Replace `using RouteEntry = std::pair<std::string, std::vector<std::string>>` with:

```cpp
struct RouteFilterEntry {
    std::string              writer_name;
    std::vector<std::string> from_readers;
    std::vector<std::string> include_patterns; // empty = accept all
    std::vector<std::string> exclude_patterns; // empty = exclude none
};
```

Update `routeEntries()` return type and private member accordingly.

### Step 2 — Parse `include`/`exclude` in `parseRouting()`

**File:** `src/controller/MLDPPVXSControllerConfig.cpp`

After reading `from:` sequence, read optional `include:` and `exclude:` sequences and
populate `RouteFilterEntry.include_patterns` / `exclude_patterns`.

### Step 3 — Extend `RouteTable`

**File:** `include/controller/RouteTable.h`

- `WriterRoute` gains two new fields:
  ```cpp
  std::vector<std::string> include_patterns;
  std::vector<std::string> exclude_patterns;
  ```
- New public method:
  ```cpp
  bool acceptsSource(const std::string& writer_name,
                     const std::string& root_source) const noexcept;
  ```

**File:** `src/controller/RouteTable.cpp`

- `build()` signature updated to accept `vector<RouteFilterEntry>`.
  Copies patterns into `WriterRoute` during construction.
- `acceptsSource()` implementation using `fnmatch(3)` (POSIX; no extra dep):
  ```cpp
  bool RouteTable::acceptsSource(const std::string& writer_name,
                                 const std::string& root_source) const noexcept {
      auto it = table_.find(writer_name);
      if (it == table_.end()) return all_to_all_;
      const auto& route = it->second;
      if (route.accept_all) return true; // "all" sentinel
      if (!route.include_patterns.empty()) {
          bool hit = false;
          for (const auto& pat : route.include_patterns)
              if (fnmatch(pat.c_str(), root_source.c_str(), 0) == 0) { hit = true; break; }
          if (!hit) return false;
      }
      for (const auto& pat : route.exclude_patterns)
          if (fnmatch(pat.c_str(), root_source.c_str(), 0) == 0) return false;
      return true;
  }
  ```

### Step 4 — Apply filter in dispatch loop

**File:** `src/controller/MLDPPVXSController.cpp` (line ~186)

Add second guard after existing `accepts()` check:

```cpp
if (!route_table_.accepts(writers_[i]->name(), batch_values.reader_name))
    continue;
if (!route_table_.acceptsSource(writers_[i]->name(), batch_values.root_source))
    continue;
```

Two separate guards keep reader-routing and source-filtering orthogonal.

### Step 5 — Documentation

- **`README.md`**: add `include`/`exclude` keys to `routing:` YAML example with inline comments.
- **`docs/configuration.md`**: full filter key reference (type, default, semantics, glob syntax).

### Step 6 — Tests

Add unit tests in `test/controller/` (mirror existing `RouteTable` test style):

| Test case | Expected |
|-----------|----------|
| No patterns | all sources pass |
| Include match | source matching pattern passes |
| Include no-match | source not matching any include drops |
| Exclude match | source matching exclude drops even if include passes |
| Include + exclude overlap | exclude wins |
| Glob `*` wildcard | matches any suffix |
| Glob `?` wildcard | matches single character |
| `all` sentinel with patterns | patterns ignored (accept_all wins) |

## Open Questions

1. Should `fnmatch` use `FNM_PATHNAME` (no `/` in `*`) or default (bare glob)? EPICS PV names use `:` not `/`, so default (no `FNM_PATHNAME`) is correct — confirm.
2. Log dropped batches at `trace` level? Useful for debug but high volume in production.
3. Case sensitivity: `fnmatch` is case-sensitive by default on Linux. Document and keep as-is.

## Related

- `include/controller/RouteTable.h` — routing data structures
- `src/controller/MLDPPVXSController.cpp` — dispatch loop
- `include/util/bus/IDataBus.h` — `EventBatch.root_source` field
- `docs/todo/merge-root-sources-single-writer.md` — complementary routing feature
