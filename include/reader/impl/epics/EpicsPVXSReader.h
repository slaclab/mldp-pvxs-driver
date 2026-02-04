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

#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsReaderBase.h>

#include <pvxs/client.h>

namespace mldp_pvxs_driver::reader::impl::epics {

class EpicsPVXSReader : public EpicsReaderBase
{
public:
    EpicsPVXSReader(std::shared_ptr<util::bus::IEventBusPush> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const config::Config&                     cfg);

private:
    void addPV(const PVSet& pvNames);
    void processEvent(std::string pvName, pvxs::Value epics_value);

    pvxs::client::Context pva_context_;
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;

    REGISTER_READER("epics-pvxs", EpicsPVXSReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics
