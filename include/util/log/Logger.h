#pragma once

#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::util::log {

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

class ILogger
{
public:
    virtual ~ILogger() = default;

    virtual void log(Level level, std::string_view message) = 0;

    // Optional fast-path for callers to avoid formatting work.
    virtual bool shouldLog(Level /*level*/) const
    {
        return true;
    }
};

using LoggerFactory = std::function<std::shared_ptr<ILogger>(std::string_view name)>;

namespace detail {

inline const char* toString(Level level)
{
    switch (level)
    {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Critical: return "critical";
    case Level::Off: return "off";
    }
    return "unknown";
}

class CoutLogger final : public ILogger
{
public:
    explicit CoutLogger(std::string name = {})
        : name_(std::move(name))
    {
    }

    void log(Level level, std::string_view message) override
    {
        std::lock_guard<std::mutex> lock(mu_);

        std::ostream& out = (level == Level::Error || level == Level::Critical) ? std::cerr : std::cout;
        out << "[" << toString(level) << "]";
        if (!name_.empty())
        {
            out << " [" << name_ << "]";
        }
        out << " " << message << "\n";
        out.flush();
    }

private:
    std::string name_;
    std::mutex mu_;
};

inline std::mutex& globalMutex()
{
    static std::mutex mu;
    return mu;
}

inline std::shared_ptr<ILogger>& globalLogger()
{
    static std::shared_ptr<ILogger> logger = std::make_shared<CoutLogger>();
    return logger;
}

inline LoggerFactory& globalFactory()
{
    static LoggerFactory factory = [](std::string_view name) -> std::shared_ptr<ILogger>
    {
        if (name.empty())
        {
            return detail::globalLogger();
        }
        return std::static_pointer_cast<ILogger>(std::make_shared<CoutLogger>(std::string(name)));
    };
    return factory;
}

} // namespace detail

inline void setLogger(std::shared_ptr<ILogger> logger)
{
    std::lock_guard<std::mutex> lock(detail::globalMutex());
    if (logger)
    {
        detail::globalLogger() = std::move(logger);
    }
    else
    {
        detail::globalLogger() = std::make_shared<detail::CoutLogger>();
    }

    // Default behavior: if the user only sets a single logger implementation,
    // treat it as the factory result for any name.
    detail::globalFactory() = [](std::string_view /*name*/) -> std::shared_ptr<ILogger>
    {
        return detail::globalLogger();
    };
}

inline void setLoggerFactory(LoggerFactory factory)
{
    std::lock_guard<std::mutex> lock(detail::globalMutex());
    if (factory)
    {
        detail::globalFactory() = std::move(factory);
    }
    else
    {
        detail::globalFactory() = [](std::string_view name) -> std::shared_ptr<ILogger>
        {
            if (name.empty())
            {
                return detail::globalLogger();
            }
            return std::static_pointer_cast<ILogger>(std::make_shared<detail::CoutLogger>(std::string(name)));
        };
    }
}

inline ILogger& getLogger()
{
    std::lock_guard<std::mutex> lock(detail::globalMutex());
    return *detail::globalLogger();
}

inline std::shared_ptr<ILogger> getLoggerShared()
{
    std::lock_guard<std::mutex> lock(detail::globalMutex());
    return detail::globalLogger();
}

inline std::shared_ptr<ILogger> newLogger(std::string_view name)
{
    LoggerFactory factory;
    {
        std::lock_guard<std::mutex> lock(detail::globalMutex());
        factory = detail::globalFactory();
    }

    if (factory)
    {
        if (auto created = factory(name))
        {
            return created;
        }
    }

    // Hard fallback.
    if (name.empty())
    {
        return getLoggerShared();
    }
    return std::make_shared<detail::CoutLogger>(std::string(name));
}

inline void log(Level level, std::string_view message)
{
    auto& logger = getLogger();
    if (!logger.shouldLog(level))
    {
        return;
    }
    logger.log(level, message);
}

template <typename... Args>
inline void logf(Level level, std::string_view fmt, Args&&... args)
{
    auto& logger = getLogger();
    if (!logger.shouldLog(level))
    {
        return;
    }

    try
    {
        // libstdc++'s std::make_format_args expects lvalue references.
        // Named parameters are lvalues, so pass without forwarding.
        logger.log(level, std::vformat(fmt, std::make_format_args(args...)));
    }
    catch (const std::format_error& ex)
    {
        logger.log(Level::Error, std::string("Log format error: ") + ex.what());
        logger.log(level, fmt);
    }
}

// Convenience level helpers (global logger)
inline void trace(std::string_view message) { log(Level::Trace, message); }
inline void debug(std::string_view message) { log(Level::Debug, message); }
inline void info(std::string_view message) { log(Level::Info, message); }
inline void warn(std::string_view message) { log(Level::Warn, message); }
inline void error(std::string_view message) { log(Level::Error, message); }
inline void critical(std::string_view message) { log(Level::Critical, message); }

template <typename... Args>
inline void tracef(std::string_view fmt, Args&&... args) { logf(Level::Trace, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void debugf(std::string_view fmt, Args&&... args) { logf(Level::Debug, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void infof(std::string_view fmt, Args&&... args) { logf(Level::Info, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void warnf(std::string_view fmt, Args&&... args) { logf(Level::Warn, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void errorf(std::string_view fmt, Args&&... args) { logf(Level::Error, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void criticalf(std::string_view fmt, Args&&... args) { logf(Level::Critical, fmt, std::forward<Args>(args)...); }

// Overloads for an explicit logger instance (useful for helper functions)
inline void log(ILogger& logger, Level level, std::string_view message)
{
    if (!logger.shouldLog(level))
    {
        return;
    }
    logger.log(level, message);
}

template <typename... Args>
inline void logf(ILogger& logger, Level level, std::string_view fmt, Args&&... args)
{
    if (!logger.shouldLog(level))
    {
        return;
    }

    try
    {
        logger.log(level, std::vformat(fmt, std::make_format_args(args...)));
    }
    catch (const std::format_error& ex)
    {
        logger.log(Level::Error, std::string("Log format error: ") + ex.what());
        logger.log(level, fmt);
    }
}

template <typename... Args>
inline void tracef(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Trace, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void debugf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Debug, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void infof(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Info, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void warnf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Warn, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void errorf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Error, fmt, std::forward<Args>(args)...); }

template <typename... Args>
inline void criticalf(ILogger& logger, std::string_view fmt, Args&&... args) { logf(logger, Level::Critical, fmt, std::forward<Args>(args)...); }

} // namespace mldp_pvxs_driver::util::log
