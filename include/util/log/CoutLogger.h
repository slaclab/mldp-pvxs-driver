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
