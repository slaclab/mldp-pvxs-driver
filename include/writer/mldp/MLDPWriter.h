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
#include <util/bus/DataBatch.h>
#include <writer/BaseQueuedWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp/MLDPWriterConfig.h>

#include <metrics/Metrics.h>
#include <pool/MLDPGrpcPool.h>
#include <util/log/Logger.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::writer {

/// Smallest unit of queued work passed between MLDPWriter::push and workers.
struct MLDPQueueItem
{
    std::string                                                          root_source;
    std::shared_ptr<const std::unordered_map<std::string, std::string>> metadata;
    util::bus::DataBatch                                                 frame;
};

/**
 * @brief MLDP ingestion writer.
 *
 * Forwards event batches to the MLDP ingestion service over gRPC.
 * Queue management, backpressure, and thread-pool lifecycle are delegated
 * to @ref BaseQueuedWriter.  This class supplies @c toItems() and
 * @c processItem() domain hooks plus gRPC stream management.
 */
class MLDPWriter final : public BaseQueuedWriter<MLDPQueueItem>
{
    REGISTER_WRITER("mldp", MLDPWriter)
public:
    using QueueItem = MLDPQueueItem;

    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry. Delegates to the typed
     * constructor after calling @c MLDPWriterConfig::parse(root).
     */
    explicit MLDPWriter(const config::Config&             root,
                        std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit MLDPWriter(MLDPWriterConfig                  config,
                        std::shared_ptr<metrics::Metrics> metrics = nullptr);
    ~MLDPWriter() override;

    std::string name() const override
    {
        return config_.name;
    }

    bool acceptsPayload(const util::bus::BatchPayload& payload) const noexcept override
    {
        return std::holds_alternative<util::bus::TimeSeriesPayload>(payload);
    }

    /**
     * @brief Provider ID obtained after registration with the MLDP service.
     *
     * Only valid after @ref start has been called.
     */
    const std::string& providerId() const;

protected:
    std::vector<QueueItem> toItems(util::bus::IDataBus::EventBatch& batch) override;
    void                   processItem(std::size_t workerIndex, QueueItem item) override;
    void                   doStart() override;
    void                   doStop() noexcept override;

private:
    /// gRPC stream state owned by one worker for its lifetime.
    struct StreamState;

    std::vector<std::unique_ptr<StreamState>> workerStates_;

    MLDPWriterConfig                                                   config_;
    std::shared_ptr<metrics::Metrics>                                  metrics_;
    util::pool::MLDPGrpcIngestionePool::MLDPGrpcIngestionePoolShrdPtr  ingestionPool_;
    std::string                                                        providerId_;

    /// Throttled push logging (every 10s).
    std::mutex                            pushLogMutex_;
    std::chrono::steady_clock::time_point lastPushLogTime_{};

    /// Wall-clock windowed throughput tracker per source (per-worker, no shared mutex).
    struct SourceRateTracker {
        std::chrono::steady_clock::time_point lastWallTime{};
        std::size_t accumulatedBytes        = 0;
        std::size_t accumulatedPayloadBytes = 0;
    };

    void closeStream(StreamState& state, const char* reason) noexcept;
    bool ensureStream(StreamState& state);
    bool rotateStream(StreamState& state, const char* reason);
    void onWorkerIdle(std::size_t workerIndex) override;
    void updateSourceRateMetrics(StreamState&       state,
                                 std::size_t        workerIndex,
                                 const std::string& source,
                                 std::size_t        dataBatchBytes,
                                 std::size_t        payloadBytes);
    static dp::service::common::DataFrame toSingleColumnDataFrame(
        const util::bus::DataBatch&                         batch,
        std::size_t                                         colIndex,
        bool                                                isEnum,
        const std::string&                                  rootSource,
        const std::unordered_map<std::string, std::string>* metadata = nullptr);
    void updateQueueDepthMetric();
};

} // namespace mldp_pvxs_driver::writer
