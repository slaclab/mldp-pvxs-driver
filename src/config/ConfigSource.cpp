//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/ConfigSource.h>

#include <config/ConfigOverride.h>

#include <filesystem>
#include <stdexcept>

namespace mldp_pvxs_driver::config {

namespace {

namespace fs = std::filesystem;

void replaceNode(ryml::NodeRef dst, ryml::ConstNodeRef src)
{
    if (src.is_map())
    {
        dst.clear_children();
        dst.clear_val();
        dst |= c4::yml::MAP;

        for (const auto srcChild : src.children())
        {
            auto child = dst.append_child();
            if (srcChild.has_key())
            {
                std::string key{srcChild.key().str, srcChild.key().len};
                child.set_key(child.to_arena(key));
            }
            replaceNode(child, srcChild);
        }
        return;
    }

    if (src.is_seq())
    {
        dst.clear_children();
        dst.clear_val();
        dst |= c4::yml::SEQ;

        for (const auto srcChild : src.children())
        {
            auto child = dst.append_child();
            replaceNode(child, srcChild);
        }
        return;
    }

    dst.clear_children();
    if (src.has_val())
    {
        std::string value;
        src >> value;
        dst << value;
    }
    else
    {
        dst.clear_val();
    }
}

void mergeNode(ryml::NodeRef dst, ryml::ConstNodeRef src)
{
    if (src.is_map() && dst.is_map())
    {
        for (const auto srcChild : src.children())
        {
            if (!srcChild.has_key())
            {
                continue;
            }

            std::string key{srcChild.key().str, srcChild.key().len};
            auto        child = dst[dst.to_arena(key)];
            if (child.is_seed())
            {
                child.create();
                child = dst.find_child(c4::to_csubstr(key));
                replaceNode(child, srcChild);
                continue;
            }

            mergeNode(child, srcChild);
        }
        return;
    }

    replaceNode(dst, src);
}

} // namespace

Config loadMergedConfigSources(const std::vector<std::string>& sources)
{
    std::vector<std::string> effectiveSources = sources;
    if (effectiveSources.empty())
    {
        effectiveSources.push_back("config.yaml");
    }

    Config merged = Config::configFromYamlString("{}\n");
    for (const auto& source : effectiveSources)
    {
        std::error_code ec;
        const fs::path  candidate{source};

        if (fs::exists(candidate, ec) && !ec)
        {
            const auto next = Config::configFromFile(source);
            mergeNode(merged.mutableRaw(), next.raw());
            continue;
        }

        try
        {
            applyConfigAssignment(merged, parseConfigOverride(source));
        }
        catch (const ConfigOverrideError&)
        {
            throw std::runtime_error(
                "Configuration source '" + source +
                "' is not a readable file and is not valid dotted path assignment syntax (PATH=VALUE)");
        }
    }

    return merged;
}

} // namespace mldp_pvxs_driver::config
