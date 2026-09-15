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

#include <enricher/IPayloadEnricher.h>
#include <util/factory/Factory.h>

namespace mldp_pvxs_driver::enricher {

class EnricherFactory : public util::factory::Factory<EnricherFactory, IPayloadEnricher, const config::Config&>
{
public:
    static constexpr std::string_view kTypeName = "enricher";
    static std::unique_ptr<IPayloadEnricher> create(const std::string& type, const config::Config& config);
};

template <typename EnricherT>
class EnricherRegistrator
{
public:
    explicit EnricherRegistrator(const char* type)
    {
        EnricherFactory::registerType(type, [](const config::Config& config) { return std::make_unique<EnricherT>(config); });
    }
};

#define REGISTER_ENRICHER(TYPE_STRING, CLASSNAME) \
    static inline ::mldp_pvxs_driver::enricher::EnricherRegistrator<CLASSNAME> registrator_{TYPE_STRING};

} // namespace mldp_pvxs_driver::enricher
