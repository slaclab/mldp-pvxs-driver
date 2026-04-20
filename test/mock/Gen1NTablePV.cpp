//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "Gen1NTablePV.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include <pvxs/nt.h>

using namespace pvxs;

std::string Gen1NTablePV::sanitizeFieldName(const std::string& pvName)
{
    std::string out = pvName;
    for (char& c : out)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    }
    // Field names must not start with a digit
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), '_');
    return out;
}

Gen1NTablePV::Gen1NTablePV(std::string pvName, std::string signalsFile)
    : m_pvName(std::move(pvName))
{
    // Load column names from file (one per line, blank lines ignored)
    std::ifstream ifs(signalsFile);
    if (!ifs)
        throw std::runtime_error("Gen1NTablePV: cannot open signals file: " + signalsFile);

    std::string line;
    while (std::getline(ifs, line))
    {
        if (!line.empty())
        {
            m_columnNames.push_back(line);
            m_fieldNames.push_back(sanitizeFieldName(line));
        }
    }

    if (m_columnNames.empty())
        throw std::runtime_error("Gen1NTablePV: no signals found in " + signalsFile);

    // Build NTTable type: all data columns Float64, then two UInt32 timestamp columns
    nt::NTTable builder;
    for (const auto& field : m_fieldNames)
        builder.add_column(TypeCode::Float64, field.c_str());
    builder.add_column(TypeCode::UInt32, "secondsPastEpoch");
    builder.add_column(TypeCode::UInt32, "nanoseconds");

    m_pv = server::SharedPV::buildReadonly();
    m_pv.open(builder.build().create());
}

void Gen1NTablePV::registerPV(pvxs::server::Server& server)
{
    server.addPV(m_pvName, m_pv);
}

void Gen1NTablePV::post(long long seconds, long long nanos, int /*counter*/, double time)
{
    auto pv = m_pv.fetch();

    // labels: data columns + timestamp columns
    const size_t nData  = m_columnNames.size();
    const size_t nTotal = nData + 2;
    pvxs::shared_array<std::string> labels(nTotal);
    for (size_t i = 0; i < nData; ++i)
        labels[i] = m_columnNames[i];
    labels[nData]     = "secondsPastEpoch";
    labels[nData + 1] = "nanoseconds";
    pv["labels"] = labels.freeze();

    // data columns — all Float64, synthetic sinusoidal values
    for (size_t ci = 0; ci < nData; ++ci)
    {
        pvxs::shared_array<double> col(kRows);
        for (int r = 0; r < kRows; ++r)
            col[r] = 1.0 + std::sin(time + static_cast<double>(ci) * 0.1 + static_cast<double>(r) * 0.05);
        pv[std::string("value.") + m_fieldNames[ci]] = col.freeze();
    }

    // per-row timestamps
    pvxs::shared_array<uint32_t> secArr(kRows);
    pvxs::shared_array<uint32_t> nanoArr(kRows);
    for (int r = 0; r < kRows; ++r)
    {
        secArr[r]  = static_cast<uint32_t>(seconds);
        nanoArr[r] = static_cast<uint32_t>(nanos + r);
    }
    pv["value.secondsPastEpoch"] = secArr.freeze();
    pv["value.nanoseconds"]      = nanoArr.freeze();

    // table-level timestamp
    pv["timeStamp.secondsPastEpoch"] = seconds;
    pv["timeStamp.nanoseconds"]      = nanos;

    m_pv.post(pv);
}
