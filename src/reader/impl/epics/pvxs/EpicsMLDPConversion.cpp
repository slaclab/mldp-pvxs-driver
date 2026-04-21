//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <reader/impl/epics/pvxs/EpicsMLDPConversion.h>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::bus;

/*
 * Conversion flow (single PV field -> DataBatch columns):
 * 1) Inspect pvValue.type().code and dispatch by PVXS type family.
 * 2) Scalars map to one DataColumn with a single-element typed vector.
 * 3) Scalar arrays map to one DataColumn with a vector-of-vectors (one inner
 *    vector containing the full array), with dims recorded in array_dims.
 * 4) Compound values recurse into children using dotted names:
 *      parent.child[.grandchild...]
 * 5) Null values are represented as a string column with literal "null".
 *
 * Notes:
 * - Unsigned integers are normalized into signed ColumnValues types:
 *   UInt8/16/32 -> int32_t, UInt64 -> int64_t (same rule for arrays).
 * - StringA is stored as std::vector<std::string> with array_dims recorded so
 *   downstream consumers can distinguish scalar-string from string-array columns.
 */
void EpicsMLDPConversion::convertPVToDataBatch(const pvxs::Value& pvValue,
                                               DataBatch*         batch,
                                               const std::string& columnName)
{
    // For compound types, recursively expand children as sub-columns named
    // "columnName.fieldName".
    static const auto structSetter = [](const pvxs::Value& structValue,
                                        DataBatch*         db,
                                        const std::string& name)
    {
        for (const auto& member : structValue.ichildren())
        {
            if (!member.valid())
            {
                continue;
            }
            const std::string subName = name + "." + std::string(structValue.nameOf(member));
            convertPVToDataBatch(member, db, subName);
        }
    };

    // Helper: append a scalar DataColumn with a single-element vector.
    const auto appendScalarColumn = [&](ColumnValues values)
    {
        DataColumn col;
        col.name   = columnName;
        col.values = std::move(values);
        batch->columns.push_back(std::move(col));
    };

    // Helper: append an array DataColumn with a vector-of-one-inner-vector and
    // record dims in array_dims.
    const auto appendArrayColumn = [&](ColumnValues values, uint32_t size)
    {
        DataColumn col;
        col.name   = columnName;
        col.values = std::move(values);
        batch->columns.push_back(std::move(col));
        batch->array_dims[columnName] = ArrayDims{{size}};
    };

    switch (pvValue.type().code)
    {
    // --- Scalar primitives --------------------------------------------------
    case pvxs::TypeCode::Bool:
        appendScalarColumn(std::vector<bool>{pvValue.as<bool>()});
        return;

    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::Int32:
        appendScalarColumn(std::vector<int32_t>{pvValue.as<int32_t>()});
        return;

    case pvxs::TypeCode::Int64:
        appendScalarColumn(std::vector<int64_t>{pvValue.as<int64_t>()});
        return;

    case pvxs::TypeCode::UInt8:
    case pvxs::TypeCode::UInt16:
    case pvxs::TypeCode::UInt32:
        appendScalarColumn(std::vector<int32_t>{static_cast<int32_t>(pvValue.as<uint32_t>())});
        return;

    case pvxs::TypeCode::UInt64:
        appendScalarColumn(std::vector<int64_t>{static_cast<int64_t>(pvValue.as<uint64_t>())});
        return;

    case pvxs::TypeCode::Float32:
        appendScalarColumn(std::vector<float>{pvValue.as<float>()});
        return;

    case pvxs::TypeCode::Float64:
        appendScalarColumn(std::vector<double>{pvValue.as<double>()});
        return;

    case pvxs::TypeCode::String:
        appendScalarColumn(std::vector<std::string>{pvValue.as<std::string>()});
        return;

    // --- Scalar array payloads ---------------------------------------------
    // Each array becomes a DataColumn whose ColumnValues is a vector-of-one-
    // inner-vector. Dims are recorded in array_dims.
    case pvxs::TypeCode::BoolA:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const bool>>();
            std::vector<bool> inner(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<bool>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const int32_t>>();
            std::vector<int32_t> inner(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<int32_t>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::Int64A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const int64_t>>();
            std::vector<int64_t> inner(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<int64_t>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const uint32_t>>();
            std::vector<int32_t> inner;
            inner.reserve(arr.size());
            for (const auto& v : arr)
                inner.push_back(static_cast<int32_t>(v));
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<int32_t>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::UInt64A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const uint64_t>>();
            std::vector<int64_t> inner;
            inner.reserve(arr.size());
            for (const auto& v : arr)
                inner.push_back(static_cast<int64_t>(v));
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<int64_t>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::Float32A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const float>>();
            std::vector<float> inner(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<float>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::Float64A:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const double>>();
            std::vector<double> inner(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(inner.size());
            appendArrayColumn(std::vector<std::vector<double>>{std::move(inner)}, sz);
            return;
        }

    case pvxs::TypeCode::StringA:
        {
            // StringA is stored as std::vector<std::string> (index 5) with dims.
            const auto arr = pvValue.as<pvxs::shared_array<const std::string>>();
            std::vector<std::string> vals(arr.begin(), arr.end());
            const auto sz = static_cast<uint32_t>(vals.size());
            DataColumn col;
            col.name   = columnName;
            col.values = std::move(vals);
            batch->columns.push_back(std::move(col));
            batch->array_dims[columnName] = ArrayDims{{sz}};
            return;
        }

    // --- Compound / nested payloads ----------------------------------------
    // Recursively flatten nested fields to dotted column names.
    case pvxs::TypeCode::Struct:
    case pvxs::TypeCode::Union:
    case pvxs::TypeCode::Any:
        structSetter(pvValue, batch, columnName);
        return;

    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const pvxs::Value>>();
            for (const auto& cell : arr)
            {
                structSetter(cell, batch, columnName);
            }
            return;
        }

    // --- Explicit null ------------------------------------------------------
    case pvxs::TypeCode::Null:
        appendScalarColumn(std::vector<std::string>{"null"});
        return;
    }
}
