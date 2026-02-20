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

#include <util/log/Logger.h>

#include <memory>

namespace spdlog {
class logger;
namespace level {
enum level_enum : int;
}
} // namespace spdlog

namespace mldp_pvxs_driver::cli {

/**
 * @brief ILogger implementation backed by spdlog.
 *
 * CLI-only adapter used by the driver executable to route library logging
 * through the spdlog backend.
 */
class SpdlogLogger final : public mldp_pvxs_driver::util::log::ILogger
{
public:
    using mldp_pvxs_driver::util::log::ILogger::setLevel;

    explicit SpdlogLogger(std::shared_ptr<spdlog::logger> logger);

    void setLevel(mldp_pvxs_driver::util::log::Level level) override;
    bool shouldLog(mldp_pvxs_driver::util::log::Level level) const override;
    void log(mldp_pvxs_driver::util::log::Level level, std::string_view message) override;

private:
    static spdlog::level::level_enum toSpd(mldp_pvxs_driver::util::log::Level level);

    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace mldp_pvxs_driver::cli
