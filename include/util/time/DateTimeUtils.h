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
#include <cstdint>
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

    /**
     * @brief Return true if @p year is a Gregorian leap year.
     */
    static bool isLeapYear(int year);

    /**
     * @brief Convert an EPICS Archiver Appliance PB/HTTP year + seconds-into-year
     *        to a UNIX epoch seconds value.
     *
     * The Archiver Appliance encodes sample timestamps as a (year, secondsintoyear)
     * pair relative to the start of that calendar year in UTC.  This function
     * accumulates the days for each year from 1970 up to (but not including)
     * @p year, accounting for leap years, and adds @p seconds_into_year.
     *
     * @param year             Calendar year (must be >= 1970).
     * @param seconds_into_year Elapsed seconds since midnight UTC on 1 Jan of @p year.
     * @return UNIX epoch seconds.
     * @throws std::runtime_error if @p year is before 1970.
     */
    static uint64_t unixEpochSecondsFromYearAndSecondsIntoYear(int year, uint32_t seconds_into_year);
};

} // namespace mldp_pvxs_driver::util::time
