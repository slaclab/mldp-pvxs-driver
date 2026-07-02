//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file   BaseQueuedWriter.cpp
 * @brief  Template method definitions for BaseQueuedWriter<Item>.
 *
 * @details
 * This file is **not** compiled as a standalone translation unit.
 * It is `#include`-d at the bottom of `BaseQueuedWriter.h` so that the
 * compiler sees the full template body wherever the header is included.
 *
 * ## Data-flow overview
 *
 * ```
 *  Caller thread                   BaseQueuedWriter internals
 *  ─────────────                   ──────────────────────────
 *
 *  push(EventBatch)
 *      │
 *      ▼
 *  toItems()          ← subclass converts one batch → zero or more Items
 *      │
 *      │  (per Item)
 *      ▼
 *  [back-pressure wait]
 *      │  queuedItems_ >= queue_capacity  →  block on backpressureCv_
 *      │  running_ == false               →  return false immediately
 *      │  forceQuit_ == true              →  return false immediately
 *      ▼
 *  round-robin channel select
 *      │  idx = nextChannel_++ % channels_.size()
 *      ▼
 *  channels_[idx].items.push_back(item)   ← item owned by the deque
 *  queuedItems_++
 *  channels_[idx].cv.notify_one()
 *      │
 *      │                              ┌──────────────────────────────┐
 *      │                              │  workerLoop(workerIndex i)   │
 *      │                              │                              │
 *      │                              │  wait on channels_[i].cv     │
 *      │                              │      ↓                       │
 *      │                              │  dequeue item                │
 *      │                              │  queuedItems_--              │
 *      │                              │  backpressureCv_.notify_one()│  ← unblocks push()
 *      │                              │      ↓                       │
 *      │                              │  processItem(i, item)        │  ← subclass logic
 *      │                              └──────────────────────────────┘
 *      ▼
 *  return true
 * ```
 *
 * ## Back-pressure contract
 *
 * `push()` **never silently drops items**. When `queuedItems_` reaches
 * `queue_capacity` the calling thread blocks on `backpressureCv_` until a
 * worker drains at least one item and calls `backpressureCv_.notify_one()`.
 * The only paths that return `false` are:
 *   - `running_` becomes `false` (normal stop).
 *   - `forceQuit_` becomes `true` (emergency stop).
 *   - `push_timeout_ms > 0` and the wait deadline expires.
 *
 * ## Worker-channel isolation
 *
 * Each worker owns one `WorkerChannel` (mutex + CV + deque).  Items are
 * distributed round-robin via `nextChannel_` so workers are independent —
 * a slow `processItem` on worker N does not block worker M.
 *
 * ## Stop sequence
 *
 * 1. `stop()` sets `running_ = false` and wakes `backpressureCv_` so any
 *    blocked `push()` callers unblock and return `false`.
 * 2. Each channel's `shutdown` flag is set and its CV is notified so the
 *    worker drains remaining items before exiting.
 * 3. `threadPool_->wait()` joins all workers.
 * 4. `doStop()` is called after all threads have exited — safe to release
 *    connections or pools here.
 *
 * `forceStop()` sets `forceQuit_ = true` instead; workers break immediately
 * without draining, discarding any queued items.
 */

#pragma once

#include <writer/BaseQueuedWriter.h>
using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::writer;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Stores queue/thread configuration and the shared logger.
 *
 * @details No channels or thread pool are created here; they are allocated
 * lazily inside `start()` so that the writer can be constructed before the
 * rest of the application is initialised.
 *
 * @param[in] cfg         Queue/thread tuning (capacity, worker count, timeout).
 * @param[in] writerName  Human-readable name embedded in every log line.
 * @param[in] logger      Shared logger instance; must not be null.
 */
template <typename Item>
BaseQueuedWriter<Item>::BaseQueuedWriter(QueueConfig                         cfg,
                                         std::string                         writerName,
                                         std::shared_ptr<util::log::ILogger> logger)
    : cfg_(cfg)
    , writerName_(std::move(writerName))
    , logger_(std::move(logger))
{
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------

/**
 * @brief Allocates per-worker channels, starts the thread pool, then calls
 *        `doStart()`.
 *
 * @details Call sequence inside `start()`:
 * 1. Guard against double-start via `running_`.
 * 2. Allocate `worker_count` `WorkerChannel` objects (each: mutex + CV + deque).
 * 3. Create a `BS::light_thread_pool` and detach one `workerLoop(i)` task per
 *    channel.  Workers block immediately on their channel CV until the first
 *    item arrives.
 * 4. Call `doStart()` — the subclass opens connections, pools, etc.
 *
 * @note `doStart()` is called **after** threads are running, so the subclass
 * may safely enqueue work from inside `doStart()` if needed.
 */
template <typename Item>
void BaseQueuedWriter<Item>::start()
{
    if (running_.load())
    {
        util::log::warnf(*logger_, "BaseQueuedWriter '{}' already started", writerName_);
        return;
    }

    running_.store(true);
    forceQuit_.store(false);
    util::log::infof(*logger_, "BaseQueuedWriter '{}' starting ({} workers, queue_capacity={})",
                     writerName_, cfg_.worker_count, cfg_.queue_capacity);

    const auto wc = static_cast<std::size_t>(std::max(1, cfg_.worker_count));
    nextChannel_.store(0, std::memory_order_relaxed);
    queuedItems_.store(0, std::memory_order_relaxed);
    channels_.clear();
    channels_.reserve(wc);
    for (std::size_t i = 0; i < wc; ++i)
        channels_.push_back(std::make_unique<WorkerChannel>());

    threadPool_ = std::make_shared<BS::light_thread_pool>(
        wc,
        [](std::size_t i)
        {
            BS::this_thread::set_os_thread_name("bqw-pool-" + std::to_string(i));
        });

    for (std::size_t i = 0; i < wc; ++i)
    {
        threadPool_->detach_task([this, i]()
                                 {
                                     workerLoop(i);
                                 });
    }

    doStart();
    util::log::infof(*logger_, "BaseQueuedWriter '{}' started", writerName_);
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------

/**
 * @brief Gracefully drains all queued items, joins workers, then calls
 *        `doStop()`.
 *
 * @details Stop sequence:
 * 1. Set `running_ = false` and wake `backpressureCv_` — any caller blocked
 *    in `push()` will unblock and return `false`.
 * 2. Set `shutdown = true` on every `WorkerChannel` and notify each channel
 *    CV — workers finish processing their current item, drain the rest of the
 *    deque, then exit.
 * 3. `threadPool_->wait()` blocks until all worker tasks have returned.
 * 4. Channels and thread pool are destroyed.
 * 5. `doStop()` is called — safe to release connections or pools here because
 *    no worker thread is running at this point.
 *
 * @note Idempotent: calling `stop()` on an already-stopped writer is a no-op.
 */
template <typename Item>
void BaseQueuedWriter<Item>::stop() noexcept
{
    if (!running_.load())
        return;

    const auto pending = queuedItems_.load(std::memory_order_relaxed);
    util::log::infof(*logger_, "BaseQueuedWriter '{}' stopping — {} item(s) pending",
                     writerName_, pending);

    running_.store(false);
    backpressureCv_.notify_all();
    for (auto& ch : channels_)
    {
        {
            std::lock_guard lk(ch->mutex);
            ch->shutdown = true;
        }
        ch->cv.notify_one();
    }

    util::log::debugf(*logger_, "BaseQueuedWriter '{}' waiting for workers to drain...", writerName_);
    if (threadPool_)
        threadPool_->wait();

    channels_.clear();
    threadPool_.reset();
    doStop();
    util::log::infof(*logger_, "BaseQueuedWriter '{}' stopped", writerName_);
}

// ---------------------------------------------------------------------------
// forceStop
// ---------------------------------------------------------------------------

/**
 * @brief Signals an immediate, non-draining shutdown.
 *
 * @details Sets `forceQuit_ = true` and wakes all CVs.  Each `workerLoop`
 * checks `forceQuit_` at the top of its wait and breaks out immediately,
 * abandoning any items still in the channel deque.  Each `push()` caller
 * blocked on `backpressureCv_` also unblocks and returns `false`.
 *
 * @note `forceStop()` does **not** join threads — call `stop()` afterwards
 * (or let the destructor do so) to ensure clean teardown.
 */
template <typename Item>
void BaseQueuedWriter<Item>::forceStop() noexcept
{
    forceQuit_.store(true, std::memory_order_release);
    backpressureCv_.notify_all();
    for (auto& ch : channels_)
        ch->cv.notify_one();
}

// ---------------------------------------------------------------------------
// push
// ---------------------------------------------------------------------------

/**
 * @brief Converts a batch to Items via `toItems()`, then enqueues each Item
 *        with back-pressure.
 *
 * @details Per-Item enqueue path:
 * 1. Acquire `backpressureMutex_` and wait until
 *    `queuedItems_ < queue_capacity` **or** the writer is stopping.
 *    - If `push_timeout_ms == 0` (default): wait indefinitely — data is
 *      never dropped while the writer is healthy.
 *    - If `push_timeout_ms > 0`: return `false` after the deadline expires.
 * 2. Select a target channel: `idx = nextChannel_++ % channels_.size()`.
 * 3. Lock the channel mutex, append the Item to `channels_[idx].items`,
 *    increment `queuedItems_`, and notify the channel CV.
 *
 * `toItems()` is called outside any lock, on the caller's thread.  If it
 * returns an empty vector the batch is silently accepted (returns `true`).
 *
 * @param[in] batch  Incoming event batch from the bus.
 * @return `true`  if at least one Item was successfully enqueued (or if
 *                 `toItems()` returned empty — the batch was intentionally
 *                 filtered out by the subclass).
 * @return `false` if the writer is not running, `forceQuit_` is set, or
 *                 a `push_timeout_ms` deadline expired before space was
 *                 available.
 */
template <typename Item>
bool BaseQueuedWriter<Item>::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (!running_.load())
        return false;

    std::vector<Item> items;
    try
    {
        items = toItems(batch);
    }
    catch (const std::exception& ex)
    {
        util::log::errorf(*logger_, "BaseQueuedWriter '{}' toItems() threw: {}", writerName_, ex.what());
        return false;
    }
    catch (...)
    {
        util::log::errorf(*logger_, "BaseQueuedWriter '{}' toItems() threw unknown exception", writerName_);
        return false;
    }

    if (items.empty())
        return true;

    bool enqueued = false;
    for (auto& item : items)
    {
        {
            std::unique_lock lk(backpressureMutex_);
            if (cfg_.push_timeout_ms > 0)
            {
                const bool ok = backpressureCv_.wait_for(
                    lk,
                    std::chrono::milliseconds(cfg_.push_timeout_ms),
                    [this]
                    {
                        return queuedItems_.load(std::memory_order_relaxed) <
                                   static_cast<std::size_t>(cfg_.queue_capacity) ||
                               !running_.load() ||
                               forceQuit_.load(std::memory_order_acquire);
                    });
                if (!ok)
                {
                    util::log::warnf(*logger_, "BaseQueuedWriter '{}' push timed out after {}ms",
                                     writerName_, cfg_.push_timeout_ms);
                    return false;
                }
            }
            else
            {
                backpressureCv_.wait(lk, [this]
                                     {
                                         return queuedItems_.load(std::memory_order_relaxed) <
                                                    static_cast<std::size_t>(cfg_.queue_capacity) ||
                                                !running_.load() ||
                                                forceQuit_.load(std::memory_order_acquire);
                                     });
            }
            if (!running_.load() || forceQuit_.load(std::memory_order_acquire))
                return enqueued;
        }

        const auto idx = nextChannel_.fetch_add(1, std::memory_order_relaxed) % channels_.size();
        {
            std::lock_guard lk(channels_[idx]->mutex);
            channels_[idx]->items.push_back(std::move(item));
        }
        channels_[idx]->cv.notify_one();
        queuedItems_.fetch_add(1, std::memory_order_relaxed);
        enqueued = true;
    }
    return enqueued;
}

// ---------------------------------------------------------------------------
// Protected helpers
// ---------------------------------------------------------------------------

/**
 * @brief Notifies the back-pressure CV that one queue slot has been freed.
 *
 * @details The base `workerLoop` calls this automatically after each dequeue.
 * Subclasses may call it additionally when they split or discard items inside
 * `processItem()` to return capacity to `push()` sooner than the next natural
 * dequeue cycle.
 */
template <typename Item>
void BaseQueuedWriter<Item>::notifySlotFree()
{
    backpressureCv_.notify_one();
}

/**
 * @brief Returns the total number of items currently queued across all worker
 *        channels.
 *
 * @return Approximate queue depth (relaxed atomic load; may lag slightly).
 */
template <typename Item>
std::size_t BaseQueuedWriter<Item>::queueDepth() const noexcept
{
    return queuedItems_.load(std::memory_order_relaxed);
}

/**
 * @brief Returns the configured number of worker threads.
 * @return Worker count as stored in `QueueConfig::worker_count`.
 */
template <typename Item>
int BaseQueuedWriter<Item>::workerCount() const noexcept
{
    return cfg_.worker_count;
}

/**
 * @brief Returns the maximum number of items that may be queued before
 *        `push()` blocks.
 * @return Queue capacity as stored in `QueueConfig::queue_capacity`.
 */
template <typename Item>
int BaseQueuedWriter<Item>::queueCapacity() const noexcept
{
    return cfg_.queue_capacity;
}

/**
 * @brief Returns the human-readable name supplied at construction.
 * @return Reference to the internal writer name string.
 */
template <typename Item>
const std::string& BaseQueuedWriter<Item>::writerName() const noexcept
{
    return writerName_;
}

/**
 * @brief Returns a reference to the shared logger.
 * @return Dereferenced `ILogger`; always valid while the writer is alive.
 */
template <typename Item>
util::log::ILogger& BaseQueuedWriter<Item>::logger() noexcept
{
    return *logger_;
}

// ---------------------------------------------------------------------------
// workerLoop (private)
// ---------------------------------------------------------------------------

/**
 * @brief Per-worker thread body: dequeues Items and calls `processItem()`.
 *
 * @details Each worker owns a single `WorkerChannel` at index `workerIndex`.
 * The loop:
 * 1. Waits on `channels_[workerIndex].cv` until the channel has items, the
 *    `shutdown` flag is set, or `forceQuit_` is raised.
 * 2. On `forceQuit_`: breaks immediately — queued items are abandoned.
 * 3. On `shutdown` with an empty deque: breaks — normal drain complete.
 * 4. Otherwise: moves the front item out of the deque, decrements
 *    `queuedItems_`, and calls `backpressureCv_.notify_one()` to unblock any
 *    `push()` caller that was waiting for capacity.
 * 5. Calls `processItem(workerIndex, item)` outside the channel lock so
 *    other workers and `push()` can proceed concurrently.
 * 6. Exceptions from `processItem()` are caught and logged; the loop
 *    continues with the next item — a single bad item does not kill the
 *    worker.
 *
 * @param[in] workerIndex  Zero-based index of this worker; also the index of
 *                         the `WorkerChannel` this worker exclusively drains.
 */
template <typename Item>
void BaseQueuedWriter<Item>::workerLoop(std::size_t workerIndex)
{
    auto& ch = *channels_[workerIndex];

    while (true)
    {
        Item item{};
        bool hasItem = false;
        {
            std::unique_lock lk(ch.mutex);
            const auto pred = [&]
            {
                return !ch.items.empty() || ch.shutdown ||
                       forceQuit_.load(std::memory_order_relaxed);
            };
            if (cfg_.idle_check_ms > 0)
            {
                ch.cv.wait_for(lk, std::chrono::milliseconds(cfg_.idle_check_ms), pred);
            }
            else
            {
                ch.cv.wait(lk, pred);
            }
            if (forceQuit_.load(std::memory_order_relaxed))
                break;
            if (ch.shutdown && ch.items.empty())
                break;
            if (!ch.items.empty())
            {
                item    = std::move(ch.items.front());
                hasItem = true;
                ch.items.pop_front();
            }
        }

        if (!hasItem)
        {
            try { onWorkerIdle(workerIndex); } catch (...) {}
            continue;
        }

        queuedItems_.fetch_sub(1, std::memory_order_relaxed);
        backpressureCv_.notify_one();

        try
        {
            processItem(workerIndex, std::move(item));
        }
        catch (const std::exception& ex)
        {
            util::log::errorf(*logger_, "BaseQueuedWriter '{}' worker[{}] processItem threw: {}",
                              writerName_, workerIndex, ex.what());
        }
        catch (...)
        {
            util::log::errorf(*logger_, "BaseQueuedWriter '{}' worker[{}] processItem threw unknown exception",
                              writerName_, workerIndex);
        }
    }
}
