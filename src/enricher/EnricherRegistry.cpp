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

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::enricher;

namespace {

bool isRegisteredEnricher(const std::string& type)
{
    const auto registered = EnricherFactory::registeredTypes();
    return std::find(registered.begin(), registered.end(), type) != registered.end();
}

config::Config withScriptPath(const config::Config& definition, const std::filesystem::path& script_path, bool require_type)
{
    auto merged = config::Config::configFromYamlString(config::ryml::emitrs_yaml<std::string>(definition.raw()));
    if (!merged.hasChild("type"))
    {
        const auto children = merged.namedSubConfig();
        if (children.size() != 1)
            throw std::runtime_error("could not copy Python enricher configuration");
        merged = children.front().second;
    }
    auto root = merged.mutableRaw();
    if (!root.has_child("script-path"))
        root[root.to_arena("script-path")].create();
    auto child = root[root.to_arena("script-path")];
    child << child.to_arena(script_path.string());
    if (require_type)
    {
        if (!root.has_child("require-enricher-type"))
            root[root.to_arena("require-enricher-type")].create();
        auto required = root[root.to_arena("require-enricher-type")];
        required << true;
    }
    return merged;
}

} // namespace

EnricherRegistry::EnricherRegistry(const config::Config& root)
{
    if (root.hasChild("python-plugins-path"))
    {
        python_plugin_path_ = root.get("python-plugins-path");
        if (python_plugin_path_.empty())
            throw std::runtime_error("'python-plugins-path' must not be empty");
    }

    if (!root.hasChild("enrichers"))
        return;
    const auto entries = root.subConfig("enrichers");
    if (entries.size() != 1 || !entries.front().raw().is_map())
        throw std::runtime_error("enrichers must be a mapping of names to definitions");
    if (entries.front().hasChild("python-plugin-path"))
    {
        python_plugin_path_ = entries.front().get("python-plugin-path");
        if (python_plugin_path_.empty())
            throw std::runtime_error("enrichers 'python-plugin-path' must not be empty");
    }
    for (const auto& [name, definition] : entries.front().namedSubConfig())
    {
        if (name == "python-plugin-path")
            continue;
        if (name.empty())
            throw std::runtime_error("enricher name must not be empty");
        if (!definition.raw().is_map())
            throw std::runtime_error("enricher '" + name + "' must be a mapping");
        const auto type = definition.get("type");
        if (type.empty())
            throw std::runtime_error("enricher '" + name + "' is missing required field 'type'");
        if (isRegisteredEnricher(type))
        {
            enrichers_.emplace(name, EnricherFactory::create(type, definition));
            continue;
        }

#ifdef BUILD_PYTHON_PROCESSOR
        const auto resolved_path = definition.hasChild("script-path")
                                       ? std::filesystem::path{definition.get("script-path")}
                                       : std::filesystem::path{python_plugin_path_} / (type + ".py");
        const auto python_definition = withScriptPath(definition, resolved_path, true);
        auto       enricher = EnricherFactory::create("python-enricher", python_definition);
        if (enricher->enricherType() != type)
            throw std::runtime_error("Python enricher type mismatch: requested '" + type + "', script declares '" + enricher->enricherType() + "'");
        enrichers_.emplace(name, std::move(enricher));
#else
        throw std::runtime_error("Unknown enricher type: " + type);
#endif
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
