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

    // Templated helpers to keep switch cases as compact type-dispatch only.
    const auto scalarIdentity = [](auto value)
    {
        return value;
    };

    const auto writeScalarColumn = [&]<typename PvType>(auto&& addColumn,
                                                        auto&& mapValue)
    {
        auto* c = addColumn();
        c->set_name(columnName);
        c->add_values(mapValue(pvValue.as<PvType>()));
    };

    const auto writeArrayColumn = [&]<typename PvType>(auto&& addColumn,
                                                       auto&& appendValue)
    {
        const auto arr = pvValue.as<pvxs::shared_array<const PvType>>();
        auto*      c = addColumn();
        c->set_name(columnName);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(arr.size()));
        c->mutable_values()->Reserve(static_cast<int>(arr.size()));
        for (const auto& v : arr)
        {
            appendValue(c, v);
        }
    };

    switch (pvValue.type().code)
    {
    // --- Scalar primitives --------------------------------------------------
    case pvxs::TypeCode::Bool:
        writeScalarColumn.template operator()<bool>([&]()
                                                    {
                                                        return frame->add_boolcolumns();
                                                    },
                                                    scalarIdentity);
        return;
    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::Int32:
        writeScalarColumn.template operator()<int32_t>(
            [&]()
            {
                return frame->add_int32columns();
            },
            scalarIdentity);
        return;
    case pvxs::TypeCode::Int64:
        writeScalarColumn.template operator()<int64_t>(
            [&]()
            {
                return frame->add_int64columns();
            },
            scalarIdentity);
        return;
    case pvxs::TypeCode::UInt8:
    case pvxs::TypeCode::UInt16:
    case pvxs::TypeCode::UInt32:
        writeScalarColumn.template operator()<uint32_t>(
            [&]()
            {
                return frame->add_int32columns();
            },
            [](uint32_t value)
            {
                return static_cast<int32_t>(value);
            });
        return;
    case pvxs::TypeCode::UInt64:
        writeScalarColumn.template operator()<uint64_t>(
            [&]()
            {
                return frame->add_int64columns();
            },
            [](uint64_t value)
            {
                return static_cast<int64_t>(value);
            });
        return;
    case pvxs::TypeCode::Float32:
        writeScalarColumn.template operator()<float>([&]()
                                                     {
                                                         return frame->add_floatcolumns();
                                                     },
                                                     scalarIdentity);
        return;
    case pvxs::TypeCode::Float64:
        writeScalarColumn.template operator()<double>(
            [&]()
            {
                return frame->add_doublecolumns();
            },
            scalarIdentity);
        return;
    case pvxs::TypeCode::String:
        writeScalarColumn.template operator()<std::string>(
            [&]()
            {
                return frame->add_stringcolumns();
            },
            scalarIdentity);
        return;

    // --- Scalar array payloads ---------------------------------------------
    // Arrays are kept as array columns rather than exploded into scalar rows.
    case pvxs::TypeCode::BoolA:
        writeArrayColumn.template operator()<bool>(
            [&]()
            {
                return frame->add_boolarraycolumns();
            },
            [](auto* c, bool value)
            {
                c->add_values(value);
            });
        return;
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
        writeArrayColumn.template operator()<int32_t>(
            [&]()
            {
                return frame->add_int32arraycolumns();
            },
            [](auto* c, int32_t value)
            {
                c->add_values(value);
            });
        return;
    case pvxs::TypeCode::Int64A:
        writeArrayColumn.template operator()<int64_t>(
            [&]()
            {
                return frame->add_int64arraycolumns();
            },
            [](auto* c, int64_t value)
            {
                c->add_values(value);
            });
        return;
    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
        writeArrayColumn.template operator()<uint32_t>(
            [&]()
            {
                return frame->add_int32arraycolumns();
            },
            [](auto* c, uint32_t value)
            {
                c->add_values(static_cast<int32_t>(value));
            });
        return;
    case pvxs::TypeCode::UInt64A:
        writeArrayColumn.template operator()<uint64_t>(
            [&]()
            {
                return frame->add_int64arraycolumns();
            },
            [](auto* c, uint64_t value)
            {
                c->add_values(static_cast<int64_t>(value));
            });
        return;
    case pvxs::TypeCode::Float32A:
        writeArrayColumn.template operator()<float>(
            [&]()
            {
                return frame->add_floatarraycolumns();
            },
            [](auto* c, float value)
            {
                c->add_values(value);
            });
        return;
    case pvxs::TypeCode::Float64A:
        writeArrayColumn.template operator()<double>(
            [&]()
            {
                return frame->add_doublearraycolumns();
            },
            [](auto* c, double value)
            {
                c->add_values(value);
            });
        return;
    case pvxs::TypeCode::StringA:
        {
            const auto arr = pvValue.as<pvxs::shared_array<const std::string>>();
            auto*      c = frame->add_datacolumns();
            c->set_name(columnName);
            auto* sample = c->add_datavalues();
            auto* list = sample->mutable_arrayvalue();
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
