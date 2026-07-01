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
#include <writer/BaseQueuedWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.h>

#include <memory>
#include <string>
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
class MLDPPVMetadataWriter final : public BaseQueuedWriter<
    std::pair<std::string, util::bus::SourceMetadataEntry>>
{
    REGISTER_WRITER("mldp-pv-metadata", MLDPPVMetadataWriter)

public:
    using WorkItem = std::pair<std::string, util::bus::SourceMetadataEntry>;

    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry.
     */
    explicit MLDPPVMetadataWriter(const config::Config&             root,
                                  std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit MLDPPVMetadataWriter(MLDPPVMetadataWriterConfig        config,
                                  std::shared_ptr<metrics::Metrics> metrics = nullptr);

    ~MLDPPVMetadataWriter() override;

    std::string name() const override
    {
        return config_.name;
    }

    bool acceptsPayload(const util::bus::BatchPayload& p) const noexcept override
    {
        return std::holds_alternative<util::bus::SourceMetadataPayload>(p);
    }

protected:
    std::vector<WorkItem> toItems(util::bus::IDataBus::EventBatch& batch) override;
    void processItem(std::size_t workerIndex, WorkItem item) override;
    void doStart() override;
    void doStop() noexcept override;

private:
    void saveSourceMetadata(const std::string&                    sourceName,
                            const util::bus::SourceMetadataEntry& entry);

    MLDPPVMetadataWriterConfig                          config_;
    std::shared_ptr<metrics::Metrics>                   metrics_;
    std::shared_ptr<util::pool::MLDPGrpcAnnotationPool> pool_;
};

} // namespace mldp_pvxs_driver::writer
