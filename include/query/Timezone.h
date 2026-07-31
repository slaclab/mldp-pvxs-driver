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

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <string>

namespace mldp_pvxs_driver::query {

/** Formats a UTC Arrow timestamp in an IANA timezone or fixed UTC offset. */
[[nodiscard]] std::string fromUtc(const arrow::TimestampScalar& timestamp, const std::string& zone_or_offset);

} // namespace mldp_pvxs_driver::query
