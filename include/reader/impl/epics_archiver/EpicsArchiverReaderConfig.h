//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file EpicsArchiverReaderConfig.h
 * @brief Configuration parser for EPICS Archiver Appliance reader instances.
 *
 * This header provides the EpicsArchiverReaderConfig class for parsing and
 * validating Archiver Appliance reader configuration from YAML. It handles
 * reader-level settings (name, archiver hostname/URL) and the list of PVs
 * to retrieve from the archiver.
 */

#pragma once

#include <config/Config.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief Configuration parser for EPICS Archiver Appliance reader.
 *
 * Parses and validates YAML configuration for the archiver reader, extracting
 * the archiver service hostname/URL and the list of PV names to retrieve.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-archiver
 *     name: my-archiver-reader
 *     hostname: "archiver.slac.stanford.edu:11200"
 *     start_date: "2026-01-01T00:00:00Z"
 *     end_date: "2026-01-02T00:00:00Z" # optional
 *     pvs:
 *       - name: "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV"
 *       - name: "FACET:DL1:SBEN:1:BDES"
 * @endcode
 */
class EpicsArchiverReaderConfig
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
     * @brief Definition of a single PV entry for archiver retrieval.
     */
    struct PVConfig
    {
        std::string name; ///< PV name to retrieve from archiver.
    };

    /**
     * @brief Default constructor for an empty config.
     */
    EpicsArchiverReaderConfig();

    /**
     * @brief Build a typed view over the provided YAML node.
     *
     * @param readerEntry YAML configuration node for this reader.
     * @throws Error when any of the required fields (name, hostname,
     *         start_date/startDate, or pvs) are missing or malformed.
     */
    explicit EpicsArchiverReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry);

    /**
     * @brief Check whether the wrapped YAML node passed validation.
     *
     * @return true if configuration is valid; false otherwise.
     */
    bool valid() const;

    /**
     * @brief Get the configured reader name.
     *
     * @return Reader name as specified in YAML config.
     */
    const std::string& name() const;

    /**
     * @brief Get the Archiver Appliance service hostname/URL.
     *
     * @return Hostname or URL where the archiver service is accessible
     *         (e.g., "archiver.slac.stanford.edu:11200").
     */
    const std::string& hostname() const;

    /**
     * @brief Get the start of the requested archiver time window.
     *
     * @return Start date/time string as configured in YAML.
     */
    const std::string& startDate() const;

    /**
     * @brief Get the optional end of the requested archiver time window.
     *
     * @return End date/time string when configured; std::nullopt otherwise.
     */
    const std::optional<std::string>& endDate() const;

    /**
     * @brief Get the ordered list of PV entries to retrieve.
     *
     * @return Vector of PVConfig objects, each containing a PV name.
     */
    const std::vector<PVConfig>& pvs() const;

    /**
     * @brief Get just the PV names for convenience.
     *
     * @return Vector of PV name strings in the same order as pvs().
     */
    const std::vector<std::string>& pvNames() const;

private:
    /**
     * @brief Populate the typed fields from the raw YAML node.
     *
     * @param readerEntry YAML configuration node to parse.
     * @throws Error if parsing fails.
     */
    void parse(const ::mldp_pvxs_driver::config::Config& readerEntry);

    bool                     valid_ = false;
    std::string              name_;
    std::string              hostname_;
    std::string              start_date_;
    std::optional<std::string> end_date_;
    std::vector<PVConfig>    pvs_;
    std::vector<std::string> pvNames_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
