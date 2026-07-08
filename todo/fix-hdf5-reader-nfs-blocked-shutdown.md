# Fix: HDF5 Reader NFS Blocked Open Prevents Clean Shutdown

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or one of the available orchestrator plugins

## Problem

Instance ran ~24 h reading BSAS HDF5 files from `/sdf/data/...` (SDF NFS). At 07:05:11 the
last-logged file completed. Process went silent for 80 min. The reader had moved on to the
**next** file in the glob and was blocked inside `H5::H5File file(nextPath, H5F_ACC_RDONLY)` —
an NFS open that never returned. No `running_` guard exists before this call.

When SIGINT was sent at 08:25:38, `stop()` called `readers_.clear()`, which destructed
`HDF5BsasGen1Reader`. The destructor calls `worker_.join()` — blocking forever because the
worker is stuck in HDF5. This prevented the entire shutdown chain:
- `queue_not_empty_.notify_all()` never called
- `consumer_thread_.join()` never returned
- writers never drained
- `stopped_.store(true)` never set
- process could not exit

Evidence: `"Removing 1 reader(s)"` at 08:25 means `onReaderCompleted()` / `signalCompleted()`
was never reached — reader never finished.

## Root Causes

**Bug A** — No `running_` guard before file open (`HDF5BsasGen1Reader.cpp:279`). On NFS a
blocking open can hang indefinitely. No way to interrupt or detect timeout in worker thread.

**Bug B** — `MLDPPVXSController::stop()` clears readers first (line 361–364), triggering
destructors that block on `worker_.join()`, before waking the consumer thread or stopping
writers. Correct order: signal readers → wake consumer → drain writers → destroy readers.

## Fix Plan

### Task 1: Guard file open with `running_` check

**File:** `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp`

Add at top of file loop, before `H5::H5File file(...)` (line 279):

```cpp
for (const auto& filePath : files)
{
    if (!running_)         // ← add: bail before attempting blocking open
        break;
    current_file_name = filePath.filename().string();
    ...
    H5::H5File file(filePath.string(), H5F_ACC_RDONLY);
```

This prevents starting a new file after `running_` is set to false (e.g., previous chunk
returned false). It does not help when the open itself hangs — that requires Task 3.

- [ ] Add `if (!running_) break;` before `H5::H5File` constructor call
- [ ] Verify existing test suite still passes

---

### Task 2: Add `requestStop()` to Reader base class

**File:** `include/reader/IReader.h`

Add a virtual non-blocking stop signal:

```cpp
/** @brief Signal the reader to stop at the next safe point (non-blocking). */
virtual void requestStop() {}
```

**File:** `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h`

Override in HDF5 reader:

```cpp
void requestStop() override;
```

**File:** `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp`

```cpp
void HDF5BsasGen1Reader::requestStop()
{
    running_ = false;
}
```

- [ ] Add `virtual void requestStop() {}` to `Reader` base
- [ ] Add override decl to `HDF5BsasGen1Reader.h`
- [ ] Implement `requestStop()` in `HDF5BsasGen1Reader.cpp`

---

### Task 3: Restructure `stop()` — signal readers first, clear readers last

**File:** `src/controller/MLDPPVXSController.cpp`, function `stop()` (lines 348–395)

Current order:
1. `running_.store(false)`
2. `readers_.clear()` ← BLOCKS on worker_.join() if reader stuck
3. `queue_not_*_.notify_all()`
4. `consumer_thread_.join()`
5. stop writers

Fixed order:

```cpp
void MLDPPVXSController::stop()
{
    if (stopping_.exchange(true))
    {
        while (!stopped_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return;
    }

    running_.store(false);
    infof(*logger_, "Controller is stopping");

    // 1. Signal readers to stop (non-blocking — sets running_=false in reader)
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        infof(*logger_, "Signalling {} reader(s) to stop", readers_.size());
        for (auto& r : readers_)
            r->requestStop();
    }

    // 2. Wake blocked pushers and consumer thread
    queue_not_full_.notify_all();
    queue_not_empty_.notify_all();

    // 3. Join consumer thread
    if (consumer_thread_.joinable())
        consumer_thread_.join();
    infof(*logger_, "Consumer thread joined — queue drained");

    // 4. Stop processors
    for (auto& processor : processors_)
    {
        debugf(*logger_, "Stopping processor '{}'", processor->name());
        processor->stop();
    }
    processors_.clear();

    // 5. Drain and stop writers
    for (auto& w : writers_)
    {
        infof(*logger_, "Stopping writer '{}' — waiting for internal queue drain...", w->name());
        w->stop();
        infof(*logger_, "Writer '{}' stopped", w->name());
    }
    writers_.clear();

    // 6. Destroy readers last — destructor joins worker thread
    //    (may block up to timed-join timeout if stuck in HDF5 open; see Task 4)
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        infof(*logger_, "Removing {} reader(s)", readers_.size());
        readers_.clear();
    }

    infof(*logger_, "Controller stopped");
    stopped_.store(true);
}
```

