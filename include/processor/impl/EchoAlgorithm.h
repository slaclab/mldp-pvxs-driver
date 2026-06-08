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
 * @file EchoAlgorithm.h
 * @brief Declares the optional pass-through processor algorithm used for pipeline smoke tests.
 */

#pragma once

#ifdef BUILD_ECHO_PROCESSOR

#include <processor/IAlgorithm.h>

#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class EchoAlgorithm
 * @brief Re-emits the first aligned input channel unchanged under a configurable virtual output source.
 * @details
 * This lightweight algorithm is intended for debugging and smoke-testing the
 * processor pipeline when a full derived computation is not needed.
 */
class EchoAlgorithm final : public IAlgorithm
{
public:
    /**
     * @brief Configure the emitted output source name for the echo stream.
     * @param[in] cfg Processor configuration containing `output-source` and/or `sources`.
     */
    void configure(const config::Config& cfg) override;

    /**
     * @brief Return the single virtual source produced by this algorithm.
     * @return One-element list containing the configured echo output source name.
     */
    std::vector<std::string> outputSources() const noexcept override { return {output_source_}; }

    /**
     * @brief Copy the first available aligned input channel into one output payload.
     * @param[in] snapshot Aligned processor snapshot containing zero or more source batches.
     * @return Empty when @p snapshot has no channels, otherwise one echoed output payload.
     */
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;

    /**
     * @brief Return the stable configuration type name for this algorithm.
     * @return The literal string `echo`.
     */
    std::string algorithmType() const noexcept override { return "echo"; }

private:
    std::string output_source_; ///< Virtual source name used for emitted echo payloads.
};

} // namespace mldp_pvxs_driver::processor

#endif // BUILD_ECHO_PROCESSOR
