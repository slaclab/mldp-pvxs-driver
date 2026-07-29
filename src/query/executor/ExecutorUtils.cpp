//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryExecutor.h>
#include <query/ExpressionRegistry.h>
#include <query/Timezone.h>

#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>

#include <query/QueryResult.h>
#include <query/QueryableFactory.h>
#include <query/QueryTableCatalog.h>
#include <query/QueryPlanner.h>
#include <query/SpillManager.h>

#include <algorithm>
#include <cctype>
#include <arrow/array/data.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/builder.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>
#include <arrow/type.h>

#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <numeric>
#include <limits>

using namespace mldp_pvxs_driver::query;

namespace {

std::string joinOpsImpl(const std::set<PredicateOp>& ops)
{
    const auto name = [](const PredicateOp op) -> std::string_view
    {
        switch (op)
        {
            case PredicateOp::EQ: return "=";
            case PredicateOp::NEQ: return "!=";
            case PredicateOp::LT: return "<";
            case PredicateOp::LTE: return "<=";
            case PredicateOp::GT: return ">";
            case PredicateOp::GTE: return ">=";
            case PredicateOp::IN: return "IN";
            case PredicateOp::PREFIX: return "PREFIX";
            case PredicateOp::CONTAINS: return "CONTAINS";
            case PredicateOp::LIKE: return "LIKE";
            case PredicateOp::BETWEEN: return "BETWEEN";
            case PredicateOp::IS_NULL: return "IS NULL";
            case PredicateOp::IS_NOT_NULL: return "IS NOT NULL";
        }
        return "unknown";
    };
    std::ostringstream out;
    bool               first = true;
    for (const auto op : ops)
    {
        if (!first)
        {
            out << ",";
        }
        first = false;
        out << name(op);
    }
    return out.str();
}

std::string_view columnTypeNameImpl(const ColumnType type)
{
    switch (type)
    {
        case ColumnType::STRING: return "string";
        case ColumnType::TIMESTAMP: return "timestamp";
        case ColumnType::DURATION_SECONDS: return "duration_seconds";
        case ColumnType::INT: return "int";
        case ColumnType::NATIVE_VALUE: return "native_value";
        case ColumnType::BOOL: return "bool";
    }
    return "unknown";
}

ColumnType columnTypeFromArrowImpl(const std::shared_ptr<arrow::DataType>& type)
{
    switch (type->id())
    {
        case arrow::Type::INT64: return ColumnType::INT;
        case arrow::Type::BOOL: return ColumnType::BOOL;
        case arrow::Type::TIMESTAMP: return ColumnType::TIMESTAMP;
        case arrow::Type::DURATION: return ColumnType::DURATION_SECONDS;
        case arrow::Type::DENSE_UNION:
        case arrow::Type::SPARSE_UNION: return ColumnType::NATIVE_VALUE;
        default: return ColumnType::STRING;
    }
}

std::string scalarToString(const std::shared_ptr<arrow::Scalar>& scalar)
{
    if (!scalar || !scalar->is_valid)
    {
        return "";
    }
    return scalar->ToString();
}

int64_t timestampScalarValue(const std::shared_ptr<arrow::Scalar>& scalar, const std::string_view endpoint)
{
    if (!scalar || !scalar->is_valid || scalar->type->id() != arrow::Type::TIMESTAMP)
        throw std::runtime_error("MLDP time-series window subquery requires a non-null timestamp at " + std::string(endpoint));
    return std::dynamic_pointer_cast<arrow::TimestampScalar>(scalar)->value;
}

int64_t timestampToEpochSeconds(const std::shared_ptr<arrow::TimestampScalar>& scalar)
{
    const auto type = std::dynamic_pointer_cast<arrow::TimestampType>(scalar->type);
    switch (type->unit())
    {
        case arrow::TimeUnit::SECOND: return scalar->value;
        case arrow::TimeUnit::MILLI: return scalar->value / 1'000;
        case arrow::TimeUnit::MICRO: return scalar->value / 1'000'000;
        case arrow::TimeUnit::NANO: return scalar->value / 1'000'000'000;
    }
    throw std::runtime_error("Unsupported Arrow timestamp unit");
}

std::vector<ExecutableLiteralValue> extractInSubqueryValuesImpl(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
    const ColumnType target_type,
    const std::string_view target_column)
{
    std::vector<ExecutableLiteralValue> values;
    for (const auto& batch : batches)
    {
        if (batch->num_columns() != 1)
            throw std::runtime_error("IN (SELECT ...) for column '" + std::string(target_column) + "' must return exactly one column");
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            const auto scalar = batch->column(0)->GetScalar(row);
            if (!scalar.ok() || !(*scalar)->is_valid)
                throw std::runtime_error("IN (SELECT ...) for column '" + std::string(target_column) + "' returned a null value");
            switch (target_type)
            {
                case ColumnType::STRING:
                    if ((*scalar)->type->id() != arrow::Type::STRING)
                        throw std::runtime_error("IN (SELECT ...) for string column '" + std::string(target_column) + "' requires Arrow string output");
                    values.emplace_back(std::dynamic_pointer_cast<arrow::StringScalar>(*scalar)->ToString());
                    break;
                case ColumnType::BOOL:
                    if ((*scalar)->type->id() != arrow::Type::BOOL)
                        throw std::runtime_error("IN (SELECT ...) for bool column '" + std::string(target_column) + "' requires Arrow boolean output");
                    values.emplace_back(std::dynamic_pointer_cast<arrow::BooleanScalar>(*scalar)->value);
                    break;
                case ColumnType::INT:
                case ColumnType::DURATION_SECONDS:
                    switch ((*scalar)->type->id())
                    {
                        case arrow::Type::INT8:
                        case arrow::Type::INT16:
                        case arrow::Type::INT32:
                        case arrow::Type::INT64:
                        case arrow::Type::UINT8:
                        case arrow::Type::UINT16:
                        case arrow::Type::UINT32:
                        case arrow::Type::UINT64:
                            try
                            {
                                values.emplace_back(std::stoll((*scalar)->ToString()));
                            }
                            catch (const std::exception&)
                            {
                                throw std::runtime_error("IN (SELECT ...) value cannot be represented as int64 for column '" + std::string(target_column) + "'");
                            }
                            break;
                        default:
                            throw std::runtime_error("IN (SELECT ...) for integral column '" + std::string(target_column) + "' requires Arrow integral output");
                    }
                    break;
                case ColumnType::NATIVE_VALUE:
                    throw std::runtime_error("IN (SELECT ...) is not supported for native value columns");
                case ColumnType::TIMESTAMP:
                    if ((*scalar)->type->id() != arrow::Type::TIMESTAMP)
                        throw std::runtime_error("IN (SELECT ...) for timestamp column '" + std::string(target_column) + "' requires Arrow timestamp output");
                    values.emplace_back(timestampToEpochSeconds(std::dynamic_pointer_cast<arrow::TimestampScalar>(*scalar)));
                    break;
            }
        }
    }
    return values;
}

std::vector<std::pair<int64_t, int64_t>> extractNormalizedWindowsImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    std::vector<std::pair<int64_t, int64_t>> windows;
    for (const auto& batch : batches)
    {
        if (batch->num_columns() != 2 || batch->column(0)->type_id() != arrow::Type::TIMESTAMP || batch->column(1)->type_id() != arrow::Type::TIMESTAMP)
            throw std::runtime_error("MLDP time-series window subquery must return exactly two timestamp columns");
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            const auto time = batch->column(0)->GetScalar(row);
            const auto end = batch->column(1)->GetScalar(row);
            if (!time.ok() || !end.ok()) throw std::runtime_error("Failed to read MLDP time-series window subquery result");
            const auto begin_ns = timestampScalarValue(*time, "position 1");
            const auto end_ns = timestampScalarValue(*end, "position 2");
            if (end_ns < begin_ns)
                throw std::runtime_error("MLDP time-series window subquery returned an end timestamp before its start timestamp");
            windows.emplace_back(begin_ns, end_ns);
        }
    }
    std::sort(windows.begin(), windows.end());
    std::vector<std::pair<int64_t, int64_t>> normalized;
    for (const auto& window : windows)
    {
        if (normalized.empty() || window.first > normalized.back().second + (normalized.back().second != std::numeric_limits<int64_t>::max()))
            normalized.push_back(window);
        else
            normalized.back().second = std::max(normalized.back().second, window.second);
    }
    return normalized;
}

bool matchesLikePatternImpl(std::string_view value, std::string_view pattern)
{
    struct PatternToken {
        enum class Type { LITERAL, ANY, SINGLE };
        Type type;
        char value{};
    };

    std::vector<PatternToken> tokens;
    tokens.reserve(pattern.size());
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        const auto character = pattern[index];
        if (character == '\\' && index + 1 < pattern.size())
        {
            tokens.push_back({.type = PatternToken::Type::LITERAL, .value = pattern[++index]});
        }
        else if (character == '%' || character == '*')
        {
            tokens.push_back({.type = PatternToken::Type::ANY});
        }
        else if (character == '_')
        {
            tokens.push_back({.type = PatternToken::Type::SINGLE});
        }
        else
        {
            tokens.push_back({.type = PatternToken::Type::LITERAL, .value = character});
        }
    }

    const auto equalIgnoreCase = [](const char lhs, const char rhs)
    {
        return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
    };

    std::size_t value_index = 0;
    std::size_t token_index = 0;
    std::size_t wildcard_token = std::string::npos;
    std::size_t wildcard_value = 0;
    while (value_index < value.size())
    {
        if (token_index < tokens.size() &&
            (tokens[token_index].type == PatternToken::Type::SINGLE ||
             (tokens[token_index].type == PatternToken::Type::LITERAL && equalIgnoreCase(tokens[token_index].value, value[value_index]))))
        {
            ++token_index;
            ++value_index;
        }
        else if (token_index < tokens.size() && tokens[token_index].type == PatternToken::Type::ANY)
        {
            wildcard_token = token_index++;
            wildcard_value = value_index;
        }
        else if (wildcard_token != std::string::npos)
        {
            token_index = wildcard_token + 1;
            value_index = ++wildcard_value;
        }
        else
        {
            return false;
        }
    }
    while (token_index < tokens.size() && tokens[token_index].type == PatternToken::Type::ANY)
        ++token_index;
    return token_index == tokens.size();
}

struct NativeScalar
{
    enum class Kind
    {
        NULL_VALUE,
        STRING,
        BOOLEAN,
        SIGNED_INTEGER,
        UNSIGNED_INTEGER,
        FLOATING_POINT,
        TIMESTAMP,
        DURATION,
        UNSUPPORTED
    };

    Kind kind{Kind::UNSUPPORTED};
    std::variant<std::monostate, std::string, bool, int64_t, uint64_t, double> value;
};

NativeScalar nativeScalarFromArrow(std::shared_ptr<arrow::Scalar> scalar)
{
    while (scalar && scalar->is_valid &&
           (scalar->type->id() == arrow::Type::DENSE_UNION || scalar->type->id() == arrow::Type::SPARSE_UNION))
    {
        const auto union_scalar = std::dynamic_pointer_cast<arrow::UnionScalar>(scalar);
        if (!union_scalar)
        {
            return {};
        }
        scalar = union_scalar->child_value();
    }
    if (!scalar || !scalar->is_valid)
    {
        return {.kind = NativeScalar::Kind::NULL_VALUE};
    }

    switch (scalar->type->id())
    {
        case arrow::Type::STRING:
        {
            const auto value = std::dynamic_pointer_cast<arrow::StringScalar>(scalar)->value;
            return {.kind = NativeScalar::Kind::STRING,
                    .value = value ? std::string(reinterpret_cast<const char*>(value->data()), static_cast<std::size_t>(value->size())) : std::string{}};
        }
        case arrow::Type::BOOL: return {.kind = NativeScalar::Kind::BOOLEAN, .value = std::dynamic_pointer_cast<arrow::BooleanScalar>(scalar)->value};
        case arrow::Type::INT8: return {.kind = NativeScalar::Kind::SIGNED_INTEGER, .value = static_cast<int64_t>(std::dynamic_pointer_cast<arrow::Int8Scalar>(scalar)->value)};
        case arrow::Type::INT16: return {.kind = NativeScalar::Kind::SIGNED_INTEGER, .value = static_cast<int64_t>(std::dynamic_pointer_cast<arrow::Int16Scalar>(scalar)->value)};
        case arrow::Type::INT32: return {.kind = NativeScalar::Kind::SIGNED_INTEGER, .value = static_cast<int64_t>(std::dynamic_pointer_cast<arrow::Int32Scalar>(scalar)->value)};
        case arrow::Type::INT64: return {.kind = NativeScalar::Kind::SIGNED_INTEGER, .value = std::dynamic_pointer_cast<arrow::Int64Scalar>(scalar)->value};
        case arrow::Type::UINT8: return {.kind = NativeScalar::Kind::UNSIGNED_INTEGER, .value = static_cast<uint64_t>(std::dynamic_pointer_cast<arrow::UInt8Scalar>(scalar)->value)};
        case arrow::Type::UINT16: return {.kind = NativeScalar::Kind::UNSIGNED_INTEGER, .value = static_cast<uint64_t>(std::dynamic_pointer_cast<arrow::UInt16Scalar>(scalar)->value)};
        case arrow::Type::UINT32: return {.kind = NativeScalar::Kind::UNSIGNED_INTEGER, .value = static_cast<uint64_t>(std::dynamic_pointer_cast<arrow::UInt32Scalar>(scalar)->value)};
        case arrow::Type::UINT64: return {.kind = NativeScalar::Kind::UNSIGNED_INTEGER, .value = std::dynamic_pointer_cast<arrow::UInt64Scalar>(scalar)->value};
        case arrow::Type::FLOAT: return {.kind = NativeScalar::Kind::FLOATING_POINT, .value = static_cast<double>(std::dynamic_pointer_cast<arrow::FloatScalar>(scalar)->value)};
        case arrow::Type::DOUBLE: return {.kind = NativeScalar::Kind::FLOATING_POINT, .value = std::dynamic_pointer_cast<arrow::DoubleScalar>(scalar)->value};
        case arrow::Type::TIMESTAMP: return {.kind = NativeScalar::Kind::TIMESTAMP, .value = std::dynamic_pointer_cast<arrow::TimestampScalar>(scalar)->value};
        case arrow::Type::DURATION: return {.kind = NativeScalar::Kind::DURATION, .value = std::dynamic_pointer_cast<arrow::DurationScalar>(scalar)->value};
        default: return {};
    }
}

NativeScalar nativeScalarFromLiteral(const ExecutableLiteralValue& literal)
{
    if (std::holds_alternative<std::string>(literal)) return {.kind = NativeScalar::Kind::STRING, .value = std::get<std::string>(literal)};
    if (std::holds_alternative<int64_t>(literal)) return {.kind = NativeScalar::Kind::SIGNED_INTEGER, .value = std::get<int64_t>(literal)};
    if (std::holds_alternative<double>(literal)) return {.kind = NativeScalar::Kind::FLOATING_POINT, .value = std::get<double>(literal)};
    if (std::holds_alternative<bool>(literal)) return {.kind = NativeScalar::Kind::BOOLEAN, .value = std::get<bool>(literal)};
    if (std::holds_alternative<TimestampNsLiteral>(literal)) return {.kind = NativeScalar::Kind::TIMESTAMP, .value = std::get<TimestampNsLiteral>(literal).value};
    return {.kind = NativeScalar::Kind::DURATION, .value = std::get<DurationNsLiteral>(literal).value};
}

bool scalarMatchesPredicate(const std::shared_ptr<arrow::Scalar>& scalar, const Predicate& predicate)
{
    if (predicate.op == PredicateOp::IS_NULL)
    {
        return !scalar || !scalar->is_valid;
    }
    if (predicate.op == PredicateOp::IS_NOT_NULL)
    {
        return scalar && scalar->is_valid;
    }
    if (!scalar || !scalar->is_valid)
    {
        return false;
    }

    const auto native_value = nativeScalarFromArrow(scalar);
    if (native_value.kind == NativeScalar::Kind::NULL_VALUE || native_value.kind == NativeScalar::Kind::UNSUPPORTED) return false;

    const auto compareNumeric = [](const double lhs, const double rhs, const PredicateOp op)
    {
        if (op == PredicateOp::EQ) return lhs == rhs;
        if (op == PredicateOp::NEQ) return lhs != rhs;
        if (op == PredicateOp::LT) return lhs < rhs;
        if (op == PredicateOp::LTE) return lhs <= rhs;
        if (op == PredicateOp::GT) return lhs > rhs;
        if (op == PredicateOp::GTE) return lhs >= rhs;
        return false;
    };

    const auto compareSingle = [&](const ExecutableLiteralValue& literal, const PredicateOp op)
    {
        const auto native_literal = nativeScalarFromLiteral(literal);
        const auto numeric_kind = [](const NativeScalar::Kind kind)
        {
            return kind == NativeScalar::Kind::SIGNED_INTEGER || kind == NativeScalar::Kind::UNSIGNED_INTEGER || kind == NativeScalar::Kind::FLOATING_POINT;
        };
        if (native_value.kind == NativeScalar::Kind::TIMESTAMP && native_literal.kind == NativeScalar::Kind::SIGNED_INTEGER)
        {
            const auto lhs = std::get<int64_t>(native_value.value) / 1'000'000'000LL;
            const auto rhs = std::get<int64_t>(native_literal.value);
            return compareNumeric(lhs, rhs, op);
        }
        if (native_value.kind != native_literal.kind && !(numeric_kind(native_value.kind) && numeric_kind(native_literal.kind)))
        {
            return false;
        }
        if (native_value.kind == NativeScalar::Kind::STRING)
        {
            const auto& lhs = std::get<std::string>(native_value.value);
            const auto& rhs = std::get<std::string>(native_literal.value);
            if (op == PredicateOp::EQ)
                return lhs == rhs;
            if (op == PredicateOp::NEQ)
                return lhs != rhs;
            if (op == PredicateOp::PREFIX)
                return lhs.rfind(rhs, 0) == 0;
            if (op == PredicateOp::CONTAINS)
                return lhs.find(rhs) != std::string::npos;
            if (op == PredicateOp::LIKE)
                return matchesLikePatternImpl(lhs, rhs);
            if (op == PredicateOp::LT)
                return lhs < rhs;
            if (op == PredicateOp::LTE)
                return lhs <= rhs;
            if (op == PredicateOp::GT)
                return lhs > rhs;
            if (op == PredicateOp::GTE)
                return lhs >= rhs;
            return false;
        }
        if (numeric_kind(native_value.kind))
        {
            const auto as_double = [](const NativeScalar& value)
            {
                if (value.kind == NativeScalar::Kind::SIGNED_INTEGER) return static_cast<double>(std::get<int64_t>(value.value));
                if (value.kind == NativeScalar::Kind::UNSIGNED_INTEGER) return static_cast<double>(std::get<uint64_t>(value.value));
                return std::get<double>(value.value);
            };
            return compareNumeric(as_double(native_value), as_double(native_literal), op);
        }
        if (native_value.kind == NativeScalar::Kind::TIMESTAMP || native_value.kind == NativeScalar::Kind::DURATION)
        {
            const auto lhs = std::get<int64_t>(native_value.value);
            const auto rhs = std::get<int64_t>(native_literal.value);
            return compareNumeric(lhs, rhs, op);
        }
        if (native_value.kind != NativeScalar::Kind::BOOLEAN)
        {
            return false;
        }
        const auto lhs = std::get<bool>(native_value.value);
        const auto rhs = std::get<bool>(native_literal.value);
        if (op == PredicateOp::EQ)
            return lhs == rhs;
        if (op == PredicateOp::NEQ)
            return lhs != rhs;
        return false;
    };

    if (predicate.op == PredicateOp::IN)
    {
        for (const auto& value : predicate.values)
        {
            if (compareSingle(value, PredicateOp::EQ))
            {
                return true;
            }
        }
        return false;
    }

    if (predicate.op == PredicateOp::BETWEEN)
    {
        if (predicate.values.size() != 2 ||
            (!std::holds_alternative<int64_t>(predicate.values[0]) && !std::holds_alternative<double>(predicate.values[0]) && !std::holds_alternative<TimestampNsLiteral>(predicate.values[0]) && !std::holds_alternative<DurationNsLiteral>(predicate.values[0])) ||
            (!std::holds_alternative<int64_t>(predicate.values[1]) && !std::holds_alternative<double>(predicate.values[1]) && !std::holds_alternative<TimestampNsLiteral>(predicate.values[1]) && !std::holds_alternative<DurationNsLiteral>(predicate.values[1])))
        {
            return false;
        }
        if (std::holds_alternative<TimestampNsLiteral>(predicate.values[0]) || std::holds_alternative<DurationNsLiteral>(predicate.values[0]) ||
            std::holds_alternative<TimestampNsLiteral>(predicate.values[1]) || std::holds_alternative<DurationNsLiteral>(predicate.values[1]))
        {
            if (native_value.kind == NativeScalar::Kind::TIMESTAMP && std::holds_alternative<TimestampNsLiteral>(predicate.values[0]) && std::holds_alternative<TimestampNsLiteral>(predicate.values[1]))
                return std::get<int64_t>(native_value.value) >= std::get<TimestampNsLiteral>(predicate.values[0]).value && std::get<int64_t>(native_value.value) <= std::get<TimestampNsLiteral>(predicate.values[1]).value;
            if (native_value.kind == NativeScalar::Kind::DURATION && std::holds_alternative<DurationNsLiteral>(predicate.values[0]) && std::holds_alternative<DurationNsLiteral>(predicate.values[1]))
                return std::get<int64_t>(native_value.value) >= std::get<DurationNsLiteral>(predicate.values[0]).value && std::get<int64_t>(native_value.value) <= std::get<DurationNsLiteral>(predicate.values[1]).value;
            return false;
        }
        if (native_value.kind != NativeScalar::Kind::SIGNED_INTEGER && native_value.kind != NativeScalar::Kind::UNSIGNED_INTEGER && native_value.kind != NativeScalar::Kind::FLOATING_POINT) return false;
        const auto numeric = [](const auto& value) { return std::holds_alternative<int64_t>(value) ? static_cast<double>(std::get<int64_t>(value)) : std::get<double>(value); };
        const auto lo = numeric(predicate.values[0]);
        const auto hi = numeric(predicate.values[1]);
        const auto value = native_value.kind == NativeScalar::Kind::SIGNED_INTEGER ? static_cast<double>(std::get<int64_t>(native_value.value)) :
                           native_value.kind == NativeScalar::Kind::UNSIGNED_INTEGER ? static_cast<double>(std::get<uint64_t>(native_value.value)) :
                                                                                       std::get<double>(native_value.value);
        return value >= lo && value <= hi;
    }

    if (predicate.values.empty())
    {
        return false;
    }
    return compareSingle(predicate.values.front(), predicate.op);
}

bool listContainsPredicateValue(const std::shared_ptr<arrow::Scalar>& scalar, const Predicate& predicate)
{
    const auto list = std::dynamic_pointer_cast<arrow::ListScalar>(scalar);
    if (!list || !list->is_valid || !list->value)
    {
        return false;
    }
    for (int64_t index = 0; index < list->value->length(); ++index)
    {
        const auto value = list->value->GetScalar(index);
        if (!value.ok())
        {
            throw std::runtime_error(value.status().ToString());
        }
        if (scalarMatchesPredicate(*value, Predicate{.column = "tag", .op = predicate.op, .values = predicate.values}))
        {
            return true;
        }
    }
    return false;
}

bool hasUnionColumn(const std::shared_ptr<arrow::RecordBatch>& batch)
{
    return std::any_of(batch->columns().begin(), batch->columns().end(), [](const std::shared_ptr<arrow::Array>& column)
    {
        return column->type_id() == arrow::Type::DENSE_UNION || column->type_id() == arrow::Type::SPARSE_UNION;
    });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> selectUnionSafeRows(
    const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<int64_t>& selected_rows)
{
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(batch->num_columns());
    for (const auto& source : batch->columns())
    {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(), source->type(), &builder));
        const arrow::ArraySpan source_span(*source->data());
        for (std::size_t begin = 0; begin < selected_rows.size();)
        {
            const auto first_row = selected_rows[begin];
            std::size_t end = begin + 1;
            while (end < selected_rows.size() && selected_rows[end] == selected_rows[end - 1] + 1)
            {
                ++end;
            }
            RETURN_NOT_OK(builder->AppendArraySlice(source_span, first_row, static_cast<int64_t>(end - begin)));
            begin = end;
        }
        std::shared_ptr<arrow::Array> column;
        RETURN_NOT_OK(builder->Finish(&column));
        columns.push_back(std::move(column));
    }
    return arrow::RecordBatch::Make(batch->schema(), static_cast<int64_t>(selected_rows.size()), std::move(columns));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> applyFilterImpl(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                               const std::vector<Predicate>&              predicates)
{
    arrow::BooleanBuilder mask_builder;
    std::vector<int64_t> selected_rows;
    for (int64_t row = 0; row < batch->num_rows(); ++row)
    {
        bool include = true;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "tag")
            {
                int tags_index = batch->schema()->GetFieldIndex("tags");
                if (tags_index < 0)
                {
                    for (int fi = 0; fi < batch->schema()->num_fields(); ++fi)
                    {
                        const auto& name = batch->schema()->field(fi)->name();
                        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".tags") == 0)
                        {
                            tags_index = fi;
                            break;
                        }
                    }
                }
                if (tags_index < 0)
                {
                    include = false;
                    break;
                }
                ARROW_ASSIGN_OR_RAISE(auto tags, batch->column(tags_index)->GetScalar(row));
                if (!listContainsPredicateValue(tags, predicate))
                {
                    include = false;
                    break;
                }
                continue;
            }
            int field_index = batch->schema()->GetFieldIndex(predicate.column);
            if (field_index < 0)
            {
                const auto suffix = "." + predicate.column;
                for (int fi = 0; fi < batch->schema()->num_fields(); ++fi)
                {
                    const auto& name = batch->schema()->field(fi)->name();
                    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                    {
                        field_index = fi;
                        break;
                    }
                }
            }
            if (field_index < 0)
            {
                include = false;
                break;
            }
            ARROW_ASSIGN_OR_RAISE(auto scalar, batch->column(field_index)->GetScalar(row));
            if (!scalarMatchesPredicate(scalar, predicate))
            {
                include = false;
                break;
            }
        }
        RETURN_NOT_OK(mask_builder.Append(include));
        if (include)
        {
            selected_rows.push_back(row);
        }
    }

    if (hasUnionColumn(batch))
    {
        return selectUnionSafeRows(batch, selected_rows);
    }

    std::shared_ptr<arrow::Array> mask;
    RETURN_NOT_OK(mask_builder.Finish(&mask));
    ARROW_ASSIGN_OR_RAISE(auto filtered, arrow::compute::Filter(batch, mask));
    return filtered.record_batch();
}

std::vector<std::shared_ptr<arrow::RecordBatch>>
applyProjectionImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input,
                const std::vector<std::string>&                         columns)
{
    if (columns.empty())
    {
        return input;
    }

    std::vector<std::shared_ptr<arrow::RecordBatch>> output;
    output.reserve(input.size());
    for (const auto& batch : input)
    {
        std::vector<std::shared_ptr<arrow::Array>> projected_arrays;
        std::vector<std::shared_ptr<arrow::Field>> projected_fields;
        projected_arrays.reserve(columns.size());
        projected_fields.reserve(columns.size());
        for (const auto& column : columns)
        {
            const auto field_index = batch->schema()->GetFieldIndex(column);
            if (field_index < 0)
            {
                throw std::runtime_error("Projection references unknown column: " + column);
            }
            projected_arrays.push_back(batch->column(field_index));
            projected_fields.push_back(batch->schema()->field(field_index));
        }
        output.push_back(arrow::RecordBatch::Make(
            arrow::schema(std::move(projected_fields)),
            batch->num_rows(),
            std::move(projected_arrays)));
    }
    return output;
}

std::shared_ptr<arrow::Scalar> evaluateExpression(const ExpressionPtr& expression, const std::shared_ptr<arrow::RecordBatch>& batch, const int64_t row)
{
    if (!expression) throw std::runtime_error("Missing expression");
    return std::visit([&](const auto& value) -> std::shared_ptr<arrow::Scalar>
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, QualifiedColumn>)
        {
            const auto index = batch->schema()->GetFieldIndex(value.name);
            if (index < 0) throw std::runtime_error("Expression references unknown column: " + value.name);
            auto scalar = batch->column(index)->GetScalar(row);
            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
            return *scalar;
        }
        else if constexpr (std::is_same_v<T, LiteralValue>)
        {
            if (std::holds_alternative<int64_t>(value)) return std::make_shared<arrow::Int64Scalar>(std::get<int64_t>(value));
            if (std::holds_alternative<bool>(value)) return std::make_shared<arrow::BooleanScalar>(std::get<bool>(value));
            if (std::holds_alternative<std::string>(value)) return std::make_shared<arrow::StringScalar>(std::get<std::string>(value));
            if (std::holds_alternative<TimestampNsLiteral>(value)) return std::make_shared<arrow::TimestampScalar>(std::get<TimestampNsLiteral>(value).value, arrow::timestamp(arrow::TimeUnit::NANO));
            if (std::holds_alternative<DurationNsLiteral>(value)) return std::make_shared<arrow::DurationScalar>(std::get<DurationNsLiteral>(value).value, arrow::duration(arrow::TimeUnit::NANO));
            throw std::runtime_error("Unsupported literal in expression evaluator");
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>)
        {
            const auto operand = evaluateExpression(value.operand, batch, row);
            if (!operand->is_valid) return std::make_shared<arrow::NullScalar>();
            if (value.operator_name == "NOT") return std::make_shared<arrow::BooleanScalar>(!std::static_pointer_cast<arrow::BooleanScalar>(operand)->value);
            const auto integer = std::static_pointer_cast<arrow::Int64Scalar>(operand)->value;
            return std::make_shared<arrow::Int64Scalar>(value.operator_name == "-" ? -integer : integer);
        }
        else if constexpr (std::is_same_v<T, BinaryExpression>)
        {
            const auto left = evaluateExpression(value.left, batch, row);
            const auto right = evaluateExpression(value.right, batch, row);
            if (!left->is_valid || !right->is_valid) return std::make_shared<arrow::NullScalar>();
            if (left->type->id() == arrow::Type::BOOL && right->type->id() == arrow::Type::BOOL)
            {
                const auto lhs = std::static_pointer_cast<arrow::BooleanScalar>(left)->value;
                const auto rhs = std::static_pointer_cast<arrow::BooleanScalar>(right)->value;
                if (value.operator_name == "AND") return std::make_shared<arrow::BooleanScalar>(lhs && rhs);
                if (value.operator_name == "OR") return std::make_shared<arrow::BooleanScalar>(lhs || rhs);
            }
            const auto integerValue = [&](const std::shared_ptr<arrow::Scalar>& scalar) -> int64_t
            {
                if (scalar->type->id() == arrow::Type::INT64) return std::static_pointer_cast<arrow::Int64Scalar>(scalar)->value;
                if (scalar->type->id() == arrow::Type::TIMESTAMP) return std::static_pointer_cast<arrow::TimestampScalar>(scalar)->value;
                if (scalar->type->id() == arrow::Type::DURATION) return std::static_pointer_cast<arrow::DurationScalar>(scalar)->value;
                throw std::runtime_error("Expression operator " + value.operator_name + " requires numeric or temporal operands");
            };
            const auto lhs = integerValue(left);
            const auto rhs = integerValue(right);
            const auto temporalResult = [&](const int64_t result) -> std::shared_ptr<arrow::Scalar>
            {
                if (left->type->id() == arrow::Type::TIMESTAMP && right->type->id() == arrow::Type::DURATION && (value.operator_name == "+" || value.operator_name == "-"))
                    return std::make_shared<arrow::TimestampScalar>(result, left->type);
                if (left->type->id() == arrow::Type::TIMESTAMP && right->type->id() == arrow::Type::TIMESTAMP && value.operator_name == "-")
                    return std::make_shared<arrow::DurationScalar>(result, arrow::duration(arrow::TimeUnit::NANO));
                return std::make_shared<arrow::Int64Scalar>(result);
            };
            if (value.operator_name == "+") return temporalResult(lhs + rhs);
            if (value.operator_name == "-") return temporalResult(lhs - rhs);
            if (value.operator_name == "*") return std::make_shared<arrow::Int64Scalar>(lhs * rhs);
            if (value.operator_name == "/") { if (rhs == 0) throw std::runtime_error("/ divide by zero"); return std::make_shared<arrow::Int64Scalar>(lhs / rhs); }
            if (value.operator_name == "=") return std::make_shared<arrow::BooleanScalar>(lhs == rhs);
            if (value.operator_name == "!=") return std::make_shared<arrow::BooleanScalar>(lhs != rhs);
            if (value.operator_name == "<") return std::make_shared<arrow::BooleanScalar>(lhs < rhs);
            if (value.operator_name == "<=") return std::make_shared<arrow::BooleanScalar>(lhs <= rhs);
            if (value.operator_name == ">") return std::make_shared<arrow::BooleanScalar>(lhs > rhs);
            if (value.operator_name == ">=") return std::make_shared<arrow::BooleanScalar>(lhs >= rhs);
            throw std::runtime_error("Unsupported expression operator: " + value.operator_name);
        }
        else if constexpr (std::is_same_v<T, FunctionCall>)
        {
            std::string name = value.name;
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            if (name == "from_utc")
            {
                if (value.arguments.size() != 2) throw std::runtime_error("from_utc has no matching overload");
                const auto timestamp = evaluateExpression(value.arguments[0], batch, row);
                const auto zone = evaluateExpression(value.arguments[1], batch, row);
                if (!timestamp->is_valid || !zone->is_valid) return std::make_shared<arrow::StringScalar>();
                if (timestamp->type->id() != arrow::Type::TIMESTAMP || zone->type->id() != arrow::Type::STRING)
                    throw std::runtime_error("from_utc has no matching overload");
                return std::make_shared<arrow::StringScalar>(fromUtc(*std::static_pointer_cast<arrow::TimestampScalar>(timestamp), std::static_pointer_cast<arrow::StringScalar>(zone)->value->ToString()));
            }
            throw std::runtime_error("Function evaluation is not implemented: " + value.name);
        }
    }, expression->value);
}

