#pragma once

#include <config/Config.h>
#include <reader/impl/epics/EpicsReaderConfig.h>

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

    /**
     * @brief Parsed MLDP pool description.
     */
    struct PoolConfig
    {
        std::string url;          ///< Fully qualified MLDP service endpoint.
        int         max_conn = 0; ///< Maximum number of connections to keep open.
    };

    MLDPPVXSControllerConfig();
    explicit MLDPPVXSControllerConfig(const ::mldp_pvxs_driver::config::Config& root);

    /** @return Whether the configuration passed validation. */
    bool valid() const;

    /** @return Pool configuration managed by the controller. */
    const PoolConfig& pool() const;

    /**
     * @return List of configured EPICS readers.
     *
     * Reader entries are parsed using @ref reader::impl::epics::EpicsReaderConfig
     * so callers can reuse the existing reader lifecycle code.
     */
    const std::vector<reader::impl::epics::EpicsReaderConfig>& epicsReaders() const;

private:
    void parse(const ::mldp_pvxs_driver::config::Config& root);
    void parsePool(const ::mldp_pvxs_driver::config::Config& root);
    void parseReaders(const ::mldp_pvxs_driver::config::Config& root);

    bool                                                valid_ = false;
    PoolConfig                                          pool_;
    std::vector<reader::impl::epics::EpicsReaderConfig> epicsReaders_;
};

} // namespace mldp_pvxs_driver::controller
