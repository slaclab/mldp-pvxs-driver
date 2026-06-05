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

#include <string>
#include <unordered_map>

#include <util/bus/IDataBus.h>

namespace mldp_pvxs_driver::processor {

struct AlignedSnapshot
{
    std::unordered_map<std::string, util::bus::DataBatch> channels;
    util::bus::BusTimestamp                               reference_time;
};

} // namespace mldp_pvxs_driver::processor
