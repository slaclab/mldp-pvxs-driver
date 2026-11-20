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

#include <cli/LoggingConfig.h>
#include <cli/RotatingGzipFileSink.h>

#include <memory>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::cli {

/// Component category of a named logger.
enum class LogCategory
{
    Other,      ///< Pools, HTTP client, processors, startup: combined file only.
    Reader,
    Writer,
    Controller
};

/// Where a named logger's lines go besides console/combined.
struct LogRoute
{
    LogCategory category{LogCategory::Other};
    std::string fileStem; ///< Sanitized per-component file stem; empty for Other.
};

/**
 * @brief Classify a logger name created via newLogger().
 *
 * - `controller.<name>`            → Controller, `controller-<name>`
 * - `reader:<type>:<name>`         → Reader,     `reader-<type>_<name>`
 * - `<x>_reader...` (no instance)  → Reader,     `reader-<x>_reader...`
 * - `<type>_writer:<name>`         → Writer,     `writer-<type>_<name>`
 * - anything else                  → Other
 */
LogRoute classifyLogger(std::string_view name);

/// Replace every character outside `[A-Za-z0-9._-]` with '_'.
std::string sanitizeLogFileName(std::string_view name);

/**
 * @brief Install file logging: console + combined + per-component files.
 *
 * Replaces the spdlog default logger and the library logger factory
 * (mldp_pvxs_driver::util::log::setLoggerFactory). The level of the current
 * default logger is kept.
 *
 * @return The compressor; keep it alive until after spdlog::shutdown(), then
 *         call stop() so pending gzip jobs finish.
 */
std::shared_ptr<LogCompressor> installFileLogging(const LoggingConfig& cfg, const std::string& pattern);

} // namespace mldp_pvxs_driver::cli
