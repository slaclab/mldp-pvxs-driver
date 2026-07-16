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

#include <enricher/EnricherFactory.h>

#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::enricher {

class ColumnAttributesEnricher final : public IPayloadEnricher
{
    REGISTER_ENRICHER("column-attributes", ColumnAttributesEnricher)

public:
    explicit ColumnAttributesEnricher(const config::Config& config);

    void configure(const config::Config& config) override;
    bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;

    std::string enricherType() const override
    {
        return "column-attributes";
    }

private:
    std::string                                  pattern_;
    std::unordered_map<std::string, std::string> attributes_;
};

} // namespace mldp_pvxs_driver::enricher
