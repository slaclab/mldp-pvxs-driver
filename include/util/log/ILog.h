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

#include <cctype>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::util::log {

/**
 * @file ILog.h
 * @brief Logging interface for the driver library.
 *
 * This header defines the *abstraction* used by the driver library for logging.
 *
 * Design goals:
 * - The driver library must not depend on a particular logging backend.
 * - The executable (or embedding application) selects and installs the logging implementation.
 * - Library components may request *named* loggers (e.g. per reader instance) via newLogger(name).
 *
 * Typical usage from an executable:
 * @code
 * // In main(): install a logger implementation and an optional factory.
 * mldp_pvxs_driver::util::log::setLogger(myLogger);
 * mldp_pvxs_driver::util::log::setLoggerFactory([](std::string_view name) {
 *     return makeLoggerWithName(name);
 * });
 * @endcode
 *
 * Typical usage from library code:
 * @code
 * auto log = mldp_pvxs_driver::util::log::newLogger("epics_reader:my_reader");
 * log->log(mldp_pvxs_driver::util::log::Level::Info, "started");
 * @endcode
 */

/**
 * @brief Log severity levels.
 */
enum class Level
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

/**
 * @brief Parse a user-provided log level string.
 *
 * Accepted values (case-insensitive): trace, debug, info, warn|warning,
 * error|err, critical|fatal, off.
 *
 * @throws std::invalid_argument if the value is not recognized.
 */
inline Level parseLevel(std::string_view value)
{
    const auto iequals = [](std::string_view a, std::string_view b) -> bool
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb))
            {
                return false;
            }
        }
        return true;
    };

    if (iequals(value, "trace"))
        return Level::Trace;
    if (iequals(value, "debug"))
        return Level::Debug;
    if (iequals(value, "info"))
        return Level::Info;
    if (iequals(value, "warn") || iequals(value, "warning"))
        return Level::Warn;
    if (iequals(value, "error") || iequals(value, "err"))
        return Level::Error;
    if (iequals(value, "critical") || iequals(value, "fatal"))
        return Level::Critical;
    if (iequals(value, "off"))
        return Level::Off;

    throw std::invalid_argument(
        "Invalid log level '" + std::string(value)
        + "' (expected: trace, debug, info, warn, error, critical, off)");
}

/**
 * @brief Abstract logger interface used by the driver library.
 *
 * Implementations are expected to be thread-safe.
 */
class ILogger
{
public:
    virtual ~ILogger() = default;

    /**
     * @brief Emit a pre-formatted log message.
     * @param level Severity level.
     * @param message Message payload.
     */
    virtual void log(Level level, std::string_view message) = 0;

    /**
     * @brief Set the minimum log level.
     *
     * Default implementation is a no-op.
     */
    virtual void setLevel(Level /*level*/) {}

    /**
     * @brief Convenience overload: set level from a user-provided string.
     *
     * Default implementation parses the string via @ref parseLevel and calls
     * @ref setLevel(Level).
     */
    virtual void setLevel(std::string_view level)
    {
        setLevel(parseLevel(level));
    }

    /**
     * @brief Optional fast-path to avoid formatting work.
     *
     * If this returns false, callers may skip message construction.
     */
    virtual bool shouldLog(Level /*level*/) const
    {
        return true;
    }
};

/**
 * @brief Factory for creating named loggers.
 *
 * The library calls this via newLogger(name). The embedding application can use it
 * to create per-component loggers (e.g. `spdlog::logger` with distinct names).
 */
using LoggerFactory = std::function<std::shared_ptr<ILogger>(std::string_view name)>;

/**
 * @brief Install the global/default logger.
 *
 * If `logger` is null, a default std::cout/std::cerr logger is installed.
 *
 * Note: calling setLogger() also sets the factory to "always return the global logger"
 * unless a factory is explicitly installed via setLoggerFactory().
 */
void setLogger(std::shared_ptr<ILogger> logger);

/**
 * @brief Install a factory used to create named loggers.
 *
 * If `factory` is empty, a default factory is used.
 */
void setLoggerFactory(LoggerFactory factory);

/**
 * @brief Get the current global/default logger.
 */
ILogger& getLogger();

/**
 * @brief Get a shared_ptr to the current global/default logger.
 */
std::shared_ptr<ILogger> getLoggerShared();

/**
 * @brief Create or retrieve a named logger.
 *
 * This calls the installed LoggerFactory. If no factory is installed, a default
 * std::cout/std::cerr logger is returned (named if `name` is non-empty).
 */
std::shared_ptr<ILogger> newLogger(std::string_view name);

} // namespace mldp_pvxs_driver::util::log
