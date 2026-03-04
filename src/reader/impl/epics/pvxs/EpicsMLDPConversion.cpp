//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsMLDPConversion.h>
#include <cstdint>

using namespace mldp_pvxs_driver::reader::impl::epics;
using DataFrame = dp::service::common::DataFrame;

/*
 * Conversion flow (single PV field -> DataFrame columns):
 * 1) Inspect pvValue.type().code and dispatch by PVXS type family.
 * 2) Scalars map to one typed scalar column with one value.
 * 3) Scalar arrays map to one typed array column preserving full array payload.
 * 4) Compound values recurse into children using dotted names:
 *      parent.child[.grandchild...]
 * 5) Null values are represented as a string column with literal "null".
 *
 * Notes:
 * - Unsigned integers are normalized into signed MLDP column types:
 *   UInt8/16/32 -> Int32, UInt64 -> Int64 (same rule for arrays).
 * - String arrays are encoded as DataColumn/ArrayValue to preserve nested semantics.
 */
void EpicsMLDPConversion::convertPVToDataFrame(const pvxs::Value& pvValue,
                                               DataFrame*         frame,
                                               const std::string& columnName)
{
    // For compound types, recursively expand children as sub-columns named
    // "columnName.fieldName".
    static const auto structSetter = [](const pvxs::Value& structValue,
                                        DataFrame*         df,
                                        const std::string& name)
    {
        for (const auto& member : structValue.ichildren())
        {
            if (!member.valid())
            {
                continue;
            }
            const std::string subName = name + "." + std::string(structValue.nameOf(member));
            convertPVToDataFrame(member, df, subName);
        }
    };

    switch (pvValue.type().code)
    {
    // --- Scalar primitives --------------------------------------------------
    case pvxs::TypeCode::Bool:
    {
        auto* c = frame->add_boolcolumns();
        c->set_name(columnName);
        c->add_values(pvValue.as<bool>());
        return;
    }
    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::Int32:
    {
        auto* c = frame->add_int32columns();
        c->set_name(columnName);
        c->add_values(pvValue.as<int32_t>());
        return;
    }
    case pvxs::TypeCode::Int64:
    {
        auto* c = frame->add_int64columns();
        c->set_name(columnName);
        c->add_values(pvValue.as<int64_t>());
        return;
    }
    case pvxs::TypeCode::UInt8:
    case pvxs::TypeCode::UInt16:
    case pvxs::TypeCode::UInt32:
    {
        auto* c = frame->add_int32columns();
        c->set_name(columnName);
        c->add_values(static_cast<int32_t>(pvValue.as<uint32_t>()));
        return;
    }
    case pvxs::TypeCode::UInt64:
    {
        auto* c = frame->add_int64columns();
        c->set_name(columnName);
        c->add_values(static_cast<int64_t>(pvValue.as<uint64_t>()));
        return;
    }
    case pvxs::TypeCode::Float32:
    {
        auto* c = frame->add_floatcolumns();
        c->set_name(columnName);
        c->add_values(pvValue.as<float>());
        return;
    }
    case pvxs::TypeCode::Float64:
    {
        auto* c = frame->add_doublecolumns();
        c->set_name(columnName);
        c->add_values(pvValue.as<double>());
        return;
    }
    case pvxs::TypeCode::String:
    {
        auto* c = frame->add_stringcolumns();
        c->set_name(columnName);
        c->add_values(pvValue.as<std::string>());
        return;
    }

    // --- Scalar array payloads ---------------------------------------------
    // Arrays are kept as array columns rather than exploded into scalar rows.
    case pvxs::TypeCode::BoolA:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const bool>>();
        auto*      c   = frame->add_boolarraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(v);
        }
        return;
    }
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const int32_t>>();
        auto*      c   = frame->add_int32arraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(v);
        }
        return;
    }
    case pvxs::TypeCode::Int64A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const int64_t>>();
        auto*      c   = frame->add_int64arraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(v);
        }
        return;
    }
    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const uint32_t>>();
        auto*      c   = frame->add_int32arraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(static_cast<int32_t>(v));
        }
        return;
    }
    case pvxs::TypeCode::UInt64A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const uint64_t>>();
        auto*      c   = frame->add_int64arraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(static_cast<int64_t>(v));
        }
        return;
    }
    case pvxs::TypeCode::Float32A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const float>>();
        auto*      c   = frame->add_floatarraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(v);
        }
        return;
    }
    case pvxs::TypeCode::Float64A:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const double>>();
        auto*      c   = frame->add_doublearraycolumns();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto v : arr)
        {
            c->add_values(v);
        }
        return;
    }
    case pvxs::TypeCode::StringA:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const std::string>>();
        auto*      c   = frame->add_datacolumns();
        c->set_name(columnName);
        auto* sample = c->add_datavalues();
        auto* list   = sample->mutable_arrayvalue();
        for (const auto& v : arr)
        {
            list->add_datavalues()->set_stringvalue(v);
        }
        return;
    }

    // --- Compound / nested payloads ----------------------------------------
    // Recursively flatten nested fields to dotted column names.
    case pvxs::TypeCode::Struct:
    case pvxs::TypeCode::Union:
    case pvxs::TypeCode::Any:
        structSetter(pvValue, frame, columnName);
        return;
    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
    {
        const auto arr = pvValue.as<pvxs::shared_array<const pvxs::Value>>();
        for (const auto& cell : arr)
        {
            structSetter(cell, frame, columnName);
        }
        return;
    }

    // --- Explicit null ------------------------------------------------------
    case pvxs::TypeCode::Null:
    {
        auto* c = frame->add_stringcolumns();
        c->set_name(columnName);
        c->add_values("null");
        return;
    }
    }
}
