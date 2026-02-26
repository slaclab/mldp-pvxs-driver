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
 * @file EpicsArchiverReader.h
 * @brief Reader implementation for SLAC Archiver Appliance API.
 *
 * This header provides the EpicsArchiverReader class, which implements a reader
 * that retrieves archived EPICS data from the SLAC Archiver Appliance.
 */

#pragma once

#include <EPICSEvent.pb.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>
#include <util/bus/IEventBusPush.h>
#include <util/log/ILog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::util::http {
class IHttpClient;
}

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief Incremental state while decoding one PB/HTTP chunk.
 *
 * A PB/HTTP chunk begins with a @ref EPICS::PayloadInfo line and is followed by
 * one or more sample lines of the type declared in that header, ending with an
 * empty line. This structure accumulates the parsed header and converted event
 * values until the chunk terminator is reached and the batch is published.
 */
struct PbChunkState
{
    bool                                              have_header = false;           ///< True after PayloadInfo has been parsed.
    EPICS::PayloadInfo                                header;                        ///< Payload header for the current chunk.
    std::vector<util::bus::IEventBusPush::EventValue> events;                        ///< Converted sample events for this chunk.
    bool                                              have_batch_start_time = false; ///< True after first sample of current output batch.
    uint64_t                                          batch_start_epoch_seconds = 0; ///< Historical batch start (seconds).
    uint32_t                                          batch_start_nanoseconds = 0;   ///< Historical batch start (nanoseconds).
};

/**
 * @brief Reader implementation for SLAC Archiver Appliance API.
 *
 * This reader retrieves archived EPICS data from the SLAC Archiver Appliance
 * using the protobuf-based data format (EPICSEvent.proto).
 */
class EpicsArchiverReader : public ::mldp_pvxs_driver::reader::Reader
{
public:
    /**
     * @brief Construct an Archiver reader from configuration.
     *
     * @param bus Event bus for publishing retrieved data.
     * @param metrics Metrics collector for instrumentation (may be null).
     * @param cfg Reader configuration.
     */
    EpicsArchiverReader(std::shared_ptr<::mldp_pvxs_driver::util::bus::IEventBusPush> bus,
                        std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics>         metrics,
                        const ::mldp_pvxs_driver::config::Config&                     cfg);

    /**
     * @brief Destructor - stops reader and releases resources.
     */
    ~EpicsArchiverReader() override;

    /**
     * @brief Get the reader's configured name.
     *
     * @return The name specified in the reader's configuration.
     */
    std::string name() const override;

private:
    std::shared_ptr<util::log::ILogger>                          logger_;      ///< Logger instance for this reader.
    std::unique_ptr<::mldp_pvxs_driver::util::http::IHttpClient> http_client_; ///< Shared HTTP transport abstraction.
    std::string                                                  name_;        ///< Reader name from configuration.
    EpicsArchiverReaderConfig                                    config_;      ///< Parsed archiver reader configuration.

    /**
     * @brief Initialize reusable HTTP client for archiver API access.
     */
    void initializeHttpClient();

    /**
     * @brief Clean up HTTP transport resources.
     *
     * Ensures that any transport state owned by this reader is released.
     */
    void destroyHttpClient();

    /**
     * @brief Fetch and publish archived samples for configured PVs.
     *
     * Performs HTTP PB/HTTP retrieval requests against the configured archiver
     * endpoint and publishes converted samples to the event bus.
     *
     * @throws std::runtime_error on transport, protocol, or parse failures.
     */
    void fetchConfiguredPVs();

    REGISTER_READER("epics-archiver", EpicsArchiverReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
