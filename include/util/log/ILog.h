#pragma once

#include <functional>
#include <memory>
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
