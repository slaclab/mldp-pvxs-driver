#include <metrics/MetricsConfig.h>

using namespace mldp_pvxs_driver::config;

using namespace mldp_pvxs_driver::metrics;

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

    valid_ = true;
}
