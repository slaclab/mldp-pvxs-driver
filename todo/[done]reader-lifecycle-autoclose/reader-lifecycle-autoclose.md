# Reader Lifecycle & Auto-Close Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or one of hte available orchestrator plugin

**Goal:** Controller auto-shuts-down when all readers signal completion, enabling one-shot job usage (import data then quit).

**Architecture:** New `IReaderLifecycle` interface with `onReaderCompleted()`. Reader base gains `signalCompleted()` helper. Controller implements the interface: posts async removal of completed readers, triggers graceful `stop()` when list empties. Long-running readers (EPICS subscriptions, archiver periodic_tail) never signal — so controller stays alive. One-shot readers (archiver historical_once, calendar/metadata with rescan=0) signal on exit.

**Tech Stack:** C++20, GoogleTest, std::mutex for thread-safe reader removal, BS::light_thread_pool for async removal dispatch.

---

## File Structure

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `include/reader/IReaderLifecycle.h` | Interface: `onReaderCompleted(string)` |
| Modify | `include/reader/IReader.h` | Add `setLifecycleObserver()` + protected `signalCompleted()` |
| Modify | `include/controller/MLDPPVXSController.h` | Implement `IReaderLifecycle`, add `readers_mutex_`, async removal |
| Modify | `src/controller/MLDPPVXSController.cpp` | Wire lifecycle observer after creating readers, implement `onReaderCompleted()` |
| Modify | `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` | Signal completion in `HistoricalOnce` mode only |
| Modify | `src/reader/impl/slac_calendar/SlacCalendarReader.cpp` | Signal completion when one-shot (rescan ≤ 0) |
| Modify | `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp` | Signal completion when one-shot (rescan ≤ 0) |
| Create | `test/reader/reader_lifecycle_test.cpp` | Unit tests for lifecycle signaling + controller auto-close |
| Modify | `test/mock/MockDataBus.h` | (no change needed — lifecycle is separate from bus) |

---

### Task 1: Create IReaderLifecycle Interface

**Files:**
- Create: `include/reader/IReaderLifecycle.h`

- [ ] **Step 1: Create the interface header**

```cpp
//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>

namespace mldp_pvxs_driver::reader {

class IReaderLifecycle
{
public:
    virtual ~IReaderLifecycle() = default;

    virtual void onReaderCompleted(const std::string& reader_name) = 0;
};

} // namespace mldp_pvxs_driver::reader
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build --target mldp-pvxs-driver 2>&1 | head -20`
Expected: No errors (header not yet included anywhere)

- [ ] **Step 3: Commit**

```bash
git add include/reader/IReaderLifecycle.h
git commit -m "feat: add IReaderLifecycle interface for reader completion signaling"
```

---

### Task 2: Add Lifecycle Observer to Reader Base Class

**Files:**
- Modify: `include/reader/IReader.h`

- [ ] **Step 1: Write the failing test**

Create `test/reader/reader_lifecycle_test.cpp`:

```cpp
//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/IReader.h>
#include <reader/IReaderLifecycle.h>
#include <util/bus/IDataBus.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

namespace mldp_pvxs_driver::test {

class MockLifecycleObserver : public reader::IReaderLifecycle,
                              public std::enable_shared_from_this<MockLifecycleObserver>
{
public:
    void onReaderCompleted(const std::string& reader_name) override
    {
        last_completed_name_ = reader_name;
        completed_count_.fetch_add(1);
    }

    std::string lastCompletedName() const { return last_completed_name_; }
    int         completedCount() const { return completed_count_.load(); }

private:
    std::string      last_completed_name_;
    std::atomic<int> completed_count_{0};
};

class TestReader : public reader::Reader
{
public:
    using reader::Reader::Reader;
    std::string name() const override { return "test-reader"; }

    void simulateWorkDone() { signalCompleted(); }
};

class ReaderLifecycleTest : public ::testing::Test
{
protected:
    std::shared_ptr<MockLifecycleObserver> observer_ = std::make_shared<MockLifecycleObserver>();
};

TEST_F(ReaderLifecycleTest, SignalCompletedNotifiesObserver)
{
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    reader->setLifecycleObserver(observer_);

    reader->simulateWorkDone();

    EXPECT_EQ(observer_->completedCount(), 1);
    EXPECT_EQ(observer_->lastCompletedName(), "test-reader");
}

TEST_F(ReaderLifecycleTest, SignalCompletedWithoutObserverDoesNotCrash)
{
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    // No observer set
    reader->simulateWorkDone(); // must not crash
}

TEST_F(ReaderLifecycleTest, SignalCompletedWithExpiredObserverDoesNotCrash)
{
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    {
        auto temp_observer = std::make_shared<MockLifecycleObserver>();
        reader->setLifecycleObserver(temp_observer);
    }
    // Observer destroyed - weak_ptr expired
    reader->simulateWorkDone(); // must not crash
}

} // namespace mldp_pvxs_driver::test
```

- [ ] **Step 2: Add test to CMake**

Add to the appropriate `CMakeLists.txt` in `test/` directory (follow existing pattern for reader tests):

```cmake
add_executable(reader_lifecycle_test
    reader/reader_lifecycle_test.cpp
)
target_link_libraries(reader_lifecycle_test
    PRIVATE
        mldp-pvxs-driver
        GTest::gtest_main
)
add_test(NAME reader_lifecycle_test COMMAND reader_lifecycle_test)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build --target reader_lifecycle_test 2>&1 | tail -20`
Expected: Compilation error — `setLifecycleObserver` and `signalCompleted` do not exist

- [ ] **Step 4: Implement lifecycle support in Reader base**

Modify `include/reader/IReader.h`:

Add forward declaration and include at top:
```cpp
#include <reader/IReaderLifecycle.h>
```

Add public method and protected helper + member to class `Reader`:

```cpp
class Reader
{
public:
    Reader(std::shared_ptr<util::bus::IDataBus> bus,
           std::shared_ptr<metrics::Metrics>    metrics = nullptr)
        : bus_(std::move(bus))
        , metrics_(std::move(metrics)) {}

    virtual ~Reader() = default;

    virtual std::string name() const = 0;

    void setLifecycleObserver(std::weak_ptr<IReaderLifecycle> observer)
    {
        lifecycle_ = std::move(observer);
    }

protected:
    void signalCompleted()
    {
        if (auto obs = lifecycle_.lock())
        {
            obs->onReaderCompleted(name());
        }
    }

    std::shared_ptr<util::bus::IDataBus> bus_;
    std::shared_ptr<metrics::Metrics>    metrics_;

private:
    std::weak_ptr<IReaderLifecycle> lifecycle_;
};
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R reader_lifecycle_test -V`
Expected: All 3 tests PASS

- [ ] **Step 6: Commit**

```bash
git add include/reader/IReader.h test/reader/reader_lifecycle_test.cpp test/CMakeLists.txt
git commit -m "feat: add lifecycle observer to Reader base class with signalCompleted()"
```

---

### Task 3: Controller Implements IReaderLifecycle

**Files:**
- Modify: `include/controller/MLDPPVXSController.h`
- Modify: `src/controller/MLDPPVXSController.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/reader/reader_lifecycle_test.cpp`:

```cpp
#include <controller/MLDPPVXSController.h>
#include <config/Config.h>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace mldp_pvxs_driver::test {

class ControllerAutoCloseTest : public ::testing::Test
{
protected:
    // Minimal config with a fake reader type for testing
    // We'll test through the interface directly
};

TEST_F(ControllerAutoCloseTest, ControllerImplementsIReaderLifecycle)
{
    // Verify controller can be used as IReaderLifecycle
    // This is a compile-time check: shared_ptr<MLDPPVXSController> converts to
    // shared_ptr<IReaderLifecycle>
    static_assert(std::is_base_of_v<reader::IReaderLifecycle,
                                     controller::MLDPPVXSController>,
                  "Controller must implement IReaderLifecycle");
}

} // namespace mldp_pvxs_driver::test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target reader_lifecycle_test 2>&1 | tail -10`
Expected: static_assert failure — MLDPPVXSController does not inherit IReaderLifecycle

- [ ] **Step 3: Add IReaderLifecycle inheritance to controller header**

Modify `include/controller/MLDPPVXSController.h`:

Add include:
```cpp
#include <reader/IReaderLifecycle.h>
```

Change class declaration:
```cpp
class MLDPPVXSController : public util::bus::IDataBus,
                           public reader::IReaderLifecycle,
                           public std::enable_shared_from_this<MLDPPVXSController>
```

Add to private section:
```cpp
    mutable std::mutex readers_mutex_;

    void onReaderCompleted(const std::string& reader_name) override;
```

- [ ] **Step 4: Implement onReaderCompleted in controller**

Add to `src/controller/MLDPPVXSController.cpp`:

```cpp
void MLDPPVXSController::onReaderCompleted(const std::string& reader_name)
{
    if (!running_.load())
    {
        return;
    }

    infof(*logger_, "Reader '{}' signaled completion", reader_name);

    // Post async removal to avoid deadlock: reader destructor joins worker thread,
    // but this callback is invoked FROM the worker thread.
    thread_pool_->detach_task(
        [this, reader_name]()
        {
            {
                std::lock_guard<std::mutex> lock(readers_mutex_);
                auto it = std::remove_if(readers_.begin(), readers_.end(),
                                         [&](const reader::ReaderUPtr& r)
                                         { return r->name() == reader_name; });
                readers_.erase(it, readers_.end());
                infof(*logger_, "Removed completed reader '{}'. Remaining readers: {}",
                      reader_name, readers_.size());
            }

            // Check if all readers are done
            std::lock_guard<std::mutex> lock(readers_mutex_);
            if (readers_.empty())
            {
                infof(*logger_, "All readers completed — initiating graceful shutdown");
                stop();
            }
        });
}
```

- [ ] **Step 5: Wire lifecycle observer to readers after creation**

In `MLDPPVXSController::start()`, after the reader creation loop (line ~306), add:

```cpp
    // Wire lifecycle observer so readers can signal completion
    for (auto& reader : readers_)
    {
        reader->setLifecycleObserver(
            std::dynamic_pointer_cast<reader::IReaderLifecycle>(shared_from_this()));
    }
```

- [ ] **Step 6: Add readers_mutex_ protection to push() route validation**

In `MLDPPVXSController::push()`, the function doesn't access `readers_` directly (it routes to writers/processors). No mutex needed in push path. But `stop()` calls `readers_.clear()` — add mutex there:

In `MLDPPVXSController::stop()`, wrap readers_.clear():
```cpp
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        readers_.clear();
    }
```

- [ ] **Step 7: Run test to verify it passes**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R reader_lifecycle_test -V`
Expected: All tests PASS (including static_assert)

- [ ] **Step 8: Run full test suite to check no regressions**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: All existing tests still pass

- [ ] **Step 9: Commit**

```bash
git add include/controller/MLDPPVXSController.h src/controller/MLDPPVXSController.cpp
git commit -m "feat: controller implements IReaderLifecycle with async reader removal and auto-shutdown"
```

---

### Task 4: EpicsArchiverReader Signals Completion in HistoricalOnce Mode

**Files:**
- Modify: `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/reader/reader_lifecycle_test.cpp`:

```cpp
#include <reader/impl/epics_archiver/EpicsArchiverReader.h>
#include <test/mock/MockDataBus.h>

#include <condition_variable>

