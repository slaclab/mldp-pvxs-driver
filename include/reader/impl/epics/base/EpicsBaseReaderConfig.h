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

#include <reader/impl/epics/shared/EpicsReaderConfig.h>

namespace mldp_pvxs_driver::reader::impl::epics {

class EpicsBaseReaderConfig : public EpicsReaderConfig
{
public:
    EpicsBaseReaderConfig() = default;
    explicit EpicsBaseReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry);

    unsigned int monitorPollThreads() const;
    unsigned int monitorPollIntervalMs() const;

private:
    unsigned int monitor_poll_threads_{2};
    unsigned int monitor_poll_interval_ms_{5};
};

} // namespace mldp_pvxs_driver::reader::impl::epics
