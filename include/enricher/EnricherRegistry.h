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
#include <metrics/Metrics.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::enricher {

class EnricherRegistry
{
public:
    /**
     * @param root     Root configuration holding the `enrichers` mapping.
     * @param metrics  Optional metrics collector; when set every enricher is
     *                 wrapped to export runs/dropped/latency metrics labelled
     *                 with `enricher` (instance name) and `type`.
     */
    explicit EnricherRegistry(const config::Config& root, std::shared_ptr<metrics::Metrics> metrics = nullptr);
    std::vector<IPayloadEnricherPtr> resolve(const config::Config& writer) const;

private:
    /// Base directory for logical Python enricher types; relative paths use the process CWD.
    std::string                                          python_plugin_path_{"python-plugins"};
    std::unordered_map<std::string, IPayloadEnricherPtr> enrichers_;
};

} // namespace mldp_pvxs_driver::enricher
