//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/EnricherFactory.h>
#include <enricher/EnricherRegistry.h>

#include <stdexcept>
#include <unordered_set>

using namespace mldp_pvxs_driver::enricher;

EnricherRegistry::EnricherRegistry(const config::Config& root)
{
    if (!root.hasChild("enrichers"))
        return;
    const auto entries = root.subConfig("enrichers");
    if (entries.size() != 1 || !entries.front().raw().is_map())
        throw std::runtime_error("enrichers must be a mapping of names to definitions");
    for (const auto& [name, definition] : entries.front().namedSubConfig())
    {
        if (name.empty())
            throw std::runtime_error("enricher name must not be empty");
        if (!definition.raw().is_map())
            throw std::runtime_error("enricher '" + name + "' must be a mapping");
        const auto type = definition.get("type");
        if (type.empty())
            throw std::runtime_error("enricher '" + name + "' is missing required field 'type'");
        auto enricher = EnricherFactory::create(type, definition);
        if (!enricher)
            throw std::runtime_error("could not create enricher '" + name + "'");
        enrichers_.emplace(name, std::move(enricher));
    }
}

std::vector<IPayloadEnricherPtr> EnricherRegistry::resolve(const config::Config& writer) const
{
    std::vector<IPayloadEnricherPtr> result;
    if (!writer.valid() || !writer.raw().is_map())
        throw std::runtime_error("writer enricher configuration must be a mapping");
    if (!writer.hasChild("enrichers"))
        return result;
    if (!writer.isSequence("enrichers"))
        throw std::runtime_error("writer 'enrichers' must be a sequence of names");
    std::unordered_set<std::string> seen;
    for (const auto& item : writer.subConfig("enrichers"))
    {
        std::string name;
        item >> name;
        if (name.empty())
            throw std::runtime_error("writer 'enrichers' cannot contain an empty name");
        if (!seen.insert(name).second)
            throw std::runtime_error("writer 'enrichers' contains duplicate name '" + name + "'");
        const auto found = enrichers_.find(name);
        if (found == enrichers_.end())
            throw std::runtime_error("writer references unknown enricher '" + name + "'");
        result.push_back(found->second);
    }
    return result;
}
