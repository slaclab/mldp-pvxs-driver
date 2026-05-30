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
 * @file EpicsDSMetadataReaderConfig.h
 * @brief Configuration parser for the EPICS Directory Service metadata reader.
 *
 * Parses and validates YAML configuration for the epics-ds-metadata reader,
 * which fetches PV metadata via an RPC call to an EPICS Directory Service PVA
 * endpoint and publishes the results as SourceMetadataPayload onto the bus.
 */

#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics_ds {

/**
 * @brief Configuration for the EPICS Directory Service metadata reader.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-ds-metadata
 *     name: my-ds-reader
 *     service: ds
 *     query: "%"
 *     timeout-sec: 5.0
 *     source-name-column: channelName
 *     tags-column: tags
 *     rescan-interval-sec: 300.0
 * @endcode
 */
class EpicsDSMetadataReaderConfig
{
public:
    /**
     * @brief Exception thrown when the reader configuration cannot be parsed.
     */
    struct Error : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Build a typed view over the provided YAML node.
     *
     * @param cfg YAML configuration node for this reader.
     * @throws Error when required fields are missing or values are invalid.
     */
    explicit EpicsDSMetadataReaderConfig(const config::Config& cfg);

    /**
     * @brief Whether the configuration parsed successfully.
     */
    bool valid() const noexcept { return valid_; }

    /**
     * @brief Reader instance name (required, non-empty).
     */
    std::string name() const noexcept { return name_; }

    /**
     * @brief PVA service name to call via RPC (default: "ds").
     */
    std::string service() const noexcept { return service_; }

    /**
     * @brief Query pattern sent in the NTURI query.name field (default: "%").
     */
    std::string query() const noexcept { return query_; }

    /**
     * @brief RPC call timeout in seconds (default: 5.0, must be positive).
     */
    double timeoutSec() const noexcept { return timeout_sec_; }

    /**
     * @brief NTTable column name that holds the PV/source name (default: "channelName").
     */
    std::string sourceNameColumn() const noexcept { return source_name_column_; }

    /**
     * @brief NTTable column name that holds comma-separated tags (default: "", disabled).
     */
    std::string tagsColumn() const noexcept { return tags_column_; }

    /**
     * @brief Comma-separated list of columns to request via the `show` query parameter.
     *        Empty string means no `show` parameter is sent (server returns all columns).
     *        Example: "name,elementname,hostName"
     */
    std::string showColumns() const noexcept { return show_columns_; }

    /**
     * @brief Interval between periodic re-fetches in seconds (default: 0.0 = run once).
     */
    double rescanIntervalSec() const noexcept { return rescan_interval_sec_; }

private:
    void parse(const config::Config& cfg);

    bool        valid_{false};
    std::string name_;
    std::string service_{"ds"};
    std::string query_{"%"};
    double      timeout_sec_{5.0};
    std::string source_name_column_{"channelName"};
    std::string tags_column_;
    std::string show_columns_;
    double      rescan_interval_sec_{0.0};
};

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
