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

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Strongly typed view over a single EPICS reader configuration entry.
 *
 * The reader entry is expected to provide a unique reader name plus a set of PV
 * definitions, each optionally carrying a transport \p option string. The class
 * validates the YAML node upon construction and exposes the parsed data through
 * cheap accessors so that runtime code does not need to reason about YAML
 * specifics.
 */
class EpicsReaderConfig
{
public:
    /**
     * @brief Exception thrown when the reader configuration cannot be parsed.
     *
     * The message is descriptive (missing fields, wrong types, etc.) so callers
     * can surface errors directly to the control plane or logs.
     */
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Definition of a single PV entry as parsed from the YAML config.
     *
     * @note The @ref option field is optional in the YAML; it remains empty when
     *       not provided. It can be a scalar string or an arbitrary subtree;
     *       when a subtree is provided it is preserved in @ref optionConfig.
     */
    struct PVConfig
    {
        /**
         * @brief Options for the special row-timestamped NTTable handling.
         *
         * Activated when the YAML subtree under @ref optionConfig contains:
         *
         * option:
         *   type: nttable-rowts
         *   tsSeconds: secondsPastEpoch   # optional
         *   tsNanos: nanoseconds          # optional
         *
         * Source naming: each NTTable data column becomes a source whose name
         * equals the column field name.
         */
        struct NTTableRowTimestampOptions
        {
            std::string       tsSecondsField = "secondsPastEpoch";
            std::string       tsNanosField = "nanoseconds";
        };

        std::string                               name;         ///< Fully qualified PV name to monitor.
        std::string                               option;       ///< Backend-specific connection option (may be empty).
        std::optional<config::Config>             optionConfig; ///< Optional raw subtree for future extensions.
        std::optional<NTTableRowTimestampOptions> nttableRowTs; ///< Parsed options when `type: nttable-rowts` is selected.
    };

    EpicsReaderConfig();
    /**
     * @brief Build a typed view over the provided YAML node.
     * @throws Error when any of the required fields (name or pvs) are missing
     *         or malformed.
     */
    explicit EpicsReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry);

    /** @return Whether the wrapped YAML node passed validation. */
    bool valid() const;

    /** @return Configured reader name. */
    const std::string& name() const;

    /** @return Ordered list of PV entries as defined in the YAML. */
    const std::vector<PVConfig>& pvs() const;

    /**
     * @brief Convenience accessor returning just the PV names.
     *
     * Useful for subsystems that only care about the identifiers while still
     * letting others use @ref pvs() for access to richer metadata.
     */
    const std::vector<std::string>& pvNames() const;

private:
    /** @brief Populate the typed fields from the raw YAML node. */
    void parse(const ::mldp_pvxs_driver::config::Config& readerEntry);

    bool                     valid_ = false;
    std::string              name_;
    std::vector<PVConfig>    pvs_;
    std::vector<std::string> pvNames_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics
