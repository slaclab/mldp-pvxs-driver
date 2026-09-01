//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/MLDPChannelProcessorConfig.h>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::processor;

namespace {

constexpr auto kNameKey = "name";
constexpr auto kSourcesKey = "sources";
constexpr auto kAlignmentKey = "alignment";
constexpr auto kTriggerKey = "trigger";
constexpr auto kTriggerIntervalSecKey = "trigger-interval-sec";
constexpr auto kMaxBufferDepthKey = "max-buffer-depth";

AlignmentPolicy parseAlignment(const config::Config& cfg)
{
    const auto value = cfg.get(kAlignmentKey, "latest-value");
    if (value == "latest-value")
    {
        return AlignmentPolicy::LatestValue;
    }
    if (value == "all-updated")
    {
        return AlignmentPolicy::AllUpdated;
    }
    if (value == "interpolate")
    {
        return AlignmentPolicy::Interpolate;
    }
    throw MLDPChannelProcessorConfig::Error("processor: unknown 'alignment' value '" + value + "'");
}

TriggerPolicy parseTrigger(const config::Config& cfg)
{
    const auto value = cfg.get(kTriggerKey, "any-update");
    if (value == "any-update")
    {
        return TriggerPolicy::AnyUpdate;
    }
    if (value == "all-updated")
    {
        return TriggerPolicy::AllUpdated;
    }
    if (value == "interval")
    {
        return TriggerPolicy::Interval;
    }
    throw MLDPChannelProcessorConfig::Error("processor: unknown 'trigger' value '" + value + "'");
}

std::vector<std::string> parseSources(const config::Config& cfg)
{
    if (!cfg.hasChild(kSourcesKey))
    {
        return {};
    }

    const auto source_nodes = cfg.subConfig(kSourcesKey);
    if (source_nodes.empty())
    {
        return {};
    }

    std::vector<std::string> sources;
    sources.reserve(source_nodes.size());
    for (const auto& source_node : source_nodes)
    {
        std::string source;
        source_node >> source;
        if (!source.empty())
        {
            sources.push_back(source);
        }
    }

    return sources;
}

} // namespace

MLDPChannelProcessorConfig::MLDPChannelProcessorConfig(const config::Config& cfg)
{
    if (cfg.hasChild(kNameKey))
    {
        name_ = cfg.get(kNameKey);
    }

    sources_ = parseSources(cfg);
    alignment_ = parseAlignment(cfg);
    trigger_ = parseTrigger(cfg);

    const int max_buffer_depth = cfg.getInt(kMaxBufferDepthKey, 0);
    if (max_buffer_depth < 0)
    {
        throw Error("processor: 'max-buffer-depth' must be >= 0");
    }
    max_buffer_depth_ = static_cast<std::size_t>(max_buffer_depth);

    if (trigger_ == TriggerPolicy::Interval)
    {
        if (!cfg.hasChild(kTriggerIntervalSecKey))
        {
            throw Error("processor: 'trigger-interval-sec' is required when 'trigger' is 'interval'");
        }
        trigger_interval_sec_ = cfg.getDouble(kTriggerIntervalSecKey, 0.0);
        if (trigger_interval_sec_ <= 0.0)
        {
            throw Error("processor: 'trigger-interval-sec' must be > 0 when 'trigger' is 'interval'");
        }
    }
}

const std::string& MLDPChannelProcessorConfig::name() const noexcept
{
    return name_;
}

const std::vector<std::string>& MLDPChannelProcessorConfig::sources() const noexcept
{
    return sources_;
}

AlignmentPolicy MLDPChannelProcessorConfig::alignment() const noexcept
{
    return alignment_;
}

TriggerPolicy MLDPChannelProcessorConfig::trigger() const noexcept
{
    return trigger_;
}

double MLDPChannelProcessorConfig::triggerIntervalSec() const noexcept
{
    return trigger_interval_sec_;
}

std::size_t MLDPChannelProcessorConfig::maxBufferDepth() const noexcept
{
    return max_buffer_depth_;
}

void MLDPChannelProcessorConfig::setName(std::string name)
{
    name_ = std::move(name);
}

void MLDPChannelProcessorConfig::setSources(std::vector<std::string> sources)
{
    sources_ = std::move(sources);
}

bool MLDPChannelProcessorConfig::hasExplicitSources() const noexcept
{
    return !sources_.empty();
}
