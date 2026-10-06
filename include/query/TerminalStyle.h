//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file TerminalStyle.h
 * @brief ANSI styling helpers for interactive query console output. */
#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace mldp_pvxs_driver::cli::style {

inline constexpr std::string_view reset     = "\x1b[0m";
inline constexpr std::string_view bold      = "\x1b[1m";
inline constexpr std::string_view dim       = "\x1b[2m";
inline constexpr std::string_view red       = "\x1b[1;31m";
inline constexpr std::string_view yellow    = "\x1b[33m";
inline constexpr std::string_view green     = "\x1b[1;32m";
inline constexpr std::string_view cyan      = "\x1b[1;36m";
inline constexpr std::string_view magenta   = "\x1b[35m";

/** @brief Returns true when ANSI color should be used by default for @p stream.
 * Color is enabled only for stdout/stderr attached to a TTY and when NO_COLOR is unset. */
inline bool colorEnabledByDefault(const std::ostream& stream)
{
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color != nullptr && *no_color != '\0') return false;
    if (&stream == &std::cout) return ::isatty(STDOUT_FILENO) != 0;
    if (&stream == &std::cerr) return ::isatty(STDERR_FILENO) != 0;
    return false;
}

/** @brief Wraps @p text in @p code and a reset sequence when @p on is true. */
inline std::string paint(const bool on, const std::string_view code, const std::string_view text)
{
    if (!on) return std::string(text);
    std::string result;
    result.reserve(code.size() + text.size() + reset.size());
    result.append(code).append(text).append(reset);
    return result;
}

} // namespace mldp_pvxs_driver::cli::style
