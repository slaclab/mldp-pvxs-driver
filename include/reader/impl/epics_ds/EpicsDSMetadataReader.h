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
 * @file EpicsDSMetadataReader.h
 * @brief PVXS RPC-based reader that fetches PV metadata from an EPICS Directory Service.
 *
 * Issues an NTURI RPC call to a PVA Directory Service endpoint and publishes
 * the resulting NTTable as a SourceMetadataPayload on the driver bus.
 * Supports optional periodic re-fetch via rescan-interval-sec.
 */

#pragma once

#include <config/Config.h>
#include <reader/IReader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/ILog.h>

#include <pvxs/client.h>
#include <pvxs/data.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::reader::impl::epics_ds {

/**
 * @brief Reader that queries an EPICS Directory Service via PVA RPC and
 *        publishes PV metadata onto the bus as SourceMetadataPayload.
 *
 * The reader constructs an NTURI request, executes an RPC call against the
 * configured service name, parses the NTTable response, and pushes the
 * resulting SourceMetadataPayload via the bus.  When rescan-interval-sec > 0
 * the fetch is repeated at that interval until the reader is destroyed.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-ds-metadata
 *     name: my-ds-reader
 *     service: ds
 *     query: "%"
 *     timeout-sec: 5.0
 *     source-name-column: channelName
 *     tags-column: tags
 *     rescan-interval-sec: 300.0
 * @endcode
 */
class EpicsDSMetadataReader final : public reader::Reader
{
    REGISTER_READER("epics-ds-metadata", EpicsDSMetadataReader)

public:
    /**
     * @brief Construct and start the reader.
     *
     * @param bus    Event bus for publishing SourceMetadataPayload batches.
     * @param metrics Metrics collector (may be null).
     * @param cfg    Reader configuration node.
     * @throws EpicsDSMetadataReaderConfig::Error if configuration is invalid.
     */
    EpicsDSMetadataReader(std::shared_ptr<util::bus::IDataBus> bus,
                          std::shared_ptr<metrics::Metrics>    metrics,
                          const config::Config&                cfg);

    ~EpicsDSMetadataReader() override;

    std::string name() const override { return config_.name(); }

private:
    /**
     * @brief Worker thread body: fetch, parse, push, then sleep or exit.
     */
    void runWorker();

    /**
     * @brief Parse an NTTable PVXS Value into a SourceMetadataPayload.
     *
     * @param result PVXS Value returned by the RPC call.
     * @return Map of source name to metadata entry.
     */
    util::bus::SourceMetadataPayload parseNTTable(const pvxs::Value& result) const;

    EpicsDSMetadataReaderConfig         config_;
    std::shared_ptr<util::log::ILogger> logger_;
    pvxs::client::Context               pva_context_;
    std::thread                         worker_thread_;
    std::atomic<bool>                   running_{false};
    std::condition_variable             worker_cv_;
    std::mutex                          worker_mutex_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
