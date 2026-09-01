//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/base/EpicsBaseReaderConfig.h>

using namespace mldp_pvxs_driver::reader::impl::epics;

EpicsBaseReaderConfig::EpicsBaseReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry)
    : EpicsReaderConfig(readerEntry)
{
    monitor_poll_threads_ = static_cast<unsigned int>(readerEntry.getInt(MonitorPollThreadsKey, 2));
    monitor_poll_interval_ms_ = static_cast<unsigned int>(readerEntry.getInt(MonitorPollIntervalMsKey, 5));
}

unsigned int EpicsBaseReaderConfig::monitorPollThreads() const
{
    return monitor_poll_threads_;
}

unsigned int EpicsBaseReaderConfig::monitorPollIntervalMs() const
{
    return monitor_poll_interval_ms_;
}