namespace mldp_pvxs_driver::test {

class ArchiverLifecycleTest : public ::testing::Test
{
protected:
    std::shared_ptr<MockLifecycleObserver> observer_ = std::make_shared<MockLifecycleObserver>();

    bool waitForCompletion(int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (observer_->completedCount() == 0)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
};

TEST_F(ArchiverLifecycleTest, HistoricalOnceSignalsCompletion)
{
    // Build minimal config for historical_once mode with an unreachable host
    // so fetchConfiguredPVs() fails fast but worker still exits cleanly.
    // The reader should still signal completion regardless of fetch success.
    boost::property_tree::ptree pt;
    pt.put("name", "test-archiver");
    pt.put("hostname", "localhost:19999");  // unreachable
    pt.put("mode", "historical_once");
    pt.put("start-date", "2020-01-01T00:00:00Z");
    pt.put("end-date", "2020-01-01T00:01:00Z");
    pt.put("connect-timeout-sec", "1");
    pt.put("total-timeout-sec", "2");

    boost::property_tree::ptree pv_node;
    pv_node.put("name", "TEST:PV:1");
    pt.add_child("pvs.", pv_node);

    auto bus = std::make_shared<mock::MockDataBus>();
    config::Config cfg(pt);

    auto reader = std::make_unique<reader::impl::epics_archiver::EpicsArchiverReader>(
        bus, nullptr, cfg);
    reader->setLifecycleObserver(observer_);

    // Worker runs in constructor; wait for it to signal completion
    ASSERT_TRUE(waitForCompletion()) << "Archiver historical_once did not signal completion";
    EXPECT_EQ(observer_->lastCompletedName(), "test-archiver");
}

} // namespace mldp_pvxs_driver::test
```

**Note:** The exact config construction pattern may need adjustment based on how `Config` wraps property_tree in this project. Adapt to match the actual Config constructor pattern used in existing archiver tests.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R reader_lifecycle_test -R ArchiverLifecycle -V`
Expected: FAIL — observer never notified (completedCount stays 0)

- [ ] **Step 3: Add signalCompleted() call to EpicsArchiverReader::runWorker()**

Modify `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` in `runWorker()`:

After the try/catch block, just before the final `worker_done_ = true` block (around line 306), add the signal for HistoricalOnce mode:

```cpp
    // Signal lifecycle completion for one-shot mode.
    // PeriodicTail never signals — it runs until explicitly stopped.
    if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::HistoricalOnce)
    {
        signalCompleted();
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_done_ = true;
    }
```

Place this AFTER the catch blocks and BEFORE the `worker_done_` assignment. The signal fires regardless of whether the fetch succeeded or failed — the reader's work is done either way.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R reader_lifecycle_test -V`
Expected: ArchiverLifecycleTest.HistoricalOnceSignalsCompletion PASSES

- [ ] **Step 5: Commit**

```bash
git add src/reader/impl/epics_archiver/EpicsArchiverReader.cpp
git commit -m "feat: EpicsArchiverReader signals completion in HistoricalOnce mode"
```

---

### Task 5: SlacCalendarReader Signals Completion in One-Shot Mode

**Files:**
- Modify: `src/reader/impl/slac_calendar/SlacCalendarReader.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/reader/reader_lifecycle_test.cpp`:

```cpp
#include <reader/impl/slac_calendar/SlacCalendarReader.h>

namespace mldp_pvxs_driver::test {

class CalendarLifecycleTest : public ::testing::Test
{
protected:
    std::shared_ptr<MockLifecycleObserver> observer_ = std::make_shared<MockLifecycleObserver>();

    bool waitForCompletion(int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (observer_->completedCount() == 0)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
};

TEST_F(CalendarLifecycleTest, OneShotModeSignalsCompletion)
{
    // Config with rescan-interval-sec = 0 → one-shot mode
    boost::property_tree::ptree pt;
    pt.put("name", "test-calendar");
    pt.put("base-url", "http://localhost:19999");  // unreachable, fetch will fail
    pt.put("rescan-interval-sec", "0");

    boost::property_tree::ptree exp_node;
    exp_node.put("", "TEST_EXP");
    pt.add_child("experiments", boost::property_tree::ptree());
    pt.get_child("experiments").push_back({"", exp_node});

    auto bus = std::make_shared<mock::MockDataBus>();
    config::Config cfg(pt);

    auto reader = std::make_unique<reader::impl::slac_calendar::SlacCalendarReader>(
        bus, nullptr, cfg);
    reader->setLifecycleObserver(observer_);

    ASSERT_TRUE(waitForCompletion()) << "Calendar one-shot did not signal completion";
    EXPECT_EQ(observer_->lastCompletedName(), "test-calendar");
}

TEST_F(CalendarLifecycleTest, PeriodicModeDoesNotSignalCompletion)
{
    // Config with rescan-interval-sec > 0 → periodic mode
    boost::property_tree::ptree pt;
    pt.put("name", "test-calendar-periodic");
    pt.put("base-url", "http://localhost:19999");
    pt.put("rescan-interval-sec", "60");

    boost::property_tree::ptree exp_node;
    exp_node.put("", "TEST_EXP");
    pt.add_child("experiments", boost::property_tree::ptree());
    pt.get_child("experiments").push_back({"", exp_node});

    auto bus = std::make_shared<mock::MockDataBus>();
    config::Config cfg(pt);

    auto reader = std::make_unique<reader::impl::slac_calendar::SlacCalendarReader>(
        bus, nullptr, cfg);
    reader->setLifecycleObserver(observer_);

    // Wait briefly — should NOT signal
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(observer_->completedCount(), 0);
}

} // namespace mldp_pvxs_driver::test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R CalendarLifecycle -V`
Expected: FAIL — OneShotModeSignalsCompletion times out

- [ ] **Step 3: Add signalCompleted() to SlacCalendarReader::runWorker()**

Modify `src/reader/impl/slac_calendar/SlacCalendarReader.cpp` in `runWorker()`:

After the `do { ... } while (running_.load());` loop exits, add at the end of `runWorker()`:

```cpp
void SlacCalendarReader::runWorker()
{
    do
    {
        // ... existing code ...

        if (config_.rescanIntervalSec() <= 0.0)
            break;

        std::unique_lock<std::mutex> lk(worker_mutex_);
        worker_cv_.wait_for(lk,
                            std::chrono::duration<double>(config_.rescanIntervalSec()),
                            [this] { return !running_.load(); });

    } while (running_.load());

    // Signal completion only for one-shot mode (rescan ≤ 0).
    // Periodic readers exit only on shutdown (running_ = false) — not a natural completion.
    if (config_.rescanIntervalSec() <= 0.0)
    {
        signalCompleted();
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R CalendarLifecycle -V`
Expected: Both tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/reader/impl/slac_calendar/SlacCalendarReader.cpp
git commit -m "feat: SlacCalendarReader signals completion in one-shot mode"
```

---

### Task 6: EpicsDSMetadataReader Signals Completion in One-Shot Mode

**Files:**
- Modify: `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/reader/reader_lifecycle_test.cpp`:

```cpp
#include <reader/impl/epics_ds/EpicsDSMetadataReader.h>

namespace mldp_pvxs_driver::test {

class DSMetadataLifecycleTest : public ::testing::Test
{
protected:
    std::shared_ptr<MockLifecycleObserver> observer_ = std::make_shared<MockLifecycleObserver>();

    bool waitForCompletion(int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (observer_->completedCount() == 0)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
};

TEST_F(DSMetadataLifecycleTest, OneShotModeSignalsCompletion)
{
    // Config with rescan-interval-sec = 0 → one-shot mode
    // RPC will fail (no server) but reader should still signal completion
    boost::property_tree::ptree pt;
    pt.put("name", "test-ds-metadata");
    pt.put("service", "nonexistent_ds_service");
    pt.put("timeout-sec", "1");
    pt.put("rescan-interval-sec", "0");

    boost::property_tree::ptree pv_node;
    pv_node.put("name", "TEST:PV:1");
    boost::property_tree::ptree pvs_node;
    pvs_node.push_back({"", pv_node});
    pt.add_child("pvs", pvs_node);

    auto bus = std::make_shared<mock::MockDataBus>();
    config::Config cfg(pt);

    auto reader = std::make_unique<reader::impl::epics_ds::EpicsDSMetadataReader>(
        bus, nullptr, cfg);
    reader->setLifecycleObserver(observer_);

    ASSERT_TRUE(waitForCompletion()) << "DS metadata one-shot did not signal completion";
    EXPECT_EQ(observer_->lastCompletedName(), "test-ds-metadata");
}

} // namespace mldp_pvxs_driver::test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R DSMetadataLifecycle -V`
Expected: FAIL — observer never notified

- [ ] **Step 3: Add signalCompleted() to EpicsDSMetadataReader::runWorker()**

Modify `src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp` in `runWorker(std::stop_token st)`:

At the end of the function, after the `while` loop exits, add:

```cpp
void EpicsDSMetadataReader::runWorker(std::stop_token st)
{
    while (!st.stop_requested()) {
        // ... existing code ...

        if (config_.rescanIntervalSec() <= 0.0)
            break;

        // ... existing sleep ...
    }

    // Signal completion for one-shot mode.
    // If stop was requested (external shutdown), this is not a natural completion.
    if (!st.stop_requested() && config_.rescanIntervalSec() <= 0.0)
    {
        signalCompleted();
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R DSMetadataLifecycle -V`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/reader/impl/epics_ds/EpicsDSMetadataReader.cpp
git commit -m "feat: EpicsDSMetadataReader signals completion in one-shot mode"
```

---

### Task 7: Integration Test — Controller Auto-Closes When All Readers Complete

**Files:**
- Modify: `test/reader/reader_lifecycle_test.cpp`

- [ ] **Step 1: Write the integration test**

Add to `test/reader/reader_lifecycle_test.cpp`:

```cpp
namespace mldp_pvxs_driver::test {

class MockOneShotReader : public reader::Reader
{
public:
    MockOneShotReader(std::shared_ptr<util::bus::IDataBus> bus,
                      std::string                           name,
                      std::chrono::milliseconds             work_duration)
        : reader::Reader(std::move(bus), nullptr)
        , name_(std::move(name))
        , work_duration_(work_duration)
    {
        worker_ = std::thread([this] { runWorker(); });
    }

