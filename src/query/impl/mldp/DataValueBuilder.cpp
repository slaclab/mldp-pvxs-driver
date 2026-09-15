//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/DataValueBuilder.h>
#include <query/impl/mldp/MldpTimestampUtils.h>

#include <google/protobuf/message.h>

#include <stdexcept>
#include <string_view>

using namespace mldp_pvxs_driver::query::impl::mldp;

namespace {

std::shared_ptr<arrow::DataType> dataValueType()
{
    return arrow::dense_union({arrow::field("string", arrow::utf8()),
                               arrow::field("bool", arrow::boolean()),
                               arrow::field("uint32", arrow::uint32()),
                               arrow::field("uint64", arrow::uint64()),
                               arrow::field("int32", arrow::int32()),
                               arrow::field("int64", arrow::int64()),
                               arrow::field("float", arrow::float32()),
                               arrow::field("double", arrow::float64()),
                               arrow::field("binary", arrow::binary()),
                               arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO, "UTC")),
                               arrow::field("array", arrow::binary()),
                               arrow::field("structure", arrow::binary()),
                               arrow::field("image", arrow::binary())});
}

} // namespace

std::string_view mldp_pvxs_driver::query::impl::mldp::dataValueKind(const dp::service::common::DataValue& value)
{
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue: return "string";
    case dp::service::common::DataValue::kBooleanValue: return "bool";
    case dp::service::common::DataValue::kUintValue: return "uint32";
    case dp::service::common::DataValue::kUlongValue: return "uint64";
    case dp::service::common::DataValue::kIntValue: return "int32";
    case dp::service::common::DataValue::kLongValue: return "int64";
    case dp::service::common::DataValue::kFloatValue: return "float";
    case dp::service::common::DataValue::kDoubleValue: return "double";
    case dp::service::common::DataValue::kByteArrayValue: return "binary";
    case dp::service::common::DataValue::kTimestampValue: return "timestamp";
    case dp::service::common::DataValue::kArrayValue: return "array";
    case dp::service::common::DataValue::kStructureValue: return "structure";
    case dp::service::common::DataValue::kImageValue: return "image";
    case dp::service::common::DataValue::VALUE_NOT_SET: return "null";
    }
    return "null";
}

DataValueBuilder::DataValueBuilder(arrow::MemoryPool* pool)
    : string_(std::make_shared<arrow::StringBuilder>(pool))
    , boolean_(std::make_shared<arrow::BooleanBuilder>(pool))
    , uint32_(std::make_shared<arrow::UInt32Builder>(pool))
    , uint64_(std::make_shared<arrow::UInt64Builder>(pool))
    , int32_(std::make_shared<arrow::Int32Builder>(pool))
    , int64_(std::make_shared<arrow::Int64Builder>(pool))
    , float_(std::make_shared<arrow::FloatBuilder>(pool))
    , double_(std::make_shared<arrow::DoubleBuilder>(pool))
    , binary_(std::make_shared<arrow::BinaryBuilder>(pool))
    , timestamp_(std::make_shared<arrow::TimestampBuilder>(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool))
    , array_(std::make_shared<arrow::BinaryBuilder>(pool))
    , structure_(std::make_shared<arrow::BinaryBuilder>(pool))
    , image_(std::make_shared<arrow::BinaryBuilder>(pool))
    , union_builder_(pool,
                     {string_, boolean_, uint32_, uint64_, int32_, int64_, float_, double_, binary_, timestamp_, array_, structure_, image_},
                     dataValueType())
{
}

void DataValueBuilder::append(const dp::service::common::DataValue& value)
{
    const auto appendSerialized = [&](int8_t type_id, arrow::BinaryBuilder& builder, const google::protobuf::Message& message)
    {
        const auto serialized = message.SerializeAsString();
        if (!union_builder_.Append(type_id).ok() || !builder.Append(serialized).ok())
            throw std::runtime_error("Failed to append Arrow DataValue union member");
    };
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue: append(0, *string_, value.stringvalue()); break;
    case dp::service::common::DataValue::kBooleanValue: append(1, *boolean_, value.booleanvalue()); break;
    case dp::service::common::DataValue::kUintValue: append(2, *uint32_, value.uintvalue()); break;
    case dp::service::common::DataValue::kUlongValue: append(3, *uint64_, value.ulongvalue()); break;
    case dp::service::common::DataValue::kIntValue: append(4, *int32_, value.intvalue()); break;
    case dp::service::common::DataValue::kLongValue: append(5, *int64_, value.longvalue()); break;
    case dp::service::common::DataValue::kFloatValue: append(6, *float_, value.floatvalue()); break;
    case dp::service::common::DataValue::kDoubleValue: append(7, *double_, value.doublevalue()); break;
    case dp::service::common::DataValue::kByteArrayValue: append(8, *binary_, value.bytearrayvalue()); break;
    case dp::service::common::DataValue::kTimestampValue: append(9, *timestamp_, timestampToNanoseconds(value.timestampvalue())); break;
    case dp::service::common::DataValue::kArrayValue: appendSerialized(10, *array_, value.arrayvalue()); break;
    case dp::service::common::DataValue::kStructureValue: appendSerialized(11, *structure_, value.structurevalue()); break;
    case dp::service::common::DataValue::kImageValue: appendSerialized(12, *image_, value.imagevalue()); break;
    case dp::service::common::DataValue::VALUE_NOT_SET:
        if (!union_builder_.AppendNull().ok())
            throw std::runtime_error("Failed to append null Arrow DataValue");
        break;
    }
}

std::shared_ptr<arrow::Array> DataValueBuilder::finish()
{
    std::shared_ptr<arrow::Array> array;
    if (!union_builder_.Finish(&array).ok())
        throw std::runtime_error("Failed to finish Arrow DataValue union");
    return array;
}

template <typename Builder, typename Value>
void DataValueBuilder::append(int8_t type_id, Builder& builder, const Value& value)
{
    if (!union_builder_.Append(type_id).ok() || !builder.Append(value).ok())
        throw std::runtime_error("Failed to append Arrow DataValue union member");
}
