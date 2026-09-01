//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/ConfigOverride.h>

#include <cctype>
#include <optional>
#include <sstream>
#include <utility>

namespace mldp_pvxs_driver::config {

namespace {

struct PathSegment
{
    std::string           key;
    std::optional<size_t> index;
};

[[nodiscard]] std::string describePathPrefix(const std::vector<PathSegment>& segments, size_t endExclusive);

[[nodiscard]] std::vector<PathSegment> parsePath(std::string_view path)
{
    if (path.empty())
    {
        throw ConfigOverrideError("config override path must not be empty");
    }

    std::vector<PathSegment> segments;
    size_t                   pos = 0;

    while (pos < path.size())
    {
        const size_t keyStart = pos;
        while (pos < path.size() && path[pos] != '.' && path[pos] != '[')
        {
            ++pos;
        }

        if (keyStart == pos)
        {
            throw ConfigOverrideError("config override path contains an empty segment: '" + std::string(path) + "'");
        }

        PathSegment segment;
        segment.key = std::string(path.substr(keyStart, pos - keyStart));

        if (pos < path.size() && path[pos] == '[')
        {
            ++pos;
            const size_t indexStart = pos;
            while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos])))
            {
                ++pos;
            }

            if (indexStart == pos || pos >= path.size() || path[pos] != ']')
            {
                throw ConfigOverrideError("config override path has an invalid sequence index: '" + std::string(path) + "'");
            }

            segment.index = std::stoull(std::string(path.substr(indexStart, pos - indexStart)));
            ++pos;
        }

        segments.push_back(std::move(segment));

        if (pos == path.size())
        {
            break;
        }

        if (path[pos] != '.')
        {
            throw ConfigOverrideError("config override path has invalid syntax: '" + std::string(path) + "'");
        }

        ++pos;
        if (pos == path.size())
        {
            throw ConfigOverrideError("config override path must not end with '.': '" + std::string(path) + "'");
        }
    }

    return segments;
}

[[nodiscard]] ryml::NodeRef findOrSeedChild(ryml::NodeRef parent, const std::string& key)
{
    const auto keyView = c4::to_csubstr(key);
    if (parent.has_child(keyView))
    {
        return parent.find_child(keyView);
    }

    return parent[parent.to_arena(key)];
}

[[nodiscard]] ryml::NodeRef ensureMapNode(ryml::NodeRef node)
{
    if (node.is_seed())
    {
        node |= c4::yml::MAP;
        return node.tree()->ref(node.id());
    }

    if (!node.is_map())
    {
        throw ConfigOverrideError("cannot convert an existing non-map node into a map while applying config override");
    }

    return node;
}

[[nodiscard]] ryml::NodeRef descendThroughSequence(ryml::NodeRef              sequenceNode,
                                                   const std::vector<PathSegment>& segments,
                                                   size_t                     segmentIndex,
                                                   const std::string&         fullPath)
{
    if (!sequenceNode.is_seq())
    {
        throw ConfigOverrideError(
            "config override path '" + fullPath +
            "' does not match the current configuration at '" + describePathPrefix(segments, segmentIndex + 1) + "'");
    }

    const auto childCount = static_cast<size_t>(sequenceNode.num_children());
    if (childCount == 0)
    {
        auto child = sequenceNode.append_child();
        child |= c4::yml::MAP;
        return child;
    }

    if (childCount > 1)
    {
        throw ConfigOverrideError(
            "config override path '" + fullPath +
            "' is ambiguous at '" + describePathPrefix(segments, segmentIndex + 1) +
            "' because the sequence has multiple entries; use an explicit [index]");
    }

    return sequenceNode.child(0);
}

