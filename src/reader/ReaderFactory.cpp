//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// ReaderFactory.cpp
#include <reader/ReaderFactory.h>

using namespace mldp_pvxs_driver::reader;

std::unique_ptr<Reader> ReaderFactory::create(
    const std::string&                                  type,
    std::shared_ptr<util::bus::IDataBus>                bus,
    const ::mldp_pvxs_driver::config::Config&           cfg,
    std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics)
{
    // Reorder: public API is (type, bus, cfg, metrics)
    //          CreatorFn expects (bus, metrics, cfg)
    return Factory::create(type, std::move(bus), std::move(metrics), cfg);
}
