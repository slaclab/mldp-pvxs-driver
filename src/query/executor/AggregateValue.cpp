//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/AggregateValue.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/builder.h>
#include <arrow/compute/api.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace mldp_pvxs_driver::query::executor;

namespace {

constexpr std::size_t kTypeCodes = 128;

int byteWidth(const arrow::Type::type id)
{
    switch (id)
    {
    case arrow::Type::INT8:
    case arrow::Type::UINT8: return 1;
    case arrow::Type::INT16:
    case arrow::Type::UINT16: return 2;
    case arrow::Type::INT32:
    case arrow::Type::UINT32:
    case arrow::Type::DATE32:
    case arrow::Type::TIME32:
    case arrow::Type::FLOAT: return 4;
    case arrow::Type::INT64:
    case arrow::Type::UINT64:
    case arrow::Type::DATE64:
    case arrow::Type::TIME64:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DURATION:
    case arrow::Type::DOUBLE: return 8;
    default: return 0;
    }
}

// Integer storage type of an integer-backed logical type, or null.
std::shared_ptr<arrow::DataType> integerStorage(const arrow::DataType& type)
{
    switch (type.id())
    {
    case arrow::Type::INT8: return arrow::int8();
    case arrow::Type::INT16: return arrow::int16();
    case arrow::Type::INT32:
    case arrow::Type::DATE32:
    case arrow::Type::TIME32: return arrow::int32();
    case arrow::Type::INT64:
    case arrow::Type::DATE64:
    case arrow::Type::TIME64:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DURATION: return arrow::int64();
    case arrow::Type::UINT8: return arrow::uint8();
    case arrow::Type::UINT16: return arrow::uint16();
    case arrow::Type::UINT32: return arrow::uint32();
    case arrow::Type::UINT64: return arrow::uint64();
    default: return nullptr;
    }
}

std::shared_ptr<arrow::Array> check(arrow::Result<std::shared_ptr<arrow::Array>> result, const char* what)
{
    if (!result.ok()) throw std::runtime_error(std::string("GROUP BY failed to ") + what + ": " + result.status().ToString());
    return *result;
}

void check(const arrow::Status& status, const char* what)
{
    if (!status.ok()) throw std::runtime_error(std::string("GROUP BY failed to ") + what + ": " + status.ToString());
}

template <typename Builder>
std::shared_ptr<arrow::Array> finish(Builder& builder)
{
    std::shared_ptr<arrow::Array> array;
    check(builder.Finish(&array), "finish output column");
    return array;
}

std::shared_ptr<arrow::Array> withType(const std::shared_ptr<arrow::Array>& array, const std::shared_ptr<arrow::DataType>& type)
{
    if (array->type()->Equals(*type)) return array;
    auto data = array->data()->Copy();
    data->type = type;
    return arrow::MakeArray(data);
}

std::shared_ptr<arrow::Array> castTo(const std::shared_ptr<arrow::Array>& array, const std::shared_ptr<arrow::DataType>& type)
{
    if (array->type()->Equals(*type)) return array;
    auto cast = arrow::compute::Cast(*array, type);
    return check(std::move(cast), "cast output column");
}

std::string valueText(const OwnedValue& value)
{
    switch (value.kind)
    {
    case ValueKind::INT: return std::to_string(value.integer);
    case ValueKind::DOUBLE:
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.17g", value.real);
            return buffer;
        }
    case ValueKind::BOOL: return value.boolean ? "true" : "false";
    case ValueKind::STRING: return value.text;
    case ValueKind::OTHER: return value.other ? value.other->ToString() : std::string{};
    case ValueKind::NUL: return {};
    }
    return {};
}

} // namespace

ColumnReader::ColumnReader(const arrow::Array& array) : array_(&array), id_(array.type_id())
{
    const auto& data = *array.data();
    if (id_ == arrow::Type::DENSE_UNION || id_ == arrow::Type::SPARSE_UNION)
    {
        dense_union_ = id_ == arrow::Type::DENSE_UNION;
        type_codes_ = data.GetValues<int8_t>(1);
        if (dense_union_) value_offsets_ = data.GetValues<int32_t>(2);
        const auto& union_array = static_cast<const arrow::UnionArray&>(array);
        const auto& union_type = static_cast<const arrow::UnionType&>(*array.type());
        union_children_.resize(kTypeCodes);
        union_readers_.resize(kTypeCodes);
        for (int child = 0; child < union_type.num_fields(); ++child)
        {
            const auto code = static_cast<std::size_t>(union_type.type_codes()[static_cast<std::size_t>(child)]);
            union_children_[code] = union_array.field(child);
            union_readers_[code] = std::make_unique<ColumnReader>(*union_children_[code]);
        }
        return;
    }
    if (const auto width = byteWidth(id_); width > 0 && data.buffers.size() > 1 && data.buffers[1])
        values_ = data.buffers[1]->data() + data.offset * width;
}

ColumnReader::~ColumnReader() = default;
ColumnReader::ColumnReader(ColumnReader&&) noexcept = default;
ColumnReader& ColumnReader::operator=(ColumnReader&&) noexcept = default;

