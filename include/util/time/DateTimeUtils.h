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

#include <chrono>
#include <string>

namespace mldp_pvxs_driver::util::time {

/**
 * @brief Small date/time helpers shared by components that need stable UTC formatting.
 */
class DateTimeUtils
{
public:
    /**
     * @brief Truncate a system clock timestamp to millisecond precision.
     */
    static std::chrono::system_clock::time_point truncateToMilliseconds(std::chrono::system_clock::time_point tp);

    /**
     * @brief Format a UTC timestamp as ISO-8601 with millisecond precision.
     *
     * Example: 2026-02-26T20:03:50.123Z
     */
    static std::string formatIso8601UtcMillis(std::chrono::system_clock::time_point tp);
};

} // namespace mldp_pvxs_driver::util::time
