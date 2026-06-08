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
 * @file   MockDSServer.cpp
 * @brief  Implementation of MockDSServer.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 */
#include "MockDSServer.h"

#include <pvxs/nt.h>
#include <pvxs/server.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace pvxs;

namespace mldp_pvxs_driver::test::mock {

// ── JSONL parsing helpers ────────────────────────────────────────────────────

/// Extract the string value for `key` from a flat JSON object line.
/// Handles the pattern: "key":"value" — stops at the closing quote.
static std::string extractJsonString(const std::string& line,
                                     const std::string& key)
{
    const std::string searchKey = "\"" + key + "\":\"";
    const auto        pos       = line.find(searchKey);
    if (pos == std::string::npos)
        return "";
    const auto valStart = pos + searchKey.size();
    const auto valEnd   = line.find('"', valStart);
    if (valEnd == std::string::npos)
        return "";
    return line.substr(valStart, valEnd - valStart);
}

static std::vector<DsRow> parseJsonl(const std::string& jsonl)
{
    std::vector<DsRow> rows;
    std::istringstream ss(jsonl);
    std::string        line;
    while (std::getline(ss, line))
    {
        if (line.empty() || line[0] != '{')
            continue;
        DsRow row;
        for (const char* col : MockDSServer::kColumns)
            row[col] = extractJsonString(line, col);
        rows.push_back(std::move(row));
    }
    return rows;
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

MockDSServer::MockDSServer(std::string channel, std::string jsonlPath)
    : m_channel(std::move(channel))
{
    loadRows(jsonlPath);

    m_server = server::Config::fromEnv().build();

    auto rpcPV = server::SharedPV::buildMailbox();
    rpcPV.onRPC([this](server::SharedPV&,
                       std::unique_ptr<server::ExecOp>&& op,
                       pvxs::Value&&                     arg)
                {
                    std::string nameFilter = "%";
                    std::string showCol;
                    if (arg.valid())
                    {
                        auto nameField = arg["query.name"];
                        if (nameField.valid())
                            nameFilter = nameField.as<std::string>();
                        auto showField = arg["query.show"];
                        if (showField.valid())
                            showCol = showField.as<std::string>();
                    }
                    std::lock_guard<std::mutex> lk(m_mutex);
                    op->reply(buildNTTableResponse(nameFilter, showCol));
                });

    m_server.addPV(m_channel, rpcPV);
    m_server.start();
}

MockDSServer::~MockDSServer()
{
    m_server.stop();
}

// ── Private helpers ──────────────────────────────────────────────────────────

void MockDSServer::loadRows(const std::string& dataDir)
{
    std::string dir = dataDir;
    if (dir.empty())
    {
        const char* envDir = std::getenv("MLDP_TEST_DATA_DIR");
        if (envDir && *envDir)
            dir = envDir;
    }
#ifdef MLDP_TEST_DATA_DIR
    if (dir.empty())
        dir = MLDP_TEST_DATA_DIR;
#endif
    const std::string path = dir + "/ds-mock-data.json";
    std::ifstream     ifs(path);
    if (!ifs)
        throw std::runtime_error("MockDSServer: cannot open " + path);
    std::ostringstream buf;
    buf << ifs.rdbuf();
    m_rows = parseJsonl(buf.str());
}

pvxs::Value MockDSServer::buildNTTableResponse(const std::string& nameFilter,
                                               const std::string& showCol) const
{
    // Filter rows
    std::vector<const DsRow*> filtered;
    for (const auto& row : m_rows)
    {
        if (nameFilter.empty() || nameFilter == "%")
        {
            filtered.push_back(&row);
        }
        else
        {
            auto it = row.find("channelName");
            if (it != row.end() && it->second == nameFilter)
                filtered.push_back(&row);
        }
    }

    // Determine columns to emit
    std::vector<std::string> cols;
    if (!showCol.empty())
    {
        cols.push_back(showCol);
    }
    else
    {
        for (const char* c : kColumns)
            cols.emplace_back(c);
    }

    nt::NTTable builder;
    for (const auto& col : cols)
        builder.add_column(TypeCode::String, col.c_str());
    pvxs::Value val = builder.build().create();

    shared_array<std::string> labels(cols.size());
    for (size_t i = 0; i < cols.size(); ++i)
        labels[i] = cols[i];
    val["labels"] = labels.freeze();

    for (const auto& col : cols)
    {
        shared_array<std::string> colArr(filtered.size());
        for (size_t r = 0; r < filtered.size(); ++r)
        {
            auto it   = filtered[r]->find(col);
            colArr[r] = (it != filtered[r]->end()) ? it->second : "";
        }
        val[std::string("value.") + col] = colArr.freeze();
    }
    return val;
}

// ── Public accessors ─────────────────────────────────────────────────────────

size_t MockDSServer::rowCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_rows.size();
}

std::vector<DsRow> MockDSServer::rows() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_rows;
}

// ── Mutation API ─────────────────────────────────────────────────────────────

void MockDSServer::updateAttribute(const std::string& channelName,
                                   const std::string& column,
                                   const std::string& value)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& row : m_rows)
    {
        auto it = row.find("channelName");
        if (it != row.end() && it->second == channelName)
        {
            row[column] = value;
            return;
        }
    }
}

void MockDSServer::updateTags(const std::string& channelName,
                              const std::string& tags)
{
    // NOTE: do NOT delegate to updateAttribute() — that would deadlock because
    // both methods take m_mutex.  Replicate the lookup inline instead.
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& row : m_rows)
    {
        auto it = row.find("channelName");
        if (it != row.end() && it->second == channelName)
        {
            row["tags"] = tags;
            return;
        }
    }
}

void MockDSServer::addRow(DsRow row)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_rows.push_back(std::move(row));
}

void MockDSServer::removeRow(const std::string& channelName)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_rows.erase(
        std::remove_if(m_rows.begin(), m_rows.end(),
                       [&channelName](const DsRow& r)
                       {
                           auto it = r.find("channelName");
                           return it != r.end() && it->second == channelName;
                       }),
        m_rows.end());
}

void MockDSServer::setRows(std::vector<DsRow> rows)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_rows = std::move(rows);
}

} // namespace mldp_pvxs_driver::test::mock
