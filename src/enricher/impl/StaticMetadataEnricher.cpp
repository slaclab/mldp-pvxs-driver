//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/impl/StaticMetadataEnricher.h>

#include <stdexcept>
#include <utility>

namespace mldp_pvxs_driver::enricher {
namespace {

    std::unordered_map<std::string, std::string> requireMap(const config::Config& config, const std::string& key)
    {
        if (!config.hasChild(key))
            throw std::runtime_error("enricher requires '" + key + "'");

        const auto nodes = config.subConfig(key);
        if (nodes.size() != 1 || !nodes.front().raw().is_map())
            throw std::runtime_error("enricher '" + key + "' must be a mapping");

        std::unordered_map<std::string, std::string> result;
        for (const auto& [name, value] : nodes.front().namedSubConfig())
        {
            std::string text;
            value >> text;
            result.emplace(name, std::move(text));
        }
        return result;
    }

} // namespace

StaticMetadataEnricher::StaticMetadataEnricher(const config::Config& config)
{
    configure(config);
}

void StaticMetadataEnricher::configure(const config::Config& config)
{
    metadata_ = requireMap(config, "metadata");
}

bool StaticMetadataEnricher::enrich(util::bus::IDataBus::EventBatch& batch) noexcept
{
    for (const auto& [key, value] : metadata_)
        batch.metadata[key] = value;
    return true;
}

} // namespace mldp_pvxs_driver::enricher
