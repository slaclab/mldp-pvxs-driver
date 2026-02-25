//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/EpicsArchiverReader.h>

using namespace mldp_pvxs_driver::reader::impl::epics_archiver;
using namespace mldp_pvxs_driver::config;

EpicsArchiverReader::EpicsArchiverReader(
    std::shared_ptr<::mldp_pvxs_driver::util::bus::IEventBusPush> bus,
    std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics>         metrics,
    const ::mldp_pvxs_driver::config::Config&                     cfg)
    : ::mldp_pvxs_driver::reader::Reader(std::move(bus), std::move(metrics))
    , name_(cfg.get("name"))
    , config_(cfg)
{
    // Validate configuration
    if (!config_.valid())
    {
        throw EpicsArchiverReaderConfig::Error("Failed to parse Archiver reader configuration");
    }

    // TODO: Implement initialization for Archiver Appliance reader
    // - Set up connection to archiver service at config_.hostname()
    // - Initialize data retrieval for PVs in config_.pvNames()
    // - Start background polling/fetching threads
}

EpicsArchiverReader::~EpicsArchiverReader()
{
    // TODO: Clean up archiver reader resources
}

std::string EpicsArchiverReader::name() const
{
    return name_;
}