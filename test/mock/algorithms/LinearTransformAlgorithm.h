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
 * @file LinearTransformAlgorithm.h
 * @brief Weighted-sum algorithm for deriving one virtual source from inputs.
 */

#pragma once

#include <processor/IAlgorithm.h>

#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class LinearTransformAlgorithm
 * @brief Computes a weighted linear combination of configured input sources.
 */
class LinearTransformAlgorithm final : public IAlgorithm
{
public:
    void configure(const config::Config& cfg) override;
    std::vector<std::string> outputSources() const noexcept override;
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override;

private:
    std::string         output_source_;
    std::vector<std::string> sources_;
    std::vector<double> coefficients_;
    double              bias_{0.0};
    std::string         output_column_{"result"};
};

} // namespace mldp_pvxs_driver::processor
