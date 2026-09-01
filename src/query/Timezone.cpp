//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/Timezone.h>

#include <date/date.h>
#include <date/tz.h>

#include <chrono>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>

using namespace mldp_pvxs_driver::query;

namespace {

int decimal(const std::string_view text, const std::size_t offset, const std::size_t length)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data() + offset, text.data() + offset + length, value);
    if (error != std::errc{} || end != text.data() + offset + length) throw std::invalid_argument("invalid UTC offset: " + std::string(text));
    return value;
}

std::optional<int> fixedOffsetSeconds(const std::string_view value)
{
    if (value.size() != 6 || (value[0] != '+' && value[0] != '-') || value[3] != ':') return std::nullopt;
    const int hours = decimal(value, 1, 2);
    const int minutes = decimal(value, 4, 2);
    if (hours > 23 || minutes > 59) throw std::invalid_argument("invalid UTC offset: " + std::string(value));
    const int seconds = hours * 3600 + minutes * 60;
    return value[0] == '-' ? -seconds : seconds;
}

std::chrono::sys_seconds timestampSeconds(const arrow::TimestampScalar& timestamp)
{
    const auto type = std::dynamic_pointer_cast<arrow::TimestampType>(timestamp.type);
    if (!type) throw std::invalid_argument("from_utc requires an Arrow timestamp");

    switch (type->unit())
    {
        case arrow::TimeUnit::SECOND: return std::chrono::sys_seconds{std::chrono::seconds{timestamp.value}};
        case arrow::TimeUnit::MILLI: return std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{timestamp.value}});
        case arrow::TimeUnit::MICRO: return std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{timestamp.value}});
        case arrow::TimeUnit::NANO: return std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{timestamp.value}});
    }
    throw std::invalid_argument("Unsupported Arrow timestamp unit");
}

std::string offsetText(const int offset_seconds)
{
    const auto absolute = offset_seconds < 0 ? -offset_seconds : offset_seconds;
    const auto hours = absolute / 3600;
    const auto minutes = (absolute % 3600) / 60;
    char result[7];
    result[0] = offset_seconds < 0 ? '-' : '+';
    result[1] = static_cast<char>('0' + hours / 10);
    result[2] = static_cast<char>('0' + hours % 10);
    result[3] = ':';
    result[4] = static_cast<char>('0' + minutes / 10);
    result[5] = static_cast<char>('0' + minutes % 10);
    result[6] = '\0';
    return result;
}

} // namespace

std::string mldp_pvxs_driver::query::fromUtc(const arrow::TimestampScalar& timestamp, const std::string& zone_or_offset)
{
    const auto utc = timestampSeconds(timestamp);
    if (const auto offset = fixedOffsetSeconds(zone_or_offset))
    {
        return date::format("%FT%T", date::local_seconds{utc.time_since_epoch() + std::chrono::seconds{*offset}}) + offsetText(*offset);
    }
    if (!zone_or_offset.empty() && (zone_or_offset.front() == '+' || zone_or_offset.front() == '-'))
    {
        throw std::invalid_argument("invalid UTC offset: " + zone_or_offset + "; expected +/-HH:MM");
    }

    try
    {
        return date::format("%FT%T%Ez", date::make_zoned(date::locate_zone(zone_or_offset), utc));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("unknown IANA timezone '" + zone_or_offset + "': " + error.what());
    }
}
