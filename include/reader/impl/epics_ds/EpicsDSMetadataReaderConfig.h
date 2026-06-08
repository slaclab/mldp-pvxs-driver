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
 * @file   EpicsDSMetadataReaderConfig.h
 * @brief  Configuration parser for the EPICS Directory Service metadata reader.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 *
 * Parses and validates YAML configuration for the epics-ds-metadata reader,
 * which fetches PV metadata via an RPC call to an EPICS Directory Service PVA
 * endpoint and publishes the results as SourceMetadataPayload onto the bus.
 */

#pragma once

#include <config/Config.h>

#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics_ds {

/**
 * @class  EpicsDSMetadataReaderConfig
 * @brief  Configuration for the EPICS Directory Service metadata reader.
 * @details
 *   Parses and validates all keys from the YAML node passed to the constructor.
 *   Throws @c Error on missing required fields or out-of-range values.
 *
 *   Supported keys:
 *   | Key                   | Type   | Default        | Description |
 *   |-----------------------|--------|----------------|-------------|
 *   | name                  | string | (required)     | Reader instance name |
 *   | service               | string | `ds`           | PVA channel for RPC |
 *   | query                 | string | `%`            | NTURI query.name wildcard |
 *   | timeout-sec           | double | `5.0`          | RPC call timeout (> 0) |
 *   | source-name-column    | string | `channelName`  | NTTable column holding PV name |
 *   | tags-column           | string | `""`           | NTTable column holding tags (disabled when empty) |
 *   | show-columns          | string | `""`           | Comma-separated columns for wildcard `show=` param |
 *   | rescan-interval-sec   | double | `0.0`          | Re-fetch period; 0 = run once |
 *   | worker-thread-count   | int    | `1`            | 1 = inline; N>1 = 1 producer + (N-1) consumers |
 *   | max-queue-depth       | int    | `16`           | Bounded queue depth (producer/consumer mode only) |
 *   | pvs                   | list   | (required)     | Per-PV enrichment entries |
 *   | pvs[].name            | string | (required)     | Exact PV name |
 *   | pvs[].metadata        | map    | `{}`           | Static key/value attributes merged into the entry |
 *   | pv-show-columns       | string | see default    | DS `show=` columns for PV-list mode (default: `dname,ename,etype,lname,ioc,scheme,z`) |
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
 *     show-columns: "channelName,hostName,iocName"   # DS columns for wildcard query
 *     rescan-interval-sec: 300.0
 *     worker-thread-count: 4
 *     max-queue-depth: 16
 *     pvs:                                            # targeted per-PV enrichment
 *       - name: BPMS:LI20:2445:X
 *         metadata:
 *           system: bpm
 *           area: li20
 *       - name: QUAD:LI21:221:BACT
 *     pv-show-columns: "dname,ename,etype,lname,ioc,scheme,z"
 * @endcode
 */
class EpicsDSMetadataReaderConfig
{
public:
    struct PVEntry {
        std::string                                     name;
        std::unordered_map<std::string, std::string>    metadata;
    };

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
     * @brief Reader instance name (required, non-empty).
     */
    const std::string& name() const noexcept { return name_; }

    /**
     * @brief PVA service name to call via RPC (default: "ds").
     */
    const std::string& service() const noexcept { return service_; }

    /**
     * @brief Query pattern sent in the NTURI query.name field (default: "%").
     */
    const std::string& query() const noexcept { return query_; }

    /**
     * @brief RPC call timeout in seconds (default: 5.0, must be positive).
     */
    double timeoutSec() const noexcept { return timeout_sec_; }

    /**
     * @brief NTTable column name that holds the PV/source name (default: "channelName").
     */
    const std::string& sourceNameColumn() const noexcept { return source_name_column_; }

    /**
     * @brief NTTable column name that holds comma-separated tags (default: "", disabled).
     */
    const std::string& tagsColumn() const noexcept { return tags_column_; }

    /**
     * @brief Comma-separated list of columns to request via the `show` query parameter.
     *        Empty string means no `show` parameter is sent (server returns all columns).
     *        Example: "name,elementname,hostName"
     */
    const std::string& showColumns() const noexcept { return show_columns_; }

    /**
     * @brief Parsed per-PV entries for targeted DS lookups.
     */
    const std::vector<PVEntry>& pvs() const noexcept { return pvs_; }

    /**
     * @brief Parsed subset of supported DS `show=` values for PV-list mode.
     *        Defaults to `dname,ename,etype,lname,ioc,scheme,z` when omitted or blank.
     */
    const std::vector<std::string>& pvShowColumns() const noexcept { return pv_show_columns_; }

    /**
     * @brief Interval between periodic re-fetches in seconds (default: 0.0 = run once).
     */
    double rescanIntervalSec() const noexcept { return rescan_interval_sec_; }

    /**
     * @brief Total thread count: 1 = single-thread mode; N>1 = 1 producer + (N-1) consumers.
     */
    std::size_t workerThreadCount() const noexcept { return worker_thread_count_; }

    /**
     * @brief Bounded queue depth for producer/consumer mode (ignored when workerThreadCount()==1).
     */
    std::size_t maxQueueDepth() const noexcept { return max_queue_depth_; }

private:
    void parse(const config::Config& cfg);

    std::string name_;
    std::string service_{"ds"};
    std::string query_{"%"};
    double      timeout_sec_{5.0};
    std::string source_name_column_{"channelName"};
    std::string tags_column_;
    std::string show_columns_;
    std::vector<PVEntry> pvs_;
    std::vector<std::string> pv_show_columns_;
    double      rescan_interval_sec_{0.0};
    std::size_t worker_thread_count_{1};
    std::size_t max_queue_depth_{16};
};

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
