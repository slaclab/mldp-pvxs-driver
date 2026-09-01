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
 * @file   MockDSServer.h
 * @brief  In-process mock EPICS Directory Service for unit and integration tests.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 */
#pragma once

#include <pvxs/nt.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/srvcommon.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::test::mock {

using DsRow = std::unordered_map<std::string, std::string>;

/**
 * @class  MockDSServer
 * @brief  In-process mock EPICS Directory Service that serves PV metadata via pvxs RPC.
 * @details
 *   Responds to ChannelRPC requests on the configured channel with an NTTable
 *   built from a 30-row built-in dataset (ds-mock-data.jsonl).  The dataset
 *   can be mutated at runtime so that subsequent RPC calls reflect changes,
 *   enabling tests to verify that updated/added/removed rows propagate through
 *   the reader pipeline on the next scan.
 * @note Thread-safe: all mutation methods lock an internal mutex.
 */
class MockDSServer {
public:
    /**
     * @brief Construct and start the mock DS server.
     * @param[in] channel PVA channel name to serve RPC on (default: "ds").
     * @param[in] dataDir Directory containing ds-mock-data.json; uses
     *                    MLDP_TEST_DATA_DIR env var when empty.
     */
    explicit MockDSServer(std::string channel = "ds",
                          std::string dataDir = "");
    ~MockDSServer();

    /**
     * @brief Return the PVA channel name this server is listening on.
     * @return Channel name string.
     */
    std::string        channelName() const { return m_channel; }

    /**
     * @brief Return current number of rows in the dataset.
     * @return Row count.
     */
    size_t             rowCount() const;

    /**
     * @brief Return a snapshot copy of the current dataset.
     * @return Vector of row maps.
     */
    std::vector<DsRow> rows() const;

    /**
     * @brief Update one attribute of an existing row identified by channelName.
     * @param[in] channelName Key of the row to update.
     * @param[in] column      Column name to update.
     * @param[in] value       New value.
     * @note No-op if channelName not found.
     */
    void updateAttribute(const std::string& channelName,
                         const std::string& column,
                         const std::string& value);

    /**
     * @brief Replace the tags field of an existing row.
     * @param[in] channelName Key of the row to update.
     * @param[in] tags        New comma-separated tags string.
     * @note No-op if channelName not found.
     */
    void updateTags(const std::string& channelName, const std::string& tags);

    /**
     * @brief Append a new row; visible on the next RPC call.
     * @param[in] row Row map to append.
     */
    void addRow(DsRow row);

    /**
     * @brief Remove a row by channelName.
     * @param[in] channelName Key of the row to remove.
     * @note No-op if not found.
     */
    void removeRow(const std::string& channelName);

    /**
     * @brief Atomically replace the entire dataset.
     * @param[in] rows New dataset to install.
     */
    void setRows(std::vector<DsRow> rows);

    static constexpr std::array<const char*, 10> kColumns = {
        "channelName", "hostName",   "iocName",    "owner",       "pvStatus",
        "recordType",  "recordDesc", "archived",   "archiveRate", "tags"};

private:
    void        loadRows(const std::string& dataDir);
    // caller must hold m_mutex
    // nameFilter: exact channelName or "%" (all); showCol: single column or "" (all columns)
    pvxs::Value buildNTTableResponse(const std::string& nameFilter = "%",
                                     const std::string& showCol    = "") const;

    std::string          m_channel;
    pvxs::server::Server m_server;

    mutable std::mutex m_mutex;
    std::vector<DsRow> m_rows;
};

} // namespace mldp_pvxs_driver::test::mock
