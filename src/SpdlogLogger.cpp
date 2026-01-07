//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "SpdlogLogger.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace mldp_pvxs_driver::cli {

SpdlogLogger::SpdlogLogger(std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger))
{
}

void SpdlogLogger::setLevel(mldp_pvxs_driver::util::log::Level level)
{
    if (!logger_)
    {
        return;
    }
    logger_->set_level(toSpd(level));
}

bool SpdlogLogger::shouldLog(mldp_pvxs_driver::util::log::Level level) const
{
    if (!logger_)
    {
        return false;
    }
    return logger_->should_log(toSpd(level));
}

void SpdlogLogger::log(mldp_pvxs_driver::util::log::Level level, std::string_view message)
{
    if (!logger_)
    {
        return;
    }
    logger_->log(toSpd(level), "{}", message);
}

spdlog::level::level_enum SpdlogLogger::toSpd(mldp_pvxs_driver::util::log::Level level)
{
    using L = mldp_pvxs_driver::util::log::Level;
    switch (level)
    {
    case L::Trace: return spdlog::level::trace;
    case L::Debug: return spdlog::level::debug;
    case L::Info: return spdlog::level::info;
    case L::Warn: return spdlog::level::warn;
    case L::Error: return spdlog::level::err;
    case L::Critical: return spdlog::level::critical;
    case L::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace mldp_pvxs_driver::cli
