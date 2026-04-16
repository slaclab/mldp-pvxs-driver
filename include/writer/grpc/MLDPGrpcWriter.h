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

#include <config/Config.h>
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/grpc/MLDPGrpcWriterConfig.h>

#include <metrics/Metrics.h>
#include <pool/MLDPGrpcPool.h>
#include <util/log/Logger.h>

#include <BS_thread_pool.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::writer {

/**
 * @brief gRPC ingestion writer.
 *
 * Migrated verbatim from `MLDPPVXSController` — all worker logic
 * (`WorkerChannel`, `QueueItem`, `workerLoop()`, `buildRequest()`) lives
 * here.  No logic changes from the original controller implementation.
 *
 * The writer owns its `MLDPGrpcIngestionePool` and its thread pool.
 * `start()` registers the provider and spawns worker threads.
 * `push()` round-robins frames across channels identically to the
 * original controller.
 */
class MLDPGrpcWriter final : public IWriter
{
    REGISTER_WRITER("grpc", MLDPGrpcWriter)
public:
    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry. Delegates to the typed
     * constructor after calling @c MLDPGrpcWriterConfig::parse(root).
     */
    explicit MLDPGrpcWriter(const config::Config&             root,
                            std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit MLDPGrpcWriter(MLDPGrpcWriterConfig              config,
                            std::shared_ptr<metrics::Metrics> metrics = nullptr);
    ~MLDPGrpcWriter() override;

    std::string name() const override
    {
        return "grpc";
    }

    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;
    bool isHealthy() const noexcept override;

    /**
     * @brief Provider ID obtained after registration with the MLDP service.
     *
     * Only valid after @ref start has been called.
     */
    const std::string& providerId() const;

private:
    /// Smallest unit of queued work: one frame + shared batch metadata.
    struct QueueItem
    {
        std::string                                     root_source;
        std::shared_ptr<const std::vector<std::string>> tags;
        dp::service::common::DataFrame                  frame;
    };

    /// Per-worker channel: each worker has its own deque.
    struct WorkerChannel
    {
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<QueueItem>   items;
        bool                    shutdown{false};
    };

    MLDPGrpcWriterConfig                                              config_;
    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger>             logger_;
    std::shared_ptr<metrics::Metrics>                                 metrics_;
    std::shared_ptr<BS::light_thread_pool>                            threadPool_;
    util::pool::MLDPGrpcIngestionePool::MLDPGrpcIngestionePoolShrdPtr ingestionPool_;
    std::string                                                       providerId_;
    std::vector<std::unique_ptr<WorkerChannel>>                       channels_;
    std::atomic<std::size_t>                                          nextChannel_{0};
    std::atomic<std::size_t>                                          queuedItems_{0};
    std::atomic<bool>                                                 running_{false};

    void workerLoop(std::size_t workerIndex);
    bool buildRequest(const std::string&                         sourceName,
                      const dp::service::common::DataFrame&      frame,
                      const std::string&                         requestId,
                      dp::service::ingestion::IngestDataRequest& request,
                      std::size_t&                               acceptedEvents,
                      std::size_t&                               payloadBytes);
    void updateQueueDepthMetric();
};

} // namespace mldp_pvxs_driver::writer
