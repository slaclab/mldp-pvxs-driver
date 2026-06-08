//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file MLDPChannelProcessorConfig.h
 * @brief Typed parser for per-processor YAML configuration entries.
 */

#pragma once

#include <config/Config.h>
#include <processor/AlignmentPolicy.h>
#include <processor/TriggerPolicy.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class MLDPChannelProcessorConfig
 * @brief Parses and validates one processor configuration block.
 * @details
 * The configuration defines the processor name, required input sources,
 * alignment strategy, trigger policy, and any interval-specific settings
 * needed by the runtime scheduler.
 */
class MLDPChannelProcessorConfig
{
public:
    /**
     * @class Error
     * @brief Exception thrown when a processor configuration is invalid.
     */
    struct Error : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Parse a processor configuration entry.
     * @param[in] cfg YAML config rooted at one processor definition.
     * @throws Error When required fields are missing or values are invalid.
     */
    explicit MLDPChannelProcessorConfig(const config::Config& cfg);

    /** @brief Get the configured processor name. */
    const std::string& name() const noexcept;

    /** @brief Get the ordered list of required input source names. */
    const std::vector<std::string>& sources() const noexcept;

    /** @brief Get the configured alignment policy. */
    AlignmentPolicy alignment() const noexcept;

    /** @brief Get the configured trigger policy. */
    TriggerPolicy trigger() const noexcept;

    /**
     * @brief Get the interval trigger period in seconds.
     * @return Positive interval in seconds when interval triggering is used; otherwise 0.0.
     */
    double triggerIntervalSec() const noexcept;

    /** @brief Get the maximum retained buffer depth per source; zero means unlimited. */
    std::size_t maxBufferDepth() const noexcept;

private:
    std::string              name_;                  ///< Unique processor instance name.
    std::vector<std::string> sources_;               ///< Required source names consumed by the processor.
    AlignmentPolicy          alignment_{AlignmentPolicy::LatestValue}; ///< Snapshot alignment policy.
    TriggerPolicy            trigger_{TriggerPolicy::AnyUpdate}; ///< Snapshot trigger policy.
    double                   trigger_interval_sec_{0.0}; ///< Trigger period in seconds for interval mode.
    std::size_t              max_buffer_depth_{0}; ///< Maximum retained samples per input source; zero means unlimited.
};

} // namespace mldp_pvxs_driver::processor
