//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/EpicsMLDPConversion.h>

using namespace mldp_pvxs_driver::reader::impl::epics;

void EpicsMLDPConversion::convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue)
{
    const auto typeValueSetter = [&pvValue, protoValue]<typename T>(void (DataValue::*setter)(T))
    {
        (*protoValue.*setter)(pvValue.as<std::remove_cvref_t<T>>());
    };

    const auto typeArraySetter = [&pvValue, protoValue]<typename T>(void (DataValue::*setter)(T))
    {
        auto* protoArray = protoValue->mutable_arrayvalue();
        for (const auto& i : pvValue.as<pvxs::shared_array<const std::remove_cvref_t<T>>>())
        {
            auto* element = protoArray->add_datavalues();
            (*element.*setter)(i);
        }
    };

    static constexpr auto structSetter = [](const pvxs::Value& structValue, DataValue* protoStruct)
    {
        auto* structure = protoStruct->mutable_structurevalue();
        for (const auto& member : structValue.ichildren())
        {
            if (!member.valid())
            {
                continue;
            }
            auto* field = structure->add_fields();
            field->set_name(structValue.nameOf(member));
            convertPVToProtoValue(member, field->mutable_value());
        }
    };

    switch (pvValue.type().code)
    {
    case pvxs::TypeCode::Bool: return typeValueSetter(&DataValue::set_booleanvalue);
    case pvxs::TypeCode::BoolA: return typeArraySetter(&DataValue::set_booleanvalue);
    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::Int32: return typeValueSetter(&DataValue::set_intvalue);
    case pvxs::TypeCode::Int64: return typeValueSetter(&DataValue::set_longvalue);
    case pvxs::TypeCode::UInt8:
    case pvxs::TypeCode::UInt16:
    case pvxs::TypeCode::UInt32: return typeValueSetter(&DataValue::set_uintvalue);
    case pvxs::TypeCode::UInt64: return typeValueSetter(&DataValue::set_ulongvalue);
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A: return typeArraySetter(&DataValue::set_intvalue);
    case pvxs::TypeCode::Int64A: return typeArraySetter(&DataValue::set_longvalue);
    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A: return typeArraySetter(&DataValue::set_uintvalue);
    case pvxs::TypeCode::UInt64A: return typeArraySetter(&DataValue::set_ulongvalue);
    case pvxs::TypeCode::Float32: return typeValueSetter(&DataValue::set_floatvalue);
    case pvxs::TypeCode::Float64: return typeValueSetter(&DataValue::set_doublevalue);
    case pvxs::TypeCode::Float32A: return typeArraySetter(&DataValue::set_floatvalue);
    case pvxs::TypeCode::Float64A: return typeArraySetter(&DataValue::set_doublevalue);
    case pvxs::TypeCode::String: return typeValueSetter(&DataValue::set_stringvalue<const std::string&>);
    case pvxs::TypeCode::StringA: return typeArraySetter(&DataValue::set_stringvalue<const std::string&>);
    case pvxs::TypeCode::Struct:
    case pvxs::TypeCode::Union:
    case pvxs::TypeCode::Any: return structSetter(pvValue, protoValue);
    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
        {
            for (const auto& i : pvValue.as<pvxs::shared_array<const pvxs::Value>>())
            {
                structSetter(i, protoValue->mutable_arrayvalue()->add_datavalues());
            }
            return;
        }
    case pvxs::TypeCode::Null: return protoValue->set_stringvalue("null");
    }
}
