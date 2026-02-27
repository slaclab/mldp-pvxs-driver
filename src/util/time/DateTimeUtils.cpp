//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/time/DateTimeUtils.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <stdexcept>

using namespace mldp_pvxs_driver::util::time;

bool DateTimeUtils::isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

std::chrono::system_clock::time_point DateTimeUtils::truncateToMilliseconds(std::chrono::system_clock::time_point tp)
{
    return std::chrono::time_point_cast<std::chrono::milliseconds>(tp);
}

std::string DateTimeUtils::formatIso8601UtcMillis(std::chrono::system_clock::time_point tp)
{
    tp = truncateToMilliseconds(tp);
    const auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
    const auto sec_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(ms_since_epoch);
    const auto ms_part = static_cast<long>((ms_since_epoch - sec_since_epoch).count());

    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(sec_since_epoch));
    std::tm     tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif

    char buf[32];
    std::snprintf(buf,
                  sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  ms_part);
    return std::string(buf);
}

uint64_t DateTimeUtils::unixEpochSecondsFromYearAndSecondsIntoYear(int year, uint32_t seconds_into_year)
{
    if (year < 1970)
    {
        throw std::runtime_error("PB/HTTP sample year before 1970 is unsupported");
    }

    uint64_t days = 0;
    for (int y = 1970; y < year; ++y)
    {
        days += isLeapYear(y) ? 366u : 365u;
    }
    return days * 86400ULL + static_cast<uint64_t>(seconds_into_year);
}