ValueRef ColumnReader::at(const int64_t row) const
{
    if (type_codes_ != nullptr)
    {
        const auto code = static_cast<std::size_t>(type_codes_[row]);
        const auto& reader = union_readers_[code];
        if (!reader) return {};
        return reader->at(dense_union_ ? value_offsets_[row] : row);
    }
    if (array_->IsNull(row)) return {};
    ValueRef value;
    const auto integer = [&value](const int64_t v) { value.kind = ValueKind::INT; value.integer = v; return value; };
    switch (id_)
    {
    case arrow::Type::INT8: return integer(static_cast<const int8_t*>(values_)[row]);
    case arrow::Type::UINT8: return integer(static_cast<const uint8_t*>(values_)[row]);
    case arrow::Type::INT16: return integer(static_cast<const int16_t*>(values_)[row]);
    case arrow::Type::UINT16: return integer(static_cast<const uint16_t*>(values_)[row]);
    case arrow::Type::INT32:
    case arrow::Type::DATE32:
    case arrow::Type::TIME32: return integer(static_cast<const int32_t*>(values_)[row]);
    case arrow::Type::UINT32: return integer(static_cast<const uint32_t*>(values_)[row]);
    case arrow::Type::INT64:
    case arrow::Type::DATE64:
    case arrow::Type::TIME64:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DURATION: return integer(static_cast<const int64_t*>(values_)[row]);
    case arrow::Type::UINT64: return integer(static_cast<int64_t>(static_cast<const uint64_t*>(values_)[row]));
    case arrow::Type::FLOAT:
        value.kind = ValueKind::DOUBLE;
        value.real = static_cast<const float*>(values_)[row];
        return value;
    case arrow::Type::DOUBLE:
        value.kind = ValueKind::DOUBLE;
        value.real = static_cast<const double*>(values_)[row];
        return value;
    case arrow::Type::BOOL:
        value.kind = ValueKind::BOOL;
        value.boolean = static_cast<const arrow::BooleanArray&>(*array_).Value(row);
        return value;
    case arrow::Type::STRING:
    case arrow::Type::BINARY:
        value.kind = ValueKind::STRING;
        value.text = static_cast<const arrow::BinaryArray&>(*array_).GetView(row);
        return value;
    case arrow::Type::LARGE_STRING:
    case arrow::Type::LARGE_BINARY:
        value.kind = ValueKind::STRING;
        value.text = static_cast<const arrow::LargeBinaryArray&>(*array_).GetView(row);
        return value;
    default:
        value.kind = ValueKind::OTHER;
        value.array = array_;
        value.index = row;
        return value;
    }
}

OwnedValue OwnedValue::from(const ValueRef& value)
{
    OwnedValue owned;
    owned.kind = value.kind;
    owned.integer = value.integer;
    owned.real = value.real;
    owned.boolean = value.boolean;
    if (value.kind == ValueKind::STRING) owned.text.assign(value.text);
    if (value.kind == ValueKind::OTHER)
    {
        auto scalar = value.array->GetScalar(value.index);
        if (!scalar.ok()) throw std::runtime_error("GROUP BY failed to read value: " + scalar.status().ToString());
        owned.other = std::move(*scalar);
    }
    return owned;
}

int mldp_pvxs_driver::query::executor::compareValues(const ValueRef& lhs, const OwnedValue& rhs)
{
    if (lhs.kind == ValueKind::INT && rhs.kind == ValueKind::INT)
        return lhs.integer < rhs.integer ? -1 : lhs.integer > rhs.integer ? 1 : 0;
    const bool rhs_numeric = rhs.kind == ValueKind::INT || rhs.kind == ValueKind::DOUBLE;
    if (lhs.numeric() && rhs_numeric)
    {
        const auto left = lhs.asDouble();
        const auto right = rhs.kind == ValueKind::INT ? static_cast<double>(rhs.integer) : rhs.real;
        return left < right ? -1 : left > right ? 1 : 0;
    }
    if (lhs.kind == ValueKind::STRING && rhs.kind == ValueKind::STRING) return lhs.text.compare(rhs.text);
    if (lhs.kind == ValueKind::BOOL && rhs.kind == ValueKind::BOOL) return static_cast<int>(lhs.boolean) - static_cast<int>(rhs.boolean);
    throw std::runtime_error("MIN/MAX cannot compare values of different or non-orderable types");
}

