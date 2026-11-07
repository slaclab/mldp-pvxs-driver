//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "BsasTablePV.h"

#include <stdexcept>

#include <pvxs/nt.h>

using namespace pvxs;

namespace {

template <typename T>
shared_array<const T> makeArray(const BsasTablePV::Column& col, int counter, size_t rows)
{
    shared_array<T> arr(rows);
    for (size_t r = 0; r < rows; ++r)
    {
        arr[r] = static_cast<T>(col.value(counter, r));
    }
    return arr.freeze();
}

} // namespace

BsasTablePV::BsasTablePV(std::string pvName, std::vector<Column> columns, Options options)
    : m_pvName(std::move(pvName))
    , m_columns(std::move(columns))
    , m_options(std::move(options))
{
    nt::NTTable builder;
    for (const auto& col : m_columns)
    {
        builder.add_column(col.type, col.field.c_str());
    }
    if (!m_options.tsSecondsField.empty())
    {
        builder.add_column(TypeCode::UInt32, m_options.tsSecondsField.c_str());
        builder.add_column(TypeCode::UInt32, m_options.tsNanosField.c_str());
    }
    m_pv = server::SharedPV::buildReadonly();
    m_pv.open(builder.build().create());
}

void BsasTablePV::registerPV(server::Server& server)
{
    server.addPV(m_pvName, m_pv);
}

void BsasTablePV::post(int counter)
{
    auto         pv = m_pv.fetch();
    const size_t rows = m_options.rows;
    const bool   withTs = !m_options.tsSecondsField.empty();

    if (m_options.withLabels)
    {
        shared_array<std::string> labels(m_columns.size() + (withTs ? 2 : 0));
        size_t                    i = 0;
        for (const auto& col : m_columns)
        {
            labels[i++] = col.label.empty() ? col.field : col.label;
        }
        if (withTs)
        {
            labels[i++] = m_options.tsSecondsField;
            labels[i++] = m_options.tsNanosField;
        }
        pv["labels"] = labels.freeze();
    }

    for (const auto& col : m_columns)
    {
        auto field = pv["value"][col.field];
        if (!field.valid())
            throw std::runtime_error("BsasTablePV: missing field " + col.field);
        if (col.type == TypeCode::Float64)
            field.from(makeArray<double>(col, counter, rows));
        else if (col.type == TypeCode::Float32)
            field.from(makeArray<float>(col, counter, rows));
        else if (col.type == TypeCode::Int32)
            field.from(makeArray<int32_t>(col, counter, rows));
        else if (col.type == TypeCode::UInt32)
            field.from(makeArray<uint32_t>(col, counter, rows));
        else if (col.type == TypeCode::Int64)
            field.from(makeArray<int64_t>(col, counter, rows));
        else if (col.type == TypeCode::UInt64)
            field.from(makeArray<uint64_t>(col, counter, rows));
        else
            throw std::runtime_error("BsasTablePV: unsupported column type for " + col.field);
    }

    const uint32_t seconds = kBaseSeconds + static_cast<uint32_t>(counter);
    if (withTs)
    {
        shared_array<uint32_t> secArr(rows);
        shared_array<uint32_t> nanoArr(rows);
        for (size_t r = 0; r < rows; ++r)
        {
            secArr[r] = seconds;
            nanoArr[r] = static_cast<uint32_t>(r) * kRowNanosStep;
        }
        pv["value"][m_options.tsSecondsField] = secArr.freeze();
        pv["value"][m_options.tsNanosField] = nanoArr.freeze();
    }

    pv["timeStamp.secondsPastEpoch"] = seconds;
    pv["timeStamp.nanoseconds"] = 0;

    m_pv.post(pv);
}
