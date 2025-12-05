#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Strongly typed view over the metrics publication configuration.
 *
 * The configuration node passed to this class should wrap the YAML block
 * describing metrics settings:
 *
 * @code{.yaml}
 * metrics:
 *   endpoint: 0.0.0.0:9464
 * @endcode
 *
 * Callers can construct this class with the `metrics` sub-config and then pass
 * it along to the metrics collector to spin up the exposer.
 */
class MetricsConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    MetricsConfig();
    explicit MetricsConfig(const config::Config& metricsNode);

    bool               valid() const;
    const std::string& endpoint() const;

private:
    void parse(const config::Config& node);

    bool        valid_ = false;
    std::string endpoint_;
};

} // namespace mldp_pvxs_driver::metrics
