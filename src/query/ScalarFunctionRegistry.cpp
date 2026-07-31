//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/ScalarFunctionRegistry.h>

#include <query/plan/PlannerError.h>

#include <charconv>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;

namespace {

int decimal(const std::string& text, std::size_t offset, std::size_t length)
{
    int value = 0;
    if (offset + length > text.size()) throw std::invalid_argument("invalid timestamp: " + text);
    const auto [end, error] = std::from_chars(text.data() + offset, text.data() + offset + length, value);
    if (error != std::errc{} || end != text.data() + offset + length) throw std::invalid_argument("invalid timestamp: " + text);
    return value;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t localEpochSeconds(const std::string& text)
{
    if (text.size() < 19 || (text[10] != 'T' && text[10] != ' ')) throw std::invalid_argument("invalid timestamp: " + text);
    const int year = decimal(text, 0, 4);
    const int month = decimal(text, 5, 2);
    const int day = decimal(text, 8, 2);
    const int hour = decimal(text, 11, 2);
    const int minute = decimal(text, 14, 2);
    const int second = decimal(text, 17, 2);
    if (text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':' || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        throw std::invalid_argument("invalid timestamp: " + text);
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 + minute * 60 + second;
}

int offsetSeconds(const std::string& text)
{
    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':') throw std::invalid_argument("invalid UTC offset: " + text);
    const int hours = decimal(text, 1, 2);
    const int minutes = decimal(text, 4, 2);
    if (hours > 23 || minutes > 59) throw std::invalid_argument("invalid UTC offset: " + text);
    const int seconds = hours * 3600 + minutes * 60;
    return text[0] == '-' ? -seconds : seconds;
}

} // namespace

ScalarFunctionRegistry::ScalarFunctionRegistry()
{
}

ColumnType ScalarFunctionRegistry::returnType(const FunctionCall& call, const std::vector<ColumnType>& arguments) const
{
    return registry_.resolveFunction(call.name, arguments).inferReturnType(arguments);
}

int64_t ScalarFunctionRegistry::evaluateTimestamp(const FunctionCall& call) const
{
    std::vector<ColumnType> types;
    std::vector<std::string> arguments;
    for (std::size_t index = 0; index < call.arguments.size(); ++index)
    {
        const auto& argument = call.arguments[index];
        if (!argument || !std::holds_alternative<LiteralValue>(argument->value) || !std::holds_alternative<std::string>(std::get<LiteralValue>(argument->value)))
            throw plan::PlannerException(plan::TypeError{.message = call.name + " argument " + std::to_string(index + 1) + " must be STRING"});
        types.push_back(ColumnType::STRING);
        arguments.push_back(std::get<std::string>(std::get<LiteralValue>(argument->value)));
    }
    if (returnType(call, types) != ColumnType::TIMESTAMP) throw plan::PlannerException(plan::TypeError{.message = "Function does not return TIMESTAMP"});
    try
    {
        const auto& timestamp = arguments.front();
        if (arguments.size() == 1)
        {
            if (timestamp.ends_with('Z') || timestamp.ends_with('z')) return localEpochSeconds(timestamp.substr(0, timestamp.size() - 1));
            if (timestamp.size() < 25) throw std::invalid_argument("invalid timestamp: " + timestamp + "; expected Z or explicit UTC offset");
            return localEpochSeconds(timestamp.substr(0, timestamp.size() - 6)) - offsetSeconds(timestamp.substr(timestamp.size() - 6));
        }
        return localEpochSeconds(timestamp) - offsetSeconds(arguments[1]);
    }
    catch (const std::exception& error)
    {
        throw plan::PlannerException(plan::TypeError{.message = error.what()});
    }
}
