# HDF5 File Availability Check — Design & Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or one of the available orchestrator plugin

**Goal:** Before `HDF5BsasGen1Reader` opens a file, detect whether it's still being written by an unknown external process. If the file is not usable within a configurable timeout, signal completion to the controller (triggering auto-close if last reader).

**Constraint:** We do NOT control the writer. We don't know who generates these files. Cannot use companion lock files, cannot require SWMR mode, cannot modify the producer.

---

## Chosen Strategy: Mtime/Size Stability + HDF5 Open Retry + Timeout

**Detection phase (pre-open):**
1. Stat the file repeatedly over a stability window.
2. If file size AND mtime stop changing for `stability_duration` seconds → file is likely complete.
3. If file keeps changing past `availability_timeout` → abort, notify controller.

**Open phase (post-stability):**
1. Attempt `H5::H5File(..., H5F_ACC_RDONLY)`.
2. On failure (HDF5 internal lock, permission, corruption), retry with exponential backoff.
3. After `max_retries` exhausted → abort, notify controller.

**Abort behavior:**
- Log error with reason (timeout vs open-failure).
- Call `signalCompleted()` → controller removes reader → if last reader, CLI exits.

---

## Why This Strategy

| Rejected | Reason |
|----------|--------|
| Companion lock file (`.lock`/`.writing`) | Can't modify writer, don't know producer |
| HDF5 SWMR | Requires writer to open in SWMR mode |
| Advisory flock/fcntl | Only works if writer also uses advisory locks — unknown |
| `lsof`/procfs scan | Linux-specific, expensive, fragile |

**What works without writer cooperation:**
- `stat()` is always available — mtime/size changes are observable externally
- HDF5 library's own internal file lock (HDF5 ≥1.10 uses `.lock` file) causes `H5Fopen` to fail if writer holds it — retry handles this naturally
- Timeout guarantees bounded wait — reader won't hang forever

---

## File Structure

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `include/reader/impl/hdf5_bsas_gen1/HDF5FileReadyChecker.h` | Stability checker class |
| Create | `src/reader/impl/hdf5_bsas_gen1/HDF5FileReadyChecker.cpp` | Implementation |
| Modify | `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h` | Add readiness-check config fields |
| Modify | `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.cpp` | Parse new YAML keys |
| Modify | `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp` | Integrate checker + retry in `readFile()` |
| Create | `test/reader/hdf5_file_ready_checker_test.cpp` | Unit tests |

---

### Task 1: Define HDF5FileReadyChecker

**Files:**
- Create: `include/reader/impl/hdf5_bsas_gen1/HDF5FileReadyChecker.h`

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

struct FileReadyConfig
{
    // How long file must be unchanged (size + mtime) to be considered stable
    std::chrono::seconds stability_duration{5};

    // How often to poll file stat during stability check
    std::chrono::seconds poll_interval{1};

    // Max time to wait for file to become stable before giving up
    std::chrono::seconds availability_timeout{60};

    // Retry on HDF5 open failure after file passes stability check
    int max_open_retries = 3;
    std::chrono::seconds initial_retry_delay{2};
    double retry_backoff_multiplier = 2.0;
};

enum class FileReadyStatus
{
    Ready,           // File stable and openable
    StillWriting,    // File size/mtime still changing (timeout reached)
    NotFound,        // File does not exist
    OpenFailed,      // File stable but HDF5 open fails after all retries
    Cancelled        // Cancelled via stop token
};

class HDF5FileReadyChecker
{
public:
    explicit HDF5FileReadyChecker(FileReadyConfig config = {});

    /// Block until file appears stable or timeout/cancellation.
    /// Does NOT open the file — only checks stat stability.
    FileReadyStatus waitUntilStable(
        const std::filesystem::path& filePath,
        const std::atomic<bool>& running) const;

private:
    struct FileStat
    {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};

        bool operator==(const FileStat& o) const
        {
            return size == o.size && mtime == o.mtime;
        }
        bool operator!=(const FileStat& o) const { return !(*this == o); }
    };

    FileStat statFile(const std::filesystem::path& filePath) const;

    FileReadyConfig config_;
};

} // namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1
```

- [ ] Step 1: Create header file
- [ ] Step 2: Verify compiles (header-only inclusion)

---

### Task 2: Implement HDF5FileReadyChecker

**Files:**
- Create: `src/reader/impl/hdf5_bsas_gen1/HDF5FileReadyChecker.cpp`

**Algorithm for `waitUntilStable()`:**

```
start_time = now()
prev_stat = statFile(path)
stable_since = now()

