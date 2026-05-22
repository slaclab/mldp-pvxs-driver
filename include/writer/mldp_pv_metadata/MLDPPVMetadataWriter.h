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
#include <pool/MLDPGrpcAnnotationPool.h>
#include <util/log/Logger.h>
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
} // namespace mldp_pvxs_driver::metrics

namespace mldp_pvxs_driver::writer {

/**
 * @brief PV metadata writer that persists PV metadata via the DpAnnotationService gRPC API.
 *
 * Accepts @ref util::bus::SourceMetadataPayload batches, fans each source-entry
 * pair into an internal work queue, and drains the queue with a configurable
 * number of worker threads.  Each worker calls `savePvMetadata` on the
 * annotation gRPC endpoint backed by @ref util::pool::MLDPGrpcAnnotationPool.
 *
 * Lifecycle contract: construct → @ref start → @ref push … → @ref stop.
 */
class MLDPPVMetadataWriter final : public IWriter
{
    REGISTER_WRITER("mldp-pv-metadata", MLDPPVMetadataWriter)

public:
    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry.
     */
    explicit MLDPPVMetadataWriter(const config::Config&             root,
                                  std::shared_ptr<metrics::Metrics> metrics = nullptr);

    ~MLDPPVMetadataWriter() override;

    std::string name() const override
    {
        return config_.name;
    }

    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;

    bool acceptsPayload(const util::bus::BatchPayload& p) const noexcept override
    {
        return std::holds_alternative<util::bus::SourceMetadataPayload>(p);
    }

private:
    /// Smallest unit of queued work: one source name paired with its metadata entry.
    struct WorkItem
    {
        std::string                    source_name;
        util::bus::SourceMetadataEntry entry;
    };

    void workerLoop();
    void saveSourceMetadata(const std::string&                    sourceName,
                            const util::bus::SourceMetadataEntry& entry);

    MLDPPVMetadataWriterConfig                          config_;
    std::shared_ptr<metrics::Metrics>                   metrics_;
    std::shared_ptr<util::log::ILogger>                 logger_;
    std::shared_ptr<util::pool::MLDPGrpcAnnotationPool> pool_;

    std::queue<WorkItem>     work_queue_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool>        stop_{false};
    std::atomic<bool>        running_{false};
};

} // namespace mldp_pvxs_driver::writer