std::vector<std::shared_ptr<arrow::RecordBatch>> applyExpressionProjectionImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input,
                                                                                 const std::vector<ExpressionPtr>& expressions,
                                                                                 const std::vector<std::string>& names)
{
    std::vector<std::shared_ptr<arrow::RecordBatch>> output;
    output.reserve(input.size());
    for (const auto& batch : input)
    {
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        std::vector<std::shared_ptr<arrow::Field>> fields;
        for (size_t index = 0; index < expressions.size(); ++index)
        {
            if (const auto* column = std::get_if<QualifiedColumn>(&expressions[index]->value))
            {
                const auto field_index = batch->schema()->GetFieldIndex(column->name);
                if (field_index < 0)
                {
                    throw std::runtime_error("Expression references unknown column: " + column->name);
                }
                arrays.push_back(batch->column(field_index));
                fields.push_back(batch->schema()->field(field_index)->WithName(names.at(index)));
                continue;
            }
            std::shared_ptr<arrow::DataType> type;
            for (int64_t row = 0; row < batch->num_rows() && !type; ++row)
            {
                const auto scalar = evaluateExpression(expressions[index], batch, row);
                if (scalar->is_valid) type = scalar->type;
            }
            if (!type) type = arrow::null();
            std::unique_ptr<arrow::ArrayBuilder> builder;
            const auto builder_status = arrow::MakeBuilder(arrow::default_memory_pool(), type, &builder);
            if (!builder_status.ok()) throw std::runtime_error(builder_status.ToString());
            for (int64_t row = 0; row < batch->num_rows(); ++row)
            {
                const auto scalar = evaluateExpression(expressions[index], batch, row);
                const auto append_status = builder->AppendScalar(*scalar);
                if (!append_status.ok()) throw std::runtime_error(append_status.ToString());
            }
            std::shared_ptr<arrow::Array> array;
            const auto finish_status = builder->Finish(&array);
            if (!finish_status.ok()) throw std::runtime_error(finish_status.ToString());
            arrays.push_back(std::move(array));
            fields.push_back(arrow::field(names.at(index), type));
        }
        output.push_back(arrow::RecordBatch::Make(arrow::schema(std::move(fields)), batch->num_rows(), std::move(arrays)));
    }
    return output;
}

std::vector<std::shared_ptr<arrow::RecordBatch>>
applyLimitImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input, const uint64_t limit)
{
    std::vector<std::shared_ptr<arrow::RecordBatch>> output;
    output.reserve(input.size());
    uint64_t remaining = limit;
    for (const auto& batch : input)
    {
        if (remaining == 0)
        {
            break;
        }
        const auto rows = static_cast<uint64_t>(batch->num_rows());
        if (rows <= remaining)
        {
            output.push_back(batch);
            remaining -= rows;
            continue;
        }

        output.push_back(batch->Slice(0, static_cast<int64_t>(remaining)));
        remaining = 0;
    }
    return output;
}

std::shared_ptr<arrow::RecordBatch> combineBatchesImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

std::vector<std::shared_ptr<arrow::RecordBatch>>
applySortImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input, const std::vector<plan::SortKey>& keys)
{
    if (input.empty() || keys.empty())
    {
        return input;
    }
    const auto batch = combineBatchesImpl(input);
    if (!batch)
    {
        return {};
    }

    std::vector<int> indices;
    indices.reserve(keys.size());
    for (const auto& key : keys)
    {
        int index = batch->schema()->GetFieldIndex(key.column);
        if (index < 0)
        {
            const auto suffix = "." + key.column;
            for (int fi = 0; fi < batch->schema()->num_fields(); ++fi)
            {
                const auto& name = batch->schema()->field(fi)->name();
                if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    index = fi;
                    break;
                }
            }
        }
        if (index < 0)
        {
            throw std::runtime_error("ORDER BY references unknown column: " + key.column);
        }
        indices.push_back(index);
    }

    std::vector<int64_t> rows(static_cast<std::size_t>(batch->num_rows()));
    std::iota(rows.begin(), rows.end(), 0);
    std::stable_sort(rows.begin(), rows.end(), [&](const int64_t lhs_row, const int64_t rhs_row)
    {
        for (std::size_t key_index = 0; key_index < keys.size(); ++key_index)
        {
            const auto lhs = batch->column(indices[key_index])->GetScalar(lhs_row);
            const auto rhs = batch->column(indices[key_index])->GetScalar(rhs_row);
            if (!lhs.ok() || !rhs.ok())
            {
                throw std::runtime_error("Failed to read ORDER BY value");
            }
            const bool lhs_null = !*lhs || !(*lhs)->is_valid;
            const bool rhs_null = !*rhs || !(*rhs)->is_valid;
            if (lhs_null || rhs_null)
            {
                if (lhs_null != rhs_null) return !lhs_null;
                continue;
            }
            const auto lhs_value = scalarToString(*lhs);
            const auto rhs_value = scalarToString(*rhs);
            if (lhs_value == rhs_value) continue;
            return keys[key_index].descending ? lhs_value > rhs_value : lhs_value < rhs_value;
        }
        return false;
    });

    arrow::Int64Builder index_builder;
    if (!index_builder.AppendValues(rows).ok())
    {
        throw std::runtime_error("Failed to build ORDER BY row indices");
    }
    std::shared_ptr<arrow::Array> index_array;
    if (!index_builder.Finish(&index_array).ok())
    {
        throw std::runtime_error("Failed to finish ORDER BY row indices");
    }
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(batch->num_columns());
    for (const auto& column : batch->columns())
    {
        const auto taken = arrow::compute::Take(column, index_array);
        if (!taken.ok()) throw std::runtime_error(taken.status().ToString());
        arrays.push_back(taken->make_array());
    }
    return {arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(arrays))};
}

