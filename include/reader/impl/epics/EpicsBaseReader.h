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
#include <reader/impl/epics/EpicsBaseMonitorPoller.h>
#include <reader/impl/epics/EpicsReaderBase.h>

#include <mutex>

namespace mldp_pvxs_driver::reader::impl::epics {

class EpicsBaseReader : public EpicsReaderBase
{
public:
    EpicsBaseReader(std::shared_ptr<util::bus::IEventBusPush> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const config::Config&                     cfg);
    ~EpicsBaseReader() override;

private:
    void addPV(const PVSet& pvNames);
    void drainEpicsBaseQueue();
    void processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value);

    std::unique_ptr<EpicsBaseMonitorPoller> epics_base_poller_;
    std::mutex                              epics_base_drain_mutex_;

    REGISTER_READER("epics-base", EpicsBaseReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics
