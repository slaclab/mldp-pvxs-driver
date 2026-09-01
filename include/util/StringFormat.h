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

#include <sstream>
#include <string>

// Check for C++20 <format> support
#if __cplusplus >= 202002L && __has_include(<format>)
    #include <format>
    #define HAVE_STD_FORMAT 1
// Re-export format utilities for convenience
using std::format_error;
using std::make_format_args;
using std::vformat;
#else
    #define HAVE_STD_FORMAT 0
#endif

namespace mldp_pvxs_driver::util {

#if HAVE_STD_FORMAT
/**
 * Use std::format when C++20 is available.
 * Supports {} placeholders for format arguments.
 * Example: auto msg = format_string("PV {} on reader {}", pvName, readerName);
 */
template <typename... Args>
inline std::string format_string(std::format_string<Args...> fmt, Args&&... args)
{
    return std::format(fmt, std::forward<Args>(args)...);
}
#else
/**
 * Fallback for older C++ standards.
 * Supports {} placeholders that are replaced with variadic arguments.
 * Example: auto msg = format_string("PV {} on reader {}", pvName, readerName);
 */
namespace detail {
    template <typename T>
    inline void append_to_stream(std::stringstream& ss, const T& value)
    {
        ss << value;
    }

    // Helper to replace {} placeholders with arguments
    template <typename FirstArg, typename... RestArgs>
    inline std::string replace_placeholders(const std::string& fmt, const FirstArg& first, const RestArgs&... rest)
    {
        std::stringstream ss;
        ss << first;
        std::string arg_str = ss.str();

        size_t pos = fmt.find("{}");
        if (pos == std::string::npos)
        {
            return fmt;
        }

        std::string result = fmt.substr(0, pos) + arg_str + fmt.substr(pos + 2);

        if constexpr (sizeof...(rest) > 0)
        {
            return replace_placeholders(result, rest...);
        }
        return result;
    }
} // namespace detail

template <typename... Args>
inline std::string format_string(const std::string& fmt, Args&&... args)
{
    if constexpr (sizeof...(args) == 0)
    {
        return fmt;
    }
    else
    {
        return detail::replace_placeholders(fmt, std::forward<Args>(args)...);
    }
}
#endif

} // namespace mldp_pvxs_driver::util