[[nodiscard]] ryml::NodeRef ensureSequenceEntry(ryml::NodeRef                    sequenceNode,
                                                size_t                           index,
                                                bool                             isLast,
                                                bool                             allowSequenceExpansion,
                                                const std::vector<PathSegment>&  segments,
                                                size_t                           segmentIndex,
                                                const std::string&               fullPath)
{
    if (sequenceNode.is_seed())
    {
        if (!allowSequenceExpansion)
        {
            throw ConfigOverrideError(
                "config override path '" + fullPath +
                "' does not match the current configuration at '" + describePathPrefix(segments, segmentIndex + 1) + "'");
        }
        sequenceNode |= c4::yml::SEQ;
        sequenceNode = sequenceNode.tree()->ref(sequenceNode.id());
    }

    if (!sequenceNode.is_seq())
    {
        throw ConfigOverrideError(
            "config override path '" + fullPath +
            "' does not match the current configuration at '" + describePathPrefix(segments, segmentIndex + 1) + "'");
    }

    if (!allowSequenceExpansion && index >= static_cast<size_t>(sequenceNode.num_children()))
    {
        throw ConfigOverrideError(
            "config override path '" + fullPath +
            "' uses out-of-range index " + std::to_string(index));
    }

    while (static_cast<size_t>(sequenceNode.num_children()) <= index)
    {
        auto child = sequenceNode.append_child();
        if (!isLast)
        {
            child |= c4::yml::MAP;
        }
    }

    return sequenceNode.child(static_cast<ryml::id_type>(index));
}

[[nodiscard]] std::string describePathPrefix(const std::vector<PathSegment>& segments, size_t endExclusive)
{
    std::ostringstream oss;
    for (size_t i = 0; i < endExclusive; ++i)
    {
        if (i > 0)
        {
            oss << '.';
        }
        oss << segments[i].key;
        if (segments[i].index.has_value())
        {
            oss << '[' << *segments[i].index << ']';
        }
    }
    return oss.str();
}

} // namespace

ConfigOverride parseConfigOverride(std::string_view arg)
{
    const size_t eqPos = arg.find('=');
    if (eqPos == std::string_view::npos)
    {
        throw ConfigOverrideError("config override must use PATH=VALUE syntax: '" + std::string(arg) + "'");
    }

    ConfigOverride result;
    result.path = std::string(arg.substr(0, eqPos));
    result.value = std::string(arg.substr(eqPos + 1));

    if (result.path.empty())
    {
        throw ConfigOverrideError("config override path must not be empty");
    }

    return result;
}

void applyConfigOverrideImpl(Config& cfg,
                             const ConfigOverride& overrideSpec,
                             bool                  allowSequenceExpansion)
{
    auto current = cfg.mutableRaw();
    if (current.invalid())
    {
        throw ConfigOverrideError("cannot apply config override to an invalid configuration");
    }

    const auto segments = parsePath(overrideSpec.path);

    for (size_t i = 0; i < segments.size(); ++i)
    {
        const auto& segment = segments[i];
        const bool  isLast = (i + 1 == segments.size());

        if (!current.is_map())
        {
            throw ConfigOverrideError(
                "config override path '" + overrideSpec.path +
                "' does not match the current configuration at '" + describePathPrefix(segments, i) + "'");
        }

        auto child = findOrSeedChild(current, segment.key);

        if (segment.index.has_value())
        {
            child = ensureSequenceEntry(
                child, *segment.index, isLast, allowSequenceExpansion, segments, i, overrideSpec.path);
        }

        if (isLast)
        {
            if (child.readable() && (child.is_map() || child.is_seq()))
            {
                throw ConfigOverrideError(
                    "config override path '" + overrideSpec.path +
                    "' points to a non-scalar node and cannot be replaced with a scalar value");
            }

            child << overrideSpec.value;
            return;
        }

        if (child.is_seed())
        {
            child = ensureMapNode(child);
        }

        if (child.is_seq())
        {
            current = descendThroughSequence(child, segments, i, overrideSpec.path);
            continue;
        }

        if (!child.is_map())
        {
            throw ConfigOverrideError(
                "config override path '" + overrideSpec.path +
                "' does not match the current configuration at '" + describePathPrefix(segments, i + 1) + "'");
        }

        current = child;
    }
}

void applyConfigOverride(Config& cfg, const ConfigOverride& overrideSpec)
{
    applyConfigOverrideImpl(cfg, overrideSpec, false);
}

void applyConfigOverrides(Config& cfg, const std::vector<std::string>& rawOverrides)
{
    for (const auto& rawOverride : rawOverrides)
    {
        applyConfigOverride(cfg, parseConfigOverride(rawOverride));
    }
}

void applyConfigAssignment(Config& cfg, const ConfigOverride& overrideSpec)
{
    applyConfigOverrideImpl(cfg, overrideSpec, true);
}

} // namespace mldp_pvxs_driver::config
