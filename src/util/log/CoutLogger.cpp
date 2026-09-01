//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/log/CoutLogger.h>

#include <iostream>
#include <memory>

namespace mldp_pvxs_driver::util::log {

namespace {

    const char* toString(Level level)
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

    std::mutex& globalMutex()
    {
        static std::mutex mu;
        return mu;
    }

    std::shared_ptr<ILogger>& globalLogger()
    {
        static std::shared_ptr<ILogger> logger = std::make_shared<CoutLogger>();
        return logger;
    }

    LoggerFactory& globalFactory()
    {
        static LoggerFactory factory = [](std::string_view name) -> std::shared_ptr<ILogger>
        {
            if (name.empty())
            {
                return globalLogger();
            }
            return std::static_pointer_cast<ILogger>(std::make_shared<CoutLogger>(std::string(name)));
        };
        return factory;
    }

} // namespace

CoutLogger::CoutLogger(std::string name)
    : name_(std::move(name))
{
}

void CoutLogger::setLevel(Level level)
{
    std::lock_guard<std::mutex> lock(mu_);
    min_level_ = level;
}

bool CoutLogger::shouldLog(Level level) const
{
    std::lock_guard<std::mutex> lock(mu_);

    // Off disables all logging.
    if (min_level_ == Level::Off)
    {
        return false;
    }

    // Treat enum order as increasing severity.
    return static_cast<int>(level) >= static_cast<int>(min_level_);
}

void CoutLogger::log(Level level, std::string_view message)
{
    std::lock_guard<std::mutex> lock(mu_);

    // Apply filtering without re-entering shouldLog() (would deadlock).
    if (min_level_ == Level::Off || static_cast<int>(level) < static_cast<int>(min_level_))
    {
        return;
    }

    std::ostream& out = (level == Level::Error || level == Level::Critical) ? std::cerr : std::cout;
    out << "[" << toString(level) << "]";
    if (!name_.empty())
    {
        out << " [" << name_ << "]";
    }
    out << " " << message << "\n";
    out.flush();
}

void setLogger(std::shared_ptr<ILogger> logger)
{
    std::lock_guard<std::mutex> lock(globalMutex());
    if (logger)
    {
        globalLogger() = std::move(logger);
    }
    else
    {
        globalLogger() = std::make_shared<CoutLogger>();
    }

    // Default behavior: if the user only sets a single logger implementation,
    // treat it as the factory result for any name.
    globalFactory() = [](std::string_view /*name*/) -> std::shared_ptr<ILogger>
    {
        return globalLogger();
    };
}

void setLoggerFactory(LoggerFactory factory)
{
    std::lock_guard<std::mutex> lock(globalMutex());
    if (factory)
    {
        globalFactory() = std::move(factory);
    }
    else
    {
        globalFactory() = [](std::string_view name) -> std::shared_ptr<ILogger>
        {
            if (name.empty())
            {
                return globalLogger();
            }
            return std::static_pointer_cast<ILogger>(std::make_shared<CoutLogger>(std::string(name)));
        };
    }
}

ILogger& getLogger()
{
    // Keep the returned reference valid for the duration of the calling thread's use.
    thread_local std::shared_ptr<ILogger> tls;

    {
        std::lock_guard<std::mutex> lock(globalMutex());
        tls = globalLogger();
    }

    return *tls;
}

std::shared_ptr<ILogger> getLoggerShared()
{
    std::lock_guard<std::mutex> lock(globalMutex());
    return globalLogger();
}

std::shared_ptr<ILogger> newLogger(std::string_view name)
{
    LoggerFactory factory;
    {
        std::lock_guard<std::mutex> lock(globalMutex());
        factory = globalFactory();
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
    return std::make_shared<CoutLogger>(std::string(name));
}

} // namespace mldp_pvxs_driver::util::log
