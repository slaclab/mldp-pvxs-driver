//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::metrics {

inline constexpr char EndpointKey[] = "endpoint";
inline constexpr char ScanIntervalSecondsKey[] = "scan-interval-seconds";

/**
 * @brief Strongly typed view over the metrics publication configuration.
 *
 * The driver configuration can optionally expose a `metrics` block describing
 * the Prometheus HTTP endpoint that should publish internal statistics. This
 * helper parses that block once, validates the required keys, and exposes the
 * typed data so runtime components can spin up the Prometheus exposer without
 * needing to reason about YAML trees.
 *
 * Typical YAML snippet:
 * @code{.yaml}
 * metrics:
 *   endpoint: 0.0.0.0:9464
 *   scan_interval_seconds: 1
 * @endcode
 */
class MetricsConfig
{
public:
    /**
     * @brief Exception thrown when the metrics configuration is malformed.
     *
     * All validation errors surface through this exception to keep the call
     * sites concise (e.g., missing endpoint, wrong YAML type, empty strings).
     */
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Construct an empty (invalid) metrics config.
     *
     * Useful when consumers want to defer parsing until they detect that the
     * YAML contains a metrics block.
     */
    MetricsConfig();

    /**
     * @brief Parse the `metrics` YAML node into a typed structure.
     *
     * @param metricsNode Sub-config pointing to the metrics block. The node is
     *        expected to be a map that contains at least the `endpoint` key.
     * @throws Error when the node is invalid or mandatory fields are missing.
     */
    explicit MetricsConfig(const config::Config& metricsNode);

    /**
     * @brief Whether the configuration passed validation.
     *
     * @return true when the object was constructed from a valid YAML node, or
     *         false when default-constructed.
     */
    bool valid() const;

    /**
     * @brief Host:port pair where the Prometheus exposer should bind.
     *
     * Only populated when @ref valid() is true.
     */
    const std::string& endpoint() const;

    /**
     * @brief Interval in seconds between system metrics collection scans.
     *
     * Defaults to 1 second if not specified in configuration.
     * Only meaningful when @ref valid() is true.
     */
    uint32_t scanIntervalSeconds() const;

private:
    void parse(const config::Config& node);

    bool        valid_ = false;             ///< Tracks whether parsing succeeded.
    std::string endpoint_;                  ///< Cached endpoint string (host:port).
    uint32_t    scan_interval_seconds_ = 1; ///< System metrics scan interval in seconds.
};

} // namespace mldp_pvxs_driver::metrics
