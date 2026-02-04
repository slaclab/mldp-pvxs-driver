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
#include <metrics/MetricsConfig.h>
#include <pool/MLDPGrpcPoolConfig.h>

#include <optional>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::controller {

/**
 * @brief Typed view over the controller configuration tree.
 *
 * The controller requires two sets of configuration knobs:
 * - The MLDP pool connection settings (`mldp_pool` block).
 * - The list of reader instances grouped by type (currently only
 *   EPICS readers are supported via the `reader[].epics` entries).
 *
 * This class validates the YAML structure eagerly and exposes
 * convenient accessors so the runtime code can consume strongly
 * typed data instead of reasoning about YAML nodes directly.
 */
class MLDPPVXSControllerConfig
{
public:
    /**
     * @brief Exception thrown when the controller configuration is invalid.
     */
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    MLDPPVXSControllerConfig();
    explicit MLDPPVXSControllerConfig(const ::mldp_pvxs_driver::config::Config& root);

    /** @return Whether the configuration passed validation. */
    bool valid() const;

    /** @return Pool configuration managed by the controller. */
    const util::pool::MLDPGrpcPoolConfig& pool() const;

    /** @return Logical provider name to register with MLDP. */
    const std::string& providerName() const;

    /** @return Number of controller threads to spin up. */
    int controllerThreadPoolSize() const;

    /**
     * @return Max protobuf payload bytes per stream before flushing.
     *
     * When a single writer thread accumulates more than this many payload bytes
     * across queued requests, it closes the current gRPC stream and opens a new
     * one to continue sending.
     */
    std::size_t controllerStreamMaxBytes() const;

    /**
     * @return Max stream age in milliseconds before flushing.
     *
     * Writer threads close and reopen a stream when it has been open longer
     * than this duration, even if the byte threshold has not yet been reached.
     */
    std::chrono::milliseconds controllerStreamMaxAge() const;

    /**
     * @return Raw configuration blocks for registered reader instances.
     *
     * Each entry is a parsed Config node anchored to the root YAML tree. The
     * controller layer stays unaware of reader specifics, delegating parsing to
     * the reader implementations.
     */
    const std::vector<config::Config>& readerConfigs() const;

    /**
     * @return Reader entries as (type, sub-config) pairs.
     *
     * Each element represents a reader entry where the first string is the
     * reader type (for example "epics-pvxs" or "epics-base") and the second is the Config that
     * points to the reader-specific configuration block. This API is useful
     * for callers that need to dispatch based on the reader type while still
     * passing the specific sub-configuration to the reader factory.
     */
    const std::vector<std::pair<std::string, config::Config>>& readerEntries() const;

    /** @return Optional metrics configuration when the YAML provides it. */
    const std::optional<metrics::MetricsConfig>& metricsConfig() const;

private:
    void parse(const ::mldp_pvxs_driver::config::Config& root);
    void parseThreadPool(const ::mldp_pvxs_driver::config::Config& root);
    void parsePool(const ::mldp_pvxs_driver::config::Config& root);
    void parseReaders(const ::mldp_pvxs_driver::config::Config& root);
    void parseMetrics(const ::mldp_pvxs_driver::config::Config& root);
    void parseStreamLimits(const ::mldp_pvxs_driver::config::Config& root);

    bool                                valid_ = false;
    util::pool::MLDPGrpcPoolConfig      pool_;
    int                                 controllerThreadPoolSize_ = 0;
    std::size_t                         controllerStreamMaxBytes_ = 2 * 1024 * 1024;
    std::chrono::milliseconds           controllerStreamMaxAge_{200};
    std::vector<config::Config>         readerConfigs_;
    std::vector<std::pair<std::string, config::Config>> readerEntries_;
    std::optional<metrics::MetricsConfig> metricsConfig_;
};

} // namespace mldp_pvxs_driver::controller
