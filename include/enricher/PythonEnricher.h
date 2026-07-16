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

#include <enricher/EnricherFactory.h>

struct _object;
using PyObject = _object;

namespace mldp_pvxs_driver::enricher {

/** Executes a Python module's enrich(batch) function for each payload. */
class PythonEnricher final : public IPayloadEnricher
{
    REGISTER_ENRICHER("python-enricher", PythonEnricher)

public:
    explicit PythonEnricher(const config::Config& config);
    ~PythonEnricher() override;

    void configure(const config::Config& config) override;
    bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;
    std::string enricherType() const override { return "python-enricher"; }

private:
    PyObject* module_{nullptr};
    PyObject* enrich_function_{nullptr};
};

} // namespace mldp_pvxs_driver::enricher

#endif // BUILD_PYTHON_PROCESSOR
