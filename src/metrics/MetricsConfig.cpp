//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <metrics/MetricsConfig.h>

using namespace mldp_pvxs_driver::config;

using namespace mldp_pvxs_driver::metrics;

namespace {
std::string pickKey(const Config& cfg, const std::string& dashKey, const std::string& underscoreKey)
{
    if (cfg.hasChild(dashKey))
    {
        return dashKey;
    }
    return underscoreKey;
}
} // namespace

MetricsConfig::MetricsConfig() = default;

MetricsConfig::MetricsConfig(const config::Config& metricsNode)
{
    parse(metricsNode);
}

bool MetricsConfig::valid() const
{
    return valid_;
}

const std::string& MetricsConfig::endpoint() const
{
    return endpoint_;
}

uint32_t MetricsConfig::scanIntervalSeconds() const
{
    return scan_interval_seconds_;
}

void MetricsConfig::parse(const config::Config& node)
{
    // If the node is not valid it means there was no `metrics` block in
    // the top-level configuration. Treat this as "metrics not configured"
    // and leave this object in its default (invalid) state instead of
    // throwing an exception.
    if (!node.valid())
    {
        valid_ = false;
        return;
    }

    if (!node.raw().is_map())
    {
        throw Error("metrics block must be a map");
    }

    if (!node.hasChild("endpoint"))
    {
        throw Error(makeMissingFieldMessage("endpoint"));
    }

    endpoint_ = node.get("endpoint");
    if (endpoint_.empty())
    {
        throw Error("metrics.endpoint must not be empty");
    }

    // Parse optional scan-interval-seconds with default of 1.
    const auto scanIntervalKey = pickKey(node, "scan-interval-seconds", "scan_interval_seconds");
    if (node.hasChild(scanIntervalKey))
    {
        try
        {
            scan_interval_seconds_ = std::stoul(node.get(scanIntervalKey));
        }
        catch (const std::exception&)
        {
            throw Error("metrics." + scanIntervalKey + " must be a positive integer");
        }

        if (scan_interval_seconds_ < 1)
        {
            throw Error("metrics." + scanIntervalKey + " must be at least 1");
        }
    }

    valid_ = true;
}