std::shared_ptr<arrow::RecordBatch> combineBatchesImpl(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    if (batches.empty())
    {
        return nullptr;
    }
    if (batches.size() == 1)
    {
        return batches.front();
    }

    auto table_result = arrow::Table::FromRecordBatches(batches);
    if (!table_result.ok())
    {
        throw std::runtime_error(table_result.status().ToString());
    }
    auto combined_result = (*table_result)->CombineChunks();
    if (!combined_result.ok())
    {
        throw std::runtime_error(combined_result.status().ToString());
    }
    auto rows = (*combined_result)->num_rows();
    auto batch_result = (*combined_result)->CombineChunksToBatch();
    if (!batch_result.ok())
    {
        throw std::runtime_error(batch_result.status().ToString());
    }
    if ((*batch_result)->num_rows() != rows)
    {
        throw std::runtime_error("Failed to combine record batches");
    }
    return *batch_result;
}

std::string scalarKey(const std::shared_ptr<arrow::Scalar>& scalar)
{
    if (!scalar || !scalar->is_valid)
    {
        return "";
    }
    return scalar->ToString();
}

std::shared_ptr<arrow::Array> buildArrayFromIndices(const std::shared_ptr<arrow::Array>& source,
                                                    const std::vector<int64_t>&          indices)
{
    std::unique_ptr<arrow::ArrayBuilder> builder;
    auto                                 status = arrow::MakeBuilder(arrow::default_memory_pool(), source->type(), &builder);
    if (!status.ok())
    {
        throw std::runtime_error(status.ToString());
    }
    for (const auto index : indices)
    {
        if (index < 0)
        {
            status = builder->AppendNull();
        }
        else
        {
            auto scalar_result = source->GetScalar(index);
            if (!scalar_result.ok())
            {
                throw std::runtime_error(scalar_result.status().ToString());
            }
            status = builder->AppendScalar(*(*scalar_result));
        }
        if (!status.ok())
        {
            throw std::runtime_error(status.ToString());
        }
    }
    std::shared_ptr<arrow::Array> out;
    status = builder->Finish(&out);
    if (!status.ok())
    {
        throw std::runtime_error(status.ToString());
    }
    return out;
}

std::shared_ptr<arrow::RecordBatch> qualifyBatchColumnsImpl(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                        const std::string&                         alias)
{
    if (!batch)
    {
        return batch;
    }

    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(static_cast<size_t>(batch->num_columns()));
    for (const auto& field : batch->schema()->fields())
    {
        fields.push_back(field->WithName(alias + "." + field->name()));
    }
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), batch->num_rows(), batch->columns());
}

