//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file MldpTimestampUtils.h
 * @brief Shared timestamp conversion utilities for MLDP query implementations. */
#pragma once

#include <common.pb.h>

#include <cstdint>

namespace mldp_pvxs_driver::query::impl::mldp {

inline int64_t timestampToNanoseconds(const dp::service::common::Timestamp& ts)
{
    return static_cast<int64_t>(ts.epochseconds()) * 1'000'000'000LL +
           static_cast<int64_t>(ts.nanoseconds());
}

inline void setTimestamp(dp::service::common::Timestamp* target, const int64_t seconds)
{
    target->set_epochseconds(static_cast<uint64_t>(seconds));
    target->set_nanoseconds(0);
}

} // namespace mldp_pvxs_driver::query::impl::mldp
