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

#include <util/log/ILog.h>

#include <mutex>
#include <string>

namespace mldp_pvxs_driver::util::log {

/**
 * @file CoutLogger.h
 * @brief Default fallback logger implementation that writes to std::cout/std::cerr.
 *
 * This is the built-in backend used when the application does not provide its own
 * logging implementation.
 */

/**
 * @brief A minimal logger that writes messages to stdout/stderr.
 *
 * - `Error`/`Critical` go to `std::cerr`, all others to `std::cout`.
 * - If a name is provided, it is included in the prefix.
 * - Thread-safe via an internal mutex.
 */
class CoutLogger final : public ILogger
{
public:
    explicit CoutLogger(std::string name = {});

    void log(Level level, std::string_view message) override;

private:
    std::string name_;
    std::mutex  mu_;
};

} // namespace mldp_pvxs_driver::util::log
