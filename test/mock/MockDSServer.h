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

/// Mock EPICS Directory Service that serves PV metadata via pvxs RPC.
///
/// Responds to ChannelRPC requests on the configured channel with an NTTable
/// built from a 30-row built-in dataset (ds-mock-data.jsonl).  The dataset
/// can be mutated at runtime so that subsequent RPC calls reflect changes —
/// enabling tests to verify that updated/added/removed rows propagate through
/// the reader pipeline on the next scan.
///
/// Thread-safe: all mutation methods lock an internal mutex.
class MockDSServer {
public:
    /// channel: PVA channel name to serve RPC on (default "ds")
    /// dataDir: directory containing ds-mock-data.json; uses MLDP_TEST_DATA_DIR
    ///          env var when empty (same convention as rest of test suite)
    explicit MockDSServer(std::string channel = "ds",
                          std::string dataDir = "");
    ~MockDSServer();

    std::string        channelName() const { return m_channel; }
    size_t             rowCount() const;
    std::vector<DsRow> rows() const;

    // ── Mutation API ────────────────────────────────────────────────────────

    /// Update one attribute of an existing row (identified by channelName).
    /// No-op if channelName not found.
    void updateAttribute(const std::string& channelName,
                         const std::string& column,
                         const std::string& value);

    /// Replace the tags field of an existing row.
    /// No-op if channelName not found.
    void updateTags(const std::string& channelName, const std::string& tags);

    /// Append a new row. Visible on next RPC call.
    void addRow(DsRow row);

    /// Remove a row by channelName. No-op if not found.
    void removeRow(const std::string& channelName);

    /// Atomically replace the entire dataset.
    void setRows(std::vector<DsRow> rows);

    static constexpr std::array<const char*, 10> kColumns = {
        "channelName", "hostName",   "iocName",    "owner",       "pvStatus",
        "recordType",  "recordDesc", "archived",   "archiveRate", "tags"};

private:
    void        loadRows(const std::string& dataDir);
    pvxs::Value buildNTTableResponse() const; // caller must hold m_mutex

    std::string          m_channel;
    pvxs::server::Server m_server;

    mutable std::mutex m_mutex;
    std::vector<DsRow> m_rows;
};

} // namespace mldp_pvxs_driver::test::mock
