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

#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::enricher {

class EnricherRegistry
{
public:
    explicit EnricherRegistry(const config::Config& root);
    std::vector<IPayloadEnricherPtr> resolve(const config::Config& writer) const;

private:
    /// Base directory for logical Python enricher types; relative paths use the process CWD.
    std::string                                          python_plugin_path_{"enrichers"};
    std::unordered_map<std::string, IPayloadEnricherPtr> enrichers_;
};

} // namespace mldp_pvxs_driver::enricher
