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
 * @file IAlgorithm.h
 * @brief Interface for processor algorithms that transform aligned input data.
 */

#pragma once

#include <config/Config.h>
#include <processor/AlgorithmOutput.h>
#include <processor/AlignedSnapshot.h>

#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class IAlgorithm
 * @brief Abstract interface implemented by channel-processing algorithms.
 * @details
 * Implementations consume aligned source snapshots and produce one or more
 * virtual output payloads. Instances are configured once from the algorithm
 * subtree of a processor configuration and then reused across compute calls.
 */
class IAlgorithm
{
public:
    /** @brief Destroy the algorithm instance through the interface. */
    virtual ~IAlgorithm() = default;

    /**
     * @brief Load algorithm-specific configuration from the processor config.
     * @details
     * Base processor fields are parsed elsewhere; implementations should read
     * only their own algorithm-specific keys from @p cfg.
     * @param[in] cfg Parsed YAML view rooted at the processor configuration.
     */
    virtual void configure(const config::Config& cfg) = 0;

    /**
     * @brief Return the virtual source names emitted by this algorithm.
     * @return Ordered list of output source identifiers published by compute().
     */
    virtual std::vector<std::string> outputSources() const noexcept = 0;

    /**
     * @brief Compute output payloads from an aligned input snapshot.
     * @param[in] snapshot Source batches aligned for one processing step.
     * @return Output payloads ready to publish onto the data bus.
     */
    virtual std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) = 0;

    /**
     * @brief Return the configuration type name used to select this algorithm.
     * @return Stable algorithm type identifier.
     */
    virtual std::string algorithmType() const noexcept = 0;

    /**
     * @brief Reset any algorithm-maintained runtime state between processing windows.
     * @details
     * Stateless algorithms can keep the default no-op implementation.
     */
    virtual void reset() noexcept {}
};

using IAlgorithmUPtr = std::unique_ptr<IAlgorithm>;

} // namespace mldp_pvxs_driver::processor
