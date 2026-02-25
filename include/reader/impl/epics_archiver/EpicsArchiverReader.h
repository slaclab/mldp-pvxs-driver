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

#include <reader/ReaderFactory.h>
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>

#include <memory>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

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
    std::string                  name_;   ///< Reader name from configuration.
    EpicsArchiverReaderConfig    config_; ///< Parsed archiver reader configuration.

    REGISTER_READER("epics-archiver", EpicsArchiverReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver