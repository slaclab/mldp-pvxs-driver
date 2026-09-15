//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/impl/TimestampClampEnricher.h>

#include <algorithm>
#include <cstdint>

namespace mldp_pvxs_driver::enricher {

bool TimestampClampEnricher::enrich(util::bus::IDataBus::EventBatch& batch) noexcept
{
    if (!util::bus::isTimeSeries(batch))
        return true;

    for (auto& frame : std::get<util::bus::TimeSeriesPayload>(batch.payload).frames)
    {
        for (auto& timestamp : frame.timestamps)
            timestamp.nanoseconds = std::min<uint64_t>(timestamp.nanoseconds, 999999999U);
    }
    return true;
}

} // namespace mldp_pvxs_driver::enricher