void mldp_pvxs_driver::query::executor::appendValueKey(std::string& out, const ValueRef& value)
{
    const auto append_raw = [&out](const void* data, const std::size_t size) { out.append(static_cast<const char*>(data), size); };
    switch (value.kind)
    {
    case ValueKind::NUL: out.push_back('N'); return;
    case ValueKind::INT: out.push_back('I'); append_raw(&value.integer, sizeof(value.integer)); return;
    case ValueKind::DOUBLE:
        {
            // Treat -0.0 as 0.0 so equal numbers share a key.
            const double real = value.real == 0.0 ? 0.0 : value.real;
            out.push_back('D');
            append_raw(&real, sizeof(real));
            return;
        }
    case ValueKind::BOOL: out.push_back(value.boolean ? 'T' : 'F'); return;
    case ValueKind::STRING:
        {
            const auto size = static_cast<uint32_t>(value.text.size());
            out.push_back('S');
            append_raw(&size, sizeof(size));
            out.append(value.text);
            return;
        }
    case ValueKind::OTHER:
        {
            auto scalar = value.array->GetScalar(value.index);
            if (!scalar.ok()) throw std::runtime_error("GROUP BY failed to read value: " + scalar.status().ToString());
            const auto text = (*scalar)->ToString();
            const auto size = static_cast<uint32_t>(text.size());
            out.push_back('O');
            append_raw(&size, sizeof(size));
            out.append(text);
            return;
        }
    }
}

std::shared_ptr<arrow::DataType> mldp_pvxs_driver::query::executor::preservedType(const std::shared_ptr<arrow::DataType>& type)
{
    if (!type || type->id() == arrow::Type::NA || type->id() == arrow::Type::DENSE_UNION || type->id() == arrow::Type::SPARSE_UNION)
        return nullptr;
    return type;
}

std::shared_ptr<arrow::Array> mldp_pvxs_driver::query::executor::buildValueColumn(const std::vector<OwnedValue>& values,
                                                                                    const std::shared_ptr<arrow::DataType>& requested,
                                                                                    arrow::MemoryPool* pool)
{
    if (pool == nullptr) pool = arrow::default_memory_pool();
    auto type = requested;
    if (!type)
    {
        bool any = false, ints = false, reals = false, bools = false, strings = false, others = false;
        for (const auto& value : values)
        {
            any |= value.kind != ValueKind::NUL;
            ints |= value.kind == ValueKind::INT;
            reals |= value.kind == ValueKind::DOUBLE;
            bools |= value.kind == ValueKind::BOOL;
            strings |= value.kind == ValueKind::STRING;
            others |= value.kind == ValueKind::OTHER;
        }
        if (!any) return std::make_shared<arrow::NullArray>(static_cast<int64_t>(values.size()));
        const bool numbers_only = !bools && !strings && !others;
        if (numbers_only) type = reals ? arrow::float64() : arrow::int64();
        else if (bools && !ints && !reals && !strings && !others) type = arrow::boolean();
        else type = arrow::utf8();
        if (type->id() == arrow::Type::STRING && (ints || reals || bools || others))
        {
            // Mixed kinds: render every value as text.
            arrow::StringBuilder builder(pool);
            for (const auto& value : values)
                check(value.kind == ValueKind::NUL ? builder.AppendNull() : builder.Append(valueText(value)), "append value");
            return finish(builder);
        }
    }

    if (const auto storage = integerStorage(*type))
    {
        arrow::Int64Builder builder(pool);
        check(builder.Reserve(static_cast<int64_t>(values.size())), "reserve output column");
        for (const auto& value : values)
        {
            if (value.kind == ValueKind::INT) builder.UnsafeAppend(value.integer);
            else if (value.kind == ValueKind::DOUBLE) builder.UnsafeAppend(static_cast<int64_t>(value.real));
            else builder.UnsafeAppendNull();
        }
        return withType(castTo(finish(builder), storage), type);
    }
    switch (type->id())
    {
    case arrow::Type::FLOAT:
    case arrow::Type::DOUBLE:
        {
            arrow::DoubleBuilder builder(pool);
            check(builder.Reserve(static_cast<int64_t>(values.size())), "reserve output column");
            for (const auto& value : values)
            {
                if (value.kind == ValueKind::DOUBLE) builder.UnsafeAppend(value.real);
                else if (value.kind == ValueKind::INT) builder.UnsafeAppend(static_cast<double>(value.integer));
                else builder.UnsafeAppendNull();
            }
            return castTo(finish(builder), type);
        }
    case arrow::Type::BOOL:
        {
            arrow::BooleanBuilder builder(pool);
            for (const auto& value : values)
                check(value.kind == ValueKind::BOOL ? builder.Append(value.boolean) : builder.AppendNull(), "append value");
            return finish(builder);
        }
    case arrow::Type::STRING:
    case arrow::Type::BINARY:
    case arrow::Type::LARGE_STRING:
    case arrow::Type::LARGE_BINARY:
        {
            arrow::BinaryBuilder builder(pool);
            for (const auto& value : values)
                check(value.kind == ValueKind::STRING ? builder.Append(value.text) : builder.AppendNull(), "append value");
            auto array = finish(builder);
            if (type->id() == arrow::Type::BINARY) return array;
            if (type->id() == arrow::Type::STRING) return withType(array, type);
            return castTo(array, type);
        }
    default:
        {
            std::unique_ptr<arrow::ArrayBuilder> builder;
            check(arrow::MakeBuilder(pool, type, &builder), "create output builder");
            for (const auto& value : values)
                check(value.kind == ValueKind::OTHER && value.other ? builder->AppendScalar(*value.other) : builder->AppendNull(), "append value");
            std::shared_ptr<arrow::Array> array;
            check(builder->Finish(&array), "finish output column");
            return array;
        }
    }
}
