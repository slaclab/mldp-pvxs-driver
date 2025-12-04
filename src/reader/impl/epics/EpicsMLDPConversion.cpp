#include <reader/impl/epics/EpicsMLDPConversion.h>

using namespace mldp_pvxs_driver::reader::impl::epics;

void EpicsMLDPConversion::convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue)
{
    const auto typeArraySetter = [protoValue](const auto& pvArray, auto setter)
    {
        auto* protoArray = protoValue->mutable_arrayvalue();
        for (const auto& elementValue : pvArray)
        {
            auto* element = protoArray->add_datavalues();
            (*element.*setter)(elementValue);
        }
    };

    const auto structSetter = [](const pvxs::Value& structValue, DataValue* protoStruct)
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
            EpicsMLDPConversion::convertPVToProtoValue(member, field->mutable_value());
        }
    };

    switch (pvValue.type().code)
    {
    case pvxs::TypeCode::Bool: protoValue->set_booleanvalue(pvValue.as<bool>()); break;
    case pvxs::TypeCode::BoolA: typeArraySetter(pvValue.as<pvxs::shared_array<const bool>>(), &DataValue::set_booleanvalue); break;
    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::Int32: protoValue->set_intvalue(pvValue.as<int32_t>()); break;
    case pvxs::TypeCode::Int64: protoValue->set_longvalue(pvValue.as<int64_t>()); break;
    case pvxs::TypeCode::UInt8:
    case pvxs::TypeCode::UInt16:
    case pvxs::TypeCode::UInt32: protoValue->set_uintvalue(pvValue.as<uint32_t>()); break;
    case pvxs::TypeCode::UInt64: protoValue->set_ulongvalue(pvValue.as<uint64_t>()); break;
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A: typeArraySetter(pvValue.as<pvxs::shared_array<const int32_t>>(), &DataValue::set_intvalue); break;
    case pvxs::TypeCode::Int64A: typeArraySetter(pvValue.as<pvxs::shared_array<const int64_t>>(), &DataValue::set_longvalue); break;
    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A: typeArraySetter(pvValue.as<pvxs::shared_array<const uint32_t>>(), &DataValue::set_uintvalue); break;
    case pvxs::TypeCode::UInt64A: typeArraySetter(pvValue.as<pvxs::shared_array<const uint64_t>>(), &DataValue::set_ulongvalue); break;
    case pvxs::TypeCode::Float32: protoValue->set_floatvalue(pvValue.as<float>()); break;
    case pvxs::TypeCode::Float64: protoValue->set_doublevalue(pvValue.as<double>()); break;
    case pvxs::TypeCode::Float32A: typeArraySetter(pvValue.as<pvxs::shared_array<const float>>(), &DataValue::set_floatvalue); break;
    case pvxs::TypeCode::Float64A: typeArraySetter(pvValue.as<pvxs::shared_array<const double>>(), &DataValue::set_doublevalue); break;
    case pvxs::TypeCode::String: protoValue->set_stringvalue(pvValue.as<std::string>()); break;
    case pvxs::TypeCode::StringA:
        typeArraySetter(
            pvValue.as<pvxs::shared_array<const std::string>>(),
            static_cast<void (DataValue::*)(const std::string&)>(&DataValue::set_stringvalue));
        break;
    case pvxs::TypeCode::Struct:
    case pvxs::TypeCode::Union:
    case pvxs::TypeCode::Any: structSetter(pvValue, protoValue); break;
    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
        {
            const auto pvArray = pvValue.as<pvxs::shared_array<const pvxs::Value>>();
            for (const auto& structValue : pvArray)
            {
                structSetter(structValue, protoValue->mutable_arrayvalue()->add_datavalues());
            }
            break;
        }
    case pvxs::TypeCode::Null: protoValue->set_stringvalue("null"); break;
    }
}
