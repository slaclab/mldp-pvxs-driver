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

#ifdef BUILD_PYTHON_PROCESSOR

#include <processor/IAlgorithm.h>

#include <optional>
#include <string>
#include <vector>

struct _object;
using PyObject = _object;

namespace mldp_pvxs_driver::processor {

class PythonAlgorithm final : public IAlgorithm
{
public:
    explicit PythonAlgorithm(PyObject* module);
    ~PythonAlgorithm() override;

    void configure(const config::Config& cfg) override;
    std::vector<std::string> outputSources() const noexcept override;
    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override;
    std::string algorithmType() const noexcept override { return "python"; }
    void reset() noexcept override;

private:
    std::optional<AlgorithmOutput> payloadFromPyObject(PyObject* obj,
                                                       const util::bus::BusTimestamp& reference_time) const;

    PyObject*                module_{nullptr};
    PyObject*                compute_fn_{nullptr};
    std::vector<std::string> output_sources_;
};

} // namespace mldp_pvxs_driver::processor

#endif // BUILD_PYTHON_PROCESSOR
