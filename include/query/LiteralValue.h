//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file LiteralValue.h
 * @brief Defines strongly typed temporal query literal wrappers. */
#pragma once

#include <cstdint>

namespace mldp_pvxs_driver::query {

// Explicit wrappers preserve temporal units and prevent timestamps and
// durations from being confused with ordinary signed numeric literals.
/** @brief Signed nanoseconds since the Unix epoch. */
struct TimestampNsLiteral {
    int64_t value;
};

/** @brief Signed duration expressed in nanoseconds. */
struct DurationNsLiteral {
    int64_t value;
};

} // namespace mldp_pvxs_driver::query
