#include <metrics/MetricsConfig.h>

#include <sstream>

namespace {

std::string missingField(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in metrics configuration";
    return oss.str();
}

} // namespace

namespace mldp_pvxs_driver::metrics {

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
    if (!node.valid())
    {
        throw Error("Metrics configuration node is invalid");
    }

    if (!node.raw().is_map())
    {
        throw Error("metrics block must be a map");
    }

    if (!node.hasChild("endpoint"))
    {
        throw Error(missingField("endpoint"));
    }

    endpoint_ = node.get("endpoint");
    if (endpoint_.empty())
    {
        throw Error("metrics.endpoint must not be empty");
    }

    valid_ = true;
}

} // namespace mldp_pvxs_driver::metrics
