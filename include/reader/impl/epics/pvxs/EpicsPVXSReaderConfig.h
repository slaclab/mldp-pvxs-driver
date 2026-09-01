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

class EpicsPVXSReaderConfig : public EpicsReaderConfig
{
public:
    EpicsPVXSReaderConfig() = default;
    explicit EpicsPVXSReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