    ~MockOneShotReader() override
    {
        running_ = false;
        if (worker_.joinable())
            worker_.join();
    }

    std::string name() const override { return name_; }

private:
    void runWorker()
    {
        std::this_thread::sleep_for(work_duration_);
        if (running_)
            signalCompleted();
    }

    std::string               name_;
    std::chrono::milliseconds work_duration_;
    std::atomic<bool>         running_{true};
    std::thread               worker_;
};

TEST(ControllerAutoCloseIntegration, StopsWhenAllReadersComplete)
{
    auto observer = std::make_shared<MockLifecycleObserver>();

    // Create two mock one-shot readers that complete at different times
    auto bus = std::make_shared<mock::MockDataBus>();

    auto reader1 = std::make_unique<MockOneShotReader>(bus, "fast-reader", std::chrono::milliseconds(100));
    auto reader2 = std::make_unique<MockOneShotReader>(bus, "slow-reader", std::chrono::milliseconds(300));

    reader1->setLifecycleObserver(observer);
    reader2->setLifecycleObserver(observer);

    // Wait for both to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observer->completedCount() < 2)
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Timed out waiting for readers";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(observer->completedCount(), 2);
}

TEST(ControllerAutoCloseIntegration, LongRunningReaderPreventsShutdown)
{
    auto observer = std::make_shared<MockLifecycleObserver>();
    auto bus = std::make_shared<mock::MockDataBus>();

    // One reader that completes, one that doesn't
    auto oneshot_reader = std::make_unique<MockOneShotReader>(bus, "oneshot", std::chrono::milliseconds(100));
    oneshot_reader->setLifecycleObserver(observer);

    // Wait for one-shot to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (observer->completedCount() < 1)
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(observer->completedCount(), 1);
    EXPECT_EQ(observer->lastCompletedName(), "oneshot");
    // If there were a long-running reader, its absence from the completion list
    // means the controller would NOT shut down.
}

} // namespace mldp_pvxs_driver::test
```

- [ ] **Step 2: Run tests**

Run: `cmake --build build --target reader_lifecycle_test && ctest --test-dir build -R reader_lifecycle_test -V`
Expected: All tests PASS

- [ ] **Step 3: Commit**

```bash
git add test/reader/reader_lifecycle_test.cpp
git commit -m "test: add integration tests for reader lifecycle and auto-close behavior"
```

---

### Task 8: Verify No Regression — Full Test Suite

**Files:** None (verification only)

- [ ] **Step 1: Build entire project**

Run: `cmake --build build 2>&1 | tail -5`
Expected: Build succeeds with no errors

- [ ] **Step 2: Run full test suite**

Run: `ctest --test-dir build --output-on-failure -j$(nproc)`
Expected: All tests pass, no regressions

- [ ] **Step 3: Verify archiver periodic_tail does NOT signal completion**

Check `EpicsArchiverReader::runWorker()`: the `signalCompleted()` call is inside:
```cpp
if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::HistoricalOnce)
```

This means `PeriodicTail` mode never signals. Verify with the existing archiver tests that periodic_tail readers don't trigger unexpected behavior.

- [ ] **Step 4: Verify EPICS subscription readers (PVXS, Base) never signal**

These readers have no `signalCompleted()` call — they are event-driven and never naturally complete. Confirm by grep:

Run: `grep -rn "signalCompleted" src/reader/impl/epics/`
Expected: No matches (EpicsPVXSReader and EpicsBaseReader don't call it)

- [ ] **Step 5: Final commit (if any cleanup needed)**

```bash
git status
# If clean, no commit needed
```

---

## Design Notes for Implementer

### Thread Safety — The Deadlock Trap

The `onReaderCompleted()` callback is invoked FROM the reader's worker thread. The reader destructor calls `join()` on that same thread. If you destroy the reader synchronously in `onReaderCompleted()`, you deadlock (thread joins itself).

**Solution:** `onReaderCompleted()` posts removal to `thread_pool_->detach_task()`. The pool thread calls the destructor, which joins the worker thread. By that time the worker has already returned from `signalCompleted()` and is about to exit — `join()` blocks briefly (or not at all) until the thread function returns.

### Why No CLI Flag

Long-running readers (EPICS subscriptions, archiver periodic_tail) never call `signalCompleted()`. The controller's reader list never empties. Auto-close only triggers when ALL readers are one-shot AND all complete. No flag needed — the behavior is inherently correct.

### Archiver Restart Mode (PeriodicTail)

The archiver in `PeriodicTail` mode loops indefinitely (`while (running_.load())`), fetching sequential date ranges. It exits only when `running_` is set to false (external shutdown). The `signalCompleted()` guard:
```cpp
if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::HistoricalOnce)
```
ensures periodic_tail never triggers auto-close.

### In-Flight Batch Draining

When `onReaderCompleted()` fires, batches already pushed by the reader are already in the writer thread pool's queue. The controller's `stop()` method calls `writer->stop()` which (per existing implementation) drains pending work before returning. No additional draining logic needed.