std::shared_ptr<arrow::RecordBatch> joinBatchesImpl(const std::shared_ptr<arrow::RecordBatch>& left,
                                                const std::shared_ptr<arrow::RecordBatch>& right,
                                                const std::string&                         left_key,
                                                const std::string&                         right_key,
                                                const plan::JoinType                       type,
                                                const ExecutionContext&                    context,
                                                QueryStats&                                stats)
{
    const auto emptyLeft = left == nullptr || left->num_rows() == 0;
    const auto emptyRight = right == nullptr || right->num_rows() == 0;
    if (emptyLeft && (type == plan::JoinType::LEFT_OUTER || emptyRight))
    {
        if (!left)
        {
            return nullptr;
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(static_cast<size_t>(left->num_columns() + (right ? right->num_columns() : 0)));
        for (int index = 0; index < left->num_columns(); ++index)
        {
            arrays.push_back(left->column(index));
        }
        std::vector<std::shared_ptr<arrow::Field>> fields = left->schema()->fields();
        if (right)
        {
            for (int index = 0; index < right->num_columns(); ++index)
            {
                fields.push_back(right->schema()->field(index)->WithNullable(true));
                arrays.push_back(buildArrayFromIndices(right->column(index), std::vector<int64_t>(left->num_rows(), -1)));
            }
        }
        return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), left->num_rows(), std::move(arrays));
    }
    if (emptyLeft || emptyRight)
    {
        return arrow::RecordBatch::Make(
            arrow::schema(std::vector<std::shared_ptr<arrow::Field>>{}),
            0,
            std::vector<std::shared_ptr<arrow::Array>>{});
    }

    const int left_key_index = left->schema()->GetFieldIndex(left_key);
    const int right_key_index = right->schema()->GetFieldIndex(right_key);
    if (left_key_index < 0 || right_key_index < 0)
    {
        throw std::runtime_error("Join key missing from one side");
    }

    std::shared_ptr<arrow::RecordBatch> build = right;
    std::shared_ptr<arrow::RecordBatch> probe = left;
    int                                 build_key_index = right_key_index;
    int                                 probe_key_index = left_key_index;
    bool                                build_is_left = false;
    bool                                output_swap = false;

    if (type == plan::JoinType::INNER && right->num_rows() > left->num_rows())
    {
        build = left;
        probe = right;
        build_key_index = left_key_index;
        probe_key_index = right_key_index;
        build_is_left = true;
        output_swap = true;
    }

    std::vector<std::shared_ptr<arrow::RecordBatch>> build_spill_batches = {build};
    uint64_t                                         approximate_bytes = static_cast<uint64_t>(build->num_rows()) *
                                                                         static_cast<uint64_t>(std::max(1, build->num_columns())) * sizeof(int64_t);
    if (context.memory_limit_bytes > 0 && approximate_bytes > context.memory_limit_bytes && context.spill)
    {
        auto handle_result = context.spill->spill("join-build", build_spill_batches);
        if (!handle_result.ok())
        {
            throw std::runtime_error(handle_result.status().ToString());
        }
        stats.bytes_spilled += static_cast<uint64_t>(handle_result->byte_count);
        stats.spill_files += 1;
        auto reader_result = context.spill->read(*handle_result);
        if (!reader_result.ok())
        {
            throw std::runtime_error(reader_result.status().ToString());
        }
        auto reader = std::move(*reader_result);
        build_spill_batches.clear();
        while (true)
        {
            auto batch_result = reader.next();
            if (!batch_result.ok())
            {
                throw std::runtime_error(batch_result.status().ToString());
            }
            if (!*batch_result)
            {
                break;
            }
            build_spill_batches.push_back(*batch_result);
        }
        build = combineBatchesImpl(build_spill_batches);
    }

    std::unordered_map<std::string, std::vector<int64_t>> build_index;
    build_index.reserve(static_cast<size_t>(build->num_rows()));
    for (int64_t row = 0; row < build->num_rows(); ++row)
    {
        auto scalar_result = build->column(build_key_index)->GetScalar(row);
        if (!scalar_result.ok())
        {
            throw std::runtime_error(scalar_result.status().ToString());
        }
        if (!(*scalar_result) || !(*scalar_result)->is_valid)
        {
            continue;
        }
        build_index[scalarKey(*scalar_result)].push_back(row);
    }

    std::vector<int64_t> left_indices;
    std::vector<int64_t> right_indices;
    left_indices.reserve(static_cast<size_t>(probe->num_rows()));
    right_indices.reserve(static_cast<size_t>(probe->num_rows()));

    for (int64_t row = 0; row < probe->num_rows(); ++row)
    {
        auto scalar_result = probe->column(probe_key_index)->GetScalar(row);
        if (!scalar_result.ok())
        {
            throw std::runtime_error(scalar_result.status().ToString());
        }
        if (!(*scalar_result) || !(*scalar_result)->is_valid)
        {
            if (type == plan::JoinType::LEFT_OUTER && !output_swap)
            {
                left_indices.push_back(row);
                right_indices.push_back(-1);
            }
            continue;
        }

        const auto key = scalarKey(*scalar_result);
        const auto found = build_index.find(key);
        if (found == build_index.end())
        {
            if (type == plan::JoinType::LEFT_OUTER && !output_swap)
            {
                left_indices.push_back(row);
                right_indices.push_back(-1);
            }
            continue;
        }

        for (const auto match_row : found->second)
        {
            if (output_swap)
            {
                left_indices.push_back(match_row);
                right_indices.push_back(row);
            }
            else
            {
                left_indices.push_back(row);
                right_indices.push_back(match_row);
            }
        }
    }

    std::vector<std::shared_ptr<arrow::Field>> out_fields;
    std::vector<std::shared_ptr<arrow::Array>> out_arrays;
    out_fields.reserve(static_cast<size_t>(left->num_columns() + right->num_columns()));
    out_arrays.reserve(static_cast<size_t>(left->num_columns() + right->num_columns()));

    for (int index = 0; index < left->num_columns(); ++index)
    {
        out_fields.push_back(left->schema()->field(index));
        out_arrays.push_back(buildArrayFromIndices(left->column(index), left_indices));
    }
    for (int index = 0; index < right->num_columns(); ++index)
    {
        auto field = right->schema()->field(index);
        if (type == plan::JoinType::LEFT_OUTER)
        {
            field = field->WithNullable(true);
        }
        out_fields.push_back(field);
        out_arrays.push_back(buildArrayFromIndices(right->column(index), right_indices));
    }

    return arrow::RecordBatch::Make(
        arrow::schema(std::move(out_fields)),
        static_cast<int64_t>(left_indices.size()),
        std::move(out_arrays));
}

} // namespace

using namespace mldp_pvxs_driver::query::executor;

std::string mldp_pvxs_driver::query::executor::joinOps(const std::set<PredicateOp>& ops)
{
    return ::joinOpsImpl(ops);
}

std::string_view mldp_pvxs_driver::query::executor::columnTypeName(const ColumnType type)
{
    return ::columnTypeNameImpl(type);
}

ColumnType mldp_pvxs_driver::query::executor::columnTypeFromArrow(const std::shared_ptr<arrow::DataType>& type)
{
    return ::columnTypeFromArrowImpl(type);
}

bool mldp_pvxs_driver::query::executor::matchesLikePattern(const std::string_view value, const std::string_view pattern)
{
    return ::matchesLikePatternImpl(value, pattern);
}

std::vector<ExecutableLiteralValue> mldp_pvxs_driver::query::executor::extractInSubqueryValues(const RecordBatches& batches, const ColumnType type, const std::string_view column)
{
    return ::extractInSubqueryValuesImpl(batches, type, column);
}

std::vector<std::pair<int64_t, int64_t>> mldp_pvxs_driver::query::executor::extractNormalizedWindows(const RecordBatches& batches)
{
    return ::extractNormalizedWindowsImpl(batches);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> mldp_pvxs_driver::query::executor::applyFilter(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<Predicate>& predicates)
{
    if (predicates.empty()) return batch;
    return ::applyFilterImpl(batch, predicates);
}

RecordBatches mldp_pvxs_driver::query::executor::applyProjection(const RecordBatches& input, const std::vector<std::string>& columns)
{
    return ::applyProjectionImpl(input, columns);
}

RecordBatches mldp_pvxs_driver::query::executor::applyProjection(const RecordBatches& input, const std::vector<ExpressionPtr>& expressions, const std::vector<std::string>& names)
{
    return ::applyExpressionProjectionImpl(input, expressions, names);
}

RecordBatches mldp_pvxs_driver::query::executor::applyLimit(const RecordBatches& input, const uint64_t limit)
{
    return ::applyLimitImpl(input, limit);
}

RecordBatches mldp_pvxs_driver::query::executor::applySort(const RecordBatches& input, const std::vector<plan::SortKey>& keys)
{
    return ::applySortImpl(input, keys);
}

std::shared_ptr<arrow::RecordBatch> mldp_pvxs_driver::query::executor::combineBatches(const RecordBatches& batches)
{
    return ::combineBatchesImpl(batches);
}

std::shared_ptr<arrow::RecordBatch> mldp_pvxs_driver::query::executor::qualifyBatchColumns(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& alias)
{
    return ::qualifyBatchColumnsImpl(batch, alias);
}

std::shared_ptr<arrow::RecordBatch> mldp_pvxs_driver::query::executor::joinBatches(const std::shared_ptr<arrow::RecordBatch>& left,
                                                 const std::shared_ptr<arrow::RecordBatch>& right,
                                                 const std::string& left_key,
                                                 const std::string& right_key,
                                                 const plan::JoinType type,
                                                 const ExecutionContext& context,
                                                 QueryStats& stats)
{
    return ::joinBatchesImpl(left, right, left_key, right_key, type, context, stats);
}
