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

#include <util/bus/IDataBus.h>
#include <util/log/Logger.h>
#include <writer/IWriter.h>

#include <BS_thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Abstract base that provides bounded MPSC queue, backpressure, and
 *        a worker-thread pool to any writer that needs internal queuing.
 *
 * Subclasses supply the domain-specific conversion and processing logic via
 * two pure-virtual hooks:
 *   - toItems()     — convert an incoming EventBatch to zero or more Items
 *   - processItem() — consume one Item on a worker thread
 *
 * Two optional lifecycle hooks (doStart / doStop) let subclasses perform
 * setup/teardown around the thread-pool lifecycle without overriding start/stop.
 *
 * @tparam Item  The per-worker queue element type.  Each concrete subclass
 *               defines its own Item (e.g. a per-frame struct, a work-unit).
 */
template <typename Item>
class BaseQueuedWriter : public IWriter
{
public:
    /**
     * @brief Configuration consumed by the base class.
     *
     * Subclasses embed or forward these values from their own config struct.
     */
    struct QueueConfig
    {
        int queue_capacity  = 200; ///< Max queued items before push() blocks.
        int worker_count    = 8;   ///< Number of worker threads.
        int push_timeout_ms = 0;   ///< 0 = block forever; >0 = timeout in ms.
    };

    // -----------------------------------------------------------------------
    // IWriter lifecycle — final; subclasses use doStart / doStop hooks
    // -----------------------------------------------------------------------

    /**
     * @brief Starts the thread pool and calls doStart().
     */
    void start() final;

    /**
     * @brief Drains all queues, joins workers, calls doStop().
     */
    void stop() noexcept final;

    /**
     * @brief Discards queued work immediately and signals workers to exit.
     */
    void forceStop() noexcept final;

    // -----------------------------------------------------------------------
    // push — default implementation; subclass may override entirely
    // -----------------------------------------------------------------------

    /**
     * @brief Converts batch to Items via toItems(), then enqueues with backpressure.
     *
     * Blocks per-item when the queue is full.  Returns false only when the
     * writer is stopped or a push_timeout_ms timeout expires.
     */
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;

protected:
    /**
     * @brief Construct with queue config and a pre-built logger.
     *
     * @param cfg         Queue/thread tuning parameters.
     * @param writerName  Human-readable name (used in log lines).
     * @param logger      Logger instance (caller creates with newLogger()).
     */
    explicit BaseQueuedWriter(QueueConfig                         cfg,
                              std::string                         writerName,
                              std::shared_ptr<util::log::ILogger> logger);

    ~BaseQueuedWriter() override = default;

    // -----------------------------------------------------------------------
    // Pure-virtual hooks — subclass contract
    // -----------------------------------------------------------------------

    /**
     * @brief Convert an accepted EventBatch into zero or more queue Items.
     *
     * Return an empty vector to silently drop the batch.
     * Called from push() on the caller's thread.
     */
    virtual std::vector<Item> toItems(util::bus::IDataBus::EventBatch& batch) = 0;

    /**
     * @brief Process one dequeued Item.
     *
     * Called from a worker thread.  Must not throw (exceptions are caught and
     * logged by the base workerLoop).
     *
     * @param workerIndex  Zero-based index of the calling worker thread.
     * @param item         The dequeued item (moved in, subclass owns it).
     */
    virtual void processItem(std::size_t workerIndex, Item item) = 0;

    // -----------------------------------------------------------------------
    // Optional lifecycle hooks
    // -----------------------------------------------------------------------

    /** Called after threads are up, inside start(). */
    virtual void doStart() {}

    /** Called after all threads are joined, inside stop(). */
    virtual void doStop() noexcept {}

    // -----------------------------------------------------------------------
    // Protected helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Notify the backpressure CV that a slot has been freed.
     *
     * The base workerLoop calls this automatically after each dequeue.
     * Subclasses may call it additionally when they split or discard items
     * mid-processItem to return capacity sooner.
     */
    void notifySlotFree();

    /** Current total items queued across all worker channels. */
    std::size_t queueDepth() const noexcept;

    int workerCount() const noexcept;

    int queueCapacity() const noexcept;

    const std::string& writerName() const noexcept;

    util::log::ILogger& logger() noexcept;

private:
    // -----------------------------------------------------------------------
    // Per-worker channel
    // -----------------------------------------------------------------------

    struct WorkerChannel
    {
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<Item>        items;
        bool                    shutdown{false};
    };

    void workerLoop(std::size_t workerIndex);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    QueueConfig                                 cfg_;
    std::string                                 writerName_;
    std::shared_ptr<util::log::ILogger>         logger_;
    std::shared_ptr<BS::light_thread_pool>      threadPool_;
    std::vector<std::unique_ptr<WorkerChannel>> channels_;
    std::atomic<std::size_t>                    nextChannel_{0};
    std::atomic<std::size_t>                    queuedItems_{0};
    std::atomic<bool>                           running_{false};
    std::atomic<bool>                           forceQuit_{false};
    std::mutex                                  backpressureMutex_;
    std::condition_variable                     backpressureCv_;
};

} // namespace mldp_pvxs_driver::writer

#include "../../src/writer/BaseQueuedWriter.cpp"