loop:
    if !running → return Cancelled
    if now() - start_time > availability_timeout → return StillWriting

    sleep(poll_interval)

    curr_stat = statFile(path)
    if curr_stat == error → return NotFound

    if curr_stat != prev_stat:
        // File changed — reset stability timer
        prev_stat = curr_stat
        stable_since = now()
    else:
        // File unchanged
        if now() - stable_since >= stability_duration:
            return Ready

    prev_stat = curr_stat
end loop
```

- [ ] Step 1: Write failing test — returns Ready for file that doesn't change
- [ ] Step 2: Write failing test — returns StillWriting when file keeps growing past timeout
- [ ] Step 3: Write failing test — returns NotFound for missing file
- [ ] Step 4: Write failing test — returns Cancelled when running set to false
- [ ] Step 5: Implement `waitUntilStable()` — make all tests pass
- [ ] Step 6: Commit

---

### Task 3: Add Config Fields to HDF5BsasGen1ReaderConfig

**Files:**
- Modify: `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h`
- Modify: `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.cpp`

New YAML fields:
```yaml
reader:
  - type: hdf5-bsas-gen1
    file-path: /data/bsas.h5
    readiness-check:
      enabled: true                    # default: true
      stability-duration-sec: 5        # default: 5
      poll-interval-sec: 1             # default: 1
      availability-timeout-sec: 60     # default: 60
      max-open-retries: 3              # default: 3
      retry-delay-sec: 2              # default: 2
      retry-backoff-multiplier: 2.0   # default: 2.0
```

Add to `HDF5BsasGen1ReaderConfig`:
```cpp
    bool readinessCheckEnabled() const { return readiness_check_enabled_; }
    const FileReadyConfig& readinessConfig() const { return readiness_config_; }

private:
    bool readiness_check_enabled_ = true;
    FileReadyConfig readiness_config_;
