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
#include <controller/RouteTable.h>
#include <metrics/MetricsConfig.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::controller {

inline constexpr char NameKey[] = "name";
inline constexpr char ReaderKey[] = "reader";
inline constexpr char MetricsKey[] = "metrics";
inline constexpr char RoutingKey[] = "routing";

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

    /** @return Unique controller instance name (used as Prometheus label). */
    const std::string& name() const;

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

    /**
     * @return Writer entries as (type, config-node) pairs.
     *
     * Each element is the writer type identifier (e.g. "mldp", "hdf5") paired
     * with the YAML node that the @ref WriterFactory should pass to the
     * writer's constructor.
     */
    const std::vector<std::pair<std::string, config::Config>>& writerEntries() const;

    /**
     * @return Processor entries as (type, config-node) pairs.
     */
    const std::vector<std::pair<std::string, config::Config>>& processorEntries() const;

    /**
     * @brief Parsed queryable entries from the @c queryable: YAML sequence.
     */
    struct QueryableEntry
    {
        std::string    type;
        config::Config cfg;
    };

    /**
     * @return Queryable entries as (type, config-node) pairs.
     *
     * Each element is a queryable type identifier (e.g. "mldp") paired with
     * the YAML node that @ref QueryableFactory::prepare should receive.
     */
    const std::vector<QueryableEntry>& queryableEntries() const
    {
        return queryable_entries_;
    }

    /** @return Optional metrics configuration when the YAML provides it. */
    const std::optional<metrics::MetricsConfig>& metricsConfig() const;

    /** @return Route entries mapping writer names to their source reader lists. */
    const std::vector<RouteFilterEntry>& routeEntries() const;

    /** @return Maximum number of batches the controller queue can hold before blocking. */
    std::size_t queueCapacity() const { return queue_capacity_; }

private:
    void parse(const ::mldp_pvxs_driver::config::Config& root);
    void parseWriter(const ::mldp_pvxs_driver::config::Config& root);
    void parseReaders(const ::mldp_pvxs_driver::config::Config& root);
    void parseProcessors(const ::mldp_pvxs_driver::config::Config& root);
    void parseMetrics(const ::mldp_pvxs_driver::config::Config& root);
    void parseRouting(const ::mldp_pvxs_driver::config::Config& root);
    void parseQueryables(const ::mldp_pvxs_driver::config::Config& root);

    bool                                                valid_ = false;
    std::string                                         name_{"default"};
    std::vector<config::Config>                         readerConfigs_;
    std::vector<std::pair<std::string, config::Config>> readerEntries_;
    std::vector<std::pair<std::string, config::Config>> writerEntries_;
    std::vector<std::pair<std::string, config::Config>> processorEntries_;
    std::optional<metrics::MetricsConfig>               metricsConfig_;
    std::vector<RouteFilterEntry>                       routeEntries_;
    std::vector<QueryableEntry>                         queryable_entries_;
    std::size_t                                         queue_capacity_{4096};
};

} // namespace mldp_pvxs_driver::controller