- [ ] Reorder `stop()` as above
- [ ] Verify no deadlock: `readers_.clear()` is now AFTER writers stop, consumer joined
- [ ] Run full test suite

---

### Task 4: Timed join in HDF5 reader destructor (Linux-only fallback)

When `readers_.clear()` is called and a worker is stuck in an NFS `H5File` open, the process
would still hang. Add a timed join: wait 5 s, then detach if exceeded.

**Platform:** Rocky Linux 9 (glibc) has `pthread_timedjoin_np`.

**File:** `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp`

Replace destructor (currently `~HDF5BsasGen1Reader`, lines 231–236):

```cpp
HDF5BsasGen1Reader::~HDF5BsasGen1Reader()
{
    running_ = false;
    if (worker_.joinable())
    {
#if defined(__linux__)
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        const int rc = pthread_timedjoin_np(worker_.native_handle(), nullptr, &ts);
        if (rc == 0)
        {
            worker_.detach();   // pthread already joined; release std::thread handle
        }
        else
        {
            if (logger_)
                logger_->log(util::log::Level::Warn,
                    "~HDF5BsasGen1Reader: worker stuck in HDF5 call after 5s — detaching");
            worker_.detach();
        }
#else
        worker_.join();         // macOS / fallback: blocking join
#endif
    }
}
```

**Safety:** After `detach()`, the orphaned thread holds only stack-local data (HDF5 handles,
local vectors). It must NOT access `this` members. Audit the `readFile()` loop: `running_`,
`logger_`, `metrics_` are accessed. To be safe, `running_` is already checked at safe points.
`logger_` and `metrics_` are `shared_ptr` — if reader `shared_ptr` count drops to zero on
detach, the logger/metrics objects may be destroyed. Solution: the thread captures `shared_ptr`
copies of `logger_` and `metrics_` by value at the start of `readFile()`, so they outlive `this`.

Alternatively (simpler): only detach if `force_quit_` is set (i.e., user explicitly killed),
keeping blocking join as normal-shutdown behavior. Controller then adds a SIGALRM fallback.

- [ ] Option A: Implement timed join with `pthread_timedjoin_np` + local `shared_ptr` captures
- [ ] Option B: Detach only on `force_quit_` path, keep normal blocking join
- [ ] Decision: go with **Option A** (bounded shutdown always, not just on double-SIGINT)
- [ ] Add `#include <pthread.h>` to `.cpp`
- [ ] Capture `logger_` and `metrics_` as `shared_ptr` locals at top of `readFile()`
- [ ] Test: simulate hang, confirm process exits within 5s + writer drain

---

### Task 5: Regression & integration tests

- [ ] Unit test: `requestStop()` sets `running_` false in HDF5 reader
- [ ] Unit test: controller `stop()` calls `requestStop()` on all readers before clearing queue
- [ ] Integration test: reader stuck in file loop → SIGINT → process exits within `join_timeout + writer_drain`
- [ ] Full test suite passes

---

## Files to Modify

| File | Change |
|------|--------|
| `include/reader/IReader.h` | Add `virtual void requestStop() {}` |
| `include/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h` | Declare `requestStop()` override |
| `src/reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.cpp` | `running_` guard before open; timed join in destructor; local shared_ptr captures |
| `src/controller/MLDPPVXSController.cpp` | Reorder `stop()`: signal → wake consumer → join consumer → stop writers → clear readers |

## Verification

1. Build in devcontainer
2. Run with glob matching multiple files; kill mid-run with SIGINT → clean exit within ~6 s
3. Simulate NFS hang (chmod a-r next file) → SIGINT → exits within 5 s (timed join) + writer drain
4. Run full file set → auto-completion triggers stop, process exits cleanly
5. Run full test suite — no regressions