```

- [ ] Step 1: Add fields + accessors to config class
- [ ] Step 2: Parse new YAML keys in `parse()`
- [ ] Step 3: Unit test — config parses with readiness fields
- [ ] Step 4: Unit test — defaults applied when fields omitted
- [ ] Step 5: Commit

---

### Task 4: Integrate Checker + Retry into HDF5BsasGen1Reader::readFile()

**Files:**
- Modify: `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp`

New flow for `readFile()`:

```cpp
void HDF5BsasGen1Reader::readFile()
{
    // Phase 1: Wait for file to be stable (not being written)
    if (config_.readinessCheckEnabled())
    {
        HDF5FileReadyChecker checker(config_.readinessConfig());
        auto status = checker.waitUntilStable(config_.filePath(), running_);

        if (status == FileReadyStatus::Cancelled)
        {
            return; // Shutdown requested, no signal needed
        }

        if (status != FileReadyStatus::Ready)
        {
            if (logger_)
                logger_->log(util::log::Level::Error,
                    fmt::format("HDF5BsasGen1Reader: file '{}' not ready: {}",
                                config_.filePath(), statusToString(status)));
            signalCompleted(); // Notify controller — reader gives up
            return;
        }
    }

    // Phase 2: Open with retry + backoff
    H5::H5File file;
    {
        auto cfg = config_.readinessConfig();
        int attempts = 0;
        auto delay = cfg.initial_retry_delay;

        while (true)
        {
            try
            {
                file = H5::H5File(config_.filePath(), H5F_ACC_RDONLY);
                break; // Success
            }
            catch (const H5::FileIException& e)
            {
                attempts++;
                if (attempts >= cfg.max_open_retries || !running_.load())
                {
                    if (logger_)
                        logger_->log(util::log::Level::Error,
                            fmt::format("HDF5BsasGen1Reader: failed to open '{}' after {} attempts: {}",
                                        config_.filePath(), attempts, e.getCDetailMsg()));
                    signalCompleted(); // Notify controller — reader gives up
                    return;
                }

                if (logger_)
                    logger_->log(util::log::Level::Warn,
                        fmt::format("HDF5BsasGen1Reader: open attempt {}/{} failed, retry in {}s: {}",
                                    attempts, cfg.max_open_retries, delay.count(), e.getCDetailMsg()));

                std::this_thread::sleep_for(delay);
                delay = std::chrono::seconds(
                    static_cast<int>(delay.count() * cfg.retry_backoff_multiplier));
            }
        }
    }

    // Phase 3: Read data (existing logic, replace line 75's H5::H5File constructor)
    try
    {
        H5::Group dataGroup = file.openGroup(config_.groupName());
        // ... rest of existing read logic unchanged ...
    }
    catch (...)
    { /* existing error handling */ }

    // Phase 4: One-shot reader done — signal completion
    signalCompleted();
}
```

**Key behavior:**
- Timeout waiting for stability → `signalCompleted()` → controller notified → CLI can exit
- Open failure after retries → `signalCompleted()` → same
- Cancellation (`running_ = false`) → no signal (explicit shutdown, not natural completion)
- Successful read → `signalCompleted()` at end (one-shot reader done)

- [ ] Step 1: Write integration test — reader with growing file times out and signals completion
- [ ] Step 2: Write integration test — reader waits for file to stabilize then reads successfully
- [ ] Step 3: Write integration test — reader retries on open failure and eventually succeeds
- [ ] Step 4: Write integration test — reader signals completion on open failure exhaustion
- [ ] Step 5: Refactor `readFile()` with checker + retry + signalCompleted
- [ ] Step 6: Run all tests
- [ ] Step 7: Commit

---

### Task 5: Add signalCompleted() to Successful Read Path

Currently `HDF5BsasGen1Reader::readFile()` does NOT call `signalCompleted()` on success. As a one-shot reader, it must signal when done reading (regardless of success/failure).

- [ ] Step 1: Add `signalCompleted()` at end of `readFile()` after successful read
- [ ] Step 2: Write test — reader signals completion after successful file read
- [ ] Step 3: Ensure no double-signal on error paths (early returns already signal)
- [ ] Step 4: Commit

---

### Task 6: Full Integration & Regression Test

- [ ] Step 1: Build entire project
- [ ] Step 2: Run full test suite — no regressions
- [ ] Step 3: Verify scenarios:
  - File exists, stable → reads immediately, signals completion
  - File growing → waits → stabilizes → reads, signals completion
  - File growing → timeout → signals completion with error
  - File not found → signals completion with error
  - HDF5 open fails → retries → succeeds → reads, signals completion
  - HDF5 open fails → retries exhausted → signals completion with error
  - Shutdown requested during wait → no signal (external shutdown)
- [ ] Step 4: Final commit

---

## Design Notes

### Why Mtime + Size (Not Just Mtime)

Some filesystems update mtime on `open()` or `close()`, not just on `write()`. Checking both size AND mtime gives better confidence: if size stopped growing AND mtime stopped changing, the file is very likely complete.

### Poll Interval vs Stability Duration

- `poll_interval` (default 1s): how often we stat the file
- `stability_duration` (default 5s): how long size+mtime must be unchanged

A file that's written in bursts (e.g., HDF5 dataset flushes every few seconds) needs `stability_duration` longer than the burst gap. 5s default is conservative for typical BSAS acquisition.

### Why Signal on Timeout (Not Just Log)

If file never becomes available, reader is permanently stuck. By calling `signalCompleted()`, the reader participates in the auto-close lifecycle: controller removes it, and if all other readers are also done, CLI exits cleanly. Without this, a stuck reader would keep the process alive forever.

### Thread Safety

`waitUntilStable()` runs on the reader's worker thread. It checks `running_` (atomic bool) on each poll iteration. When controller calls `stop()` → `running_ = false` → checker exits promptly without calling `signalCompleted()` (since it's an external shutdown, not a natural completion).

### HDF5 Internal Locking (Bonus Protection)

HDF5 ≥1.10 creates a `.lock` file when opening for write. If the writer uses standard HDF5 APIs, our `H5F_ACC_RDONLY` open will fail while the lock exists. The retry mechanism handles this transparently — we don't need to check for `.lock` files ourselves; we just retry the open and let HDF5 library do its own lock checking.

Note: `HDF5FilePool.cpp:131` in this project disables file locking for the writer (`H5Pset_file_locking(..., false, true)`). This means our HDF5 writer doesn't create `.lock` files. But external/unknown writers might — the retry handles both cases.
