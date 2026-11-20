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

#include <config/Config.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace mldp_pvxs_driver::cli {

/**
 * @brief Rotation/compression policy shared by every log file.
 */
struct LogRotationPolicy
{
    /// Rotate when the active file reaches this many bytes. 0 disables size rotation.
    std::uint64_t maxBytes{100ULL * 1024ULL * 1024ULL};

    /// Rotate when the active file has been open this long. 0 disables age rotation.
    std::chrono::seconds maxAge{0};

    /// gzip rotated files in the background.
    bool compress{true};

    /// Rotated/previous files kept per log stem (active file not counted). 0 = unlimited.
    int maxFiles{10};
};

/**
 * @brief File-logging configuration parsed from the optional top-level `logging:` block.
 *
 * Without the block (or without `log-file`) the driver logs to the console only.
 * Per-component files are written in the same folder as `log-file`.
 *
 * @code{.yaml}
 * logging:
 *   log-file: logs/driver.log    # main log file (every line); CLI --log-file overrides
 *   console: true                # keep console output; default true
 *   per-component:               # own file per component, next to log-file; default all false
 *     readers: false
 *     writers: false
 *     controller: false
 *   per-run: true                # file names include the run start time; default true
 *   rotation:
 *     max-size-mb: 100           # 0 = off; default 100
 *     max-age-hours: 0           # 0 = off; default 0
 *     compress: true             # default true
 *     max-files: 10              # 0 = unlimited; default 10
 * @endcode
 */
struct LoggingConfig
{
    /// Main log file path (receives every line). Empty = no main file.
    std::string logFile;

    bool console{true};
    bool perReaders{false};
    bool perWriters{false};
    bool perController{false};
    bool perRun{true};

    LogRotationPolicy rotation;

    bool anyPerComponent() const
    {
        return perReaders || perWriters || perController;
    }

    /// True when any file output is configured.
    bool fileLoggingEnabled() const
    {
        return !logFile.empty();
    }

    /// Folder holding the main log file ("." when the path has no folder).
    std::string logFileDirectory() const;

    /// Main log file name without a trailing ".log" (used as the file stem).
    std::string logFileStem() const;

    /**
     * @brief Parse the `logging:` block from the root driver configuration.
     *
     * Missing block → defaults (console only).
     *
     * @param logFileOverride  Non-empty value (CLI --log-file) replaces `log-file`
     *                         before validation.
     *
     * @throws std::runtime_error on invalid values, or when per-component files
     *         are enabled without `log-file`.
     */
    static LoggingConfig parse(const config::Config& root, const std::string& logFileOverride = "");

    /// Consistency checks (also run by parse()).
    void validate() const;

private:
    static LoggingConfig parseBlock(const config::Config& root);
};

} // namespace mldp_pvxs_driver::cli
