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

#include <query/QueryResult.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>

#include <algorithm>
#include <cctype>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/builder.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>
#include <arrow/type.h>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using namespace mldp_pvxs_driver::query;

namespace {

std::string joinOps(const std::set<PredicateOp>& ops)
{
    std::ostringstream out;
    bool               first = true;
    for (const auto op : ops)
    {
        if (!first)
        {
            out << ",";
        }
        first = false;
        out << static_cast<int>(op);
    }
    return out.str();
}

std::string scalarToString(const std::shared_ptr<arrow::Scalar>& scalar)
{
    if (!scalar || !scalar->is_valid)
    {
        return "";
    }
    return scalar->ToString();
}

bool matchesLikePattern(std::string_view value, std::string_view pattern)
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

bool scalarMatchesPredicate(const std::shared_ptr<arrow::Scalar>& scalar, const Predicate& predicate)
{
    if (!scalar || !scalar->is_valid)
    {
        return false;
    }

    const auto value_text = scalarToString(scalar);
    const auto as_int = [&]() -> int64_t
    {
        if (scalar->type->id() == arrow::Type::INT64)
        {
            return std::dynamic_pointer_cast<arrow::Int64Scalar>(scalar)->value;
        }
        if (scalar->type->id() == arrow::Type::TIMESTAMP)
        {
            return std::dynamic_pointer_cast<arrow::TimestampScalar>(scalar)->value;
        }
        if (scalar->type->id() == arrow::Type::DURATION)
        {
            return std::dynamic_pointer_cast<arrow::DurationScalar>(scalar)->value;
        }
        throw std::runtime_error("Predicate compared non-numeric scalar as integer");
    };

    const auto compareSingle = [&](const std::variant<std::string, int64_t, bool>& literal, const PredicateOp op)
    {
        if (std::holds_alternative<std::string>(literal))
        {
            const auto& rhs = std::get<std::string>(literal);
            if (op == PredicateOp::EQ)
                return value_text == rhs;
            if (op == PredicateOp::NEQ)
                return value_text != rhs;
            if (op == PredicateOp::PREFIX)
                return value_text.rfind(rhs, 0) == 0;
            if (op == PredicateOp::CONTAINS)
                return value_text.find(rhs) != std::string::npos;
            if (op == PredicateOp::LIKE)
                return matchesLikePattern(value_text, rhs);
            if (op == PredicateOp::LT)
                return value_text < rhs;
            if (op == PredicateOp::LTE)
                return value_text <= rhs;
            if (op == PredicateOp::GT)
                return value_text > rhs;
            if (op == PredicateOp::GTE)
                return value_text >= rhs;
            return false;
        }
        if (std::holds_alternative<int64_t>(literal))
        {
            const auto lhs = as_int();
            const auto rhs = std::get<int64_t>(literal);
            if (op == PredicateOp::EQ)
                return lhs == rhs;
            if (op == PredicateOp::NEQ)
                return lhs != rhs;
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

        if (scalar->type->id() != arrow::Type::BOOL)
        {
            return false;
        }
        const auto lhs = std::dynamic_pointer_cast<arrow::BooleanScalar>(scalar)->value;
        const auto rhs = std::get<bool>(literal);
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
        if (predicate.values.size() != 2 || !std::holds_alternative<int64_t>(predicate.values[0]) ||
            !std::holds_alternative<int64_t>(predicate.values[1]))
        {
            return false;
        }
        const auto lhs = as_int();
        const auto lo = std::get<int64_t>(predicate.values[0]);
        const auto hi = std::get<int64_t>(predicate.values[1]);
        return lhs >= lo && lhs <= hi;
    }

    if (predicate.values.empty())
    {
        return false;
    }
    return compareSingle(predicate.values.front(), predicate.op);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> applyFilter(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                               const std::vector<Predicate>&              predicates)
{
    arrow::BooleanBuilder mask_builder;
    for (int64_t row = 0; row < batch->num_rows(); ++row)
    {
        bool include = true;
        for (const auto& predicate : predicates)
        {
            const auto field_index = batch->schema()->GetFieldIndex(predicate.column);
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
    }

    std::shared_ptr<arrow::Array> mask;
    RETURN_NOT_OK(mask_builder.Finish(&mask));
    ARROW_ASSIGN_OR_RAISE(auto filtered, arrow::compute::Filter(batch, mask));
    return filtered.record_batch();
}

std::vector<std::shared_ptr<arrow::RecordBatch>>
applyProjection(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input,
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

std::vector<std::shared_ptr<arrow::RecordBatch>>
applyLimit(const std::vector<std::shared_ptr<arrow::RecordBatch>>& input, const uint64_t limit)
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

std::shared_ptr<arrow::RecordBatch> combineBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
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

std::shared_ptr<arrow::RecordBatch> qualifyBatchColumns(const std::shared_ptr<arrow::RecordBatch>& batch,
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

std::shared_ptr<arrow::RecordBatch> joinBatches(const std::shared_ptr<arrow::RecordBatch>& left,
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
        build = combineBatches(build_spill_batches);
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

void collectPlanWarnings(const plan::PhysicalNodePtr& node, std::vector<std::string>& warnings)
{
    if (!node)
    {
        return;
    }
    if (const auto* hash = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        warnings.insert(warnings.end(), hash->warnings.begin(), hash->warnings.end());
        collectPlanWarnings(hash->left, warnings);
        collectPlanWarnings(hash->right, warnings);
        return;
    }
    if (const auto* nested = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        collectPlanWarnings(nested->outer, warnings);
        collectPlanWarnings(nested->inner, warnings);
        return;
    }
    if (const auto* block = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        warnings.insert(warnings.end(), block->warnings.begin(), block->warnings.end());
        collectPlanWarnings(block->outer, warnings);
        collectPlanWarnings(block->inner, warnings);
        return;
    }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        collectPlanWarnings(filter->input, warnings);
        return;
    }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        collectPlanWarnings(project->input, warnings);
        return;
    }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        collectPlanWarnings(limit->input, warnings);
    }
}

std::vector<std::shared_ptr<arrow::RecordBatch>>
executeNode(const plan::PhysicalNodePtr& node, QueryStats& stats, const ExecutionContext& context)
{
    if (!node)
    {
        return {};
    }

    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        auto                                             queryable = QueryableFactory::instance().createByTable(scan->table_name);
        std::vector<std::shared_ptr<arrow::RecordBatch>> output;
        std::string                                      page_token;
        do
        {
            const auto result = queryable->execute(scan->table_name,
                                                   scan->pushable_predicates,
                                                   scan->projection_hint,
                                                   context,
                                                   page_token);
            ++stats.rpc_calls;
            if (result.batch != nullptr)
            {
                output.push_back(scan->qualify_output ? qualifyBatchColumns(result.batch, scan->table_alias) : result.batch);
                stats.rows_from_backend += static_cast<uint64_t>(result.batch->num_rows());
            }
            page_token = result.next_page_token;
        } while (!page_token.empty());
        return output;
    }

    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        const auto                                       input = executeNode(filter->input, stats, context);
        std::vector<std::shared_ptr<arrow::RecordBatch>> output;
        output.reserve(input.size());
        for (const auto& batch : input)
        {
            auto filtered = applyFilter(batch, filter->predicates);
            if (!filtered.ok())
            {
                throw std::runtime_error(filtered.status().ToString());
            }
            output.push_back(*filtered);
        }
        return output;
    }

    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        return applyProjection(executeNode(project->input, stats, context), project->columns);
    }

    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        return applyLimit(executeNode(limit->input, stats, context), limit->limit);
    }

    if (const auto* join = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        auto left_batch = combineBatches(executeNode(join->left, stats, context));
        auto right_batch = combineBatches(executeNode(join->right, stats, context));
        auto joined = joinBatches(left_batch, right_batch, join->condition.left_column, join->condition.right_column, join->type, context, stats);
        if (!joined)
        {
            return {};
        }
        return {joined};
    }

    if (const auto* join = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        auto outer_batch = combineBatches(executeNode(join->outer, stats, context));
        auto inner_batch = combineBatches(executeNode(join->inner, stats, context));
        auto joined = joinBatches(outer_batch,
                                  inner_batch,
                                  join->condition.left_column,
                                  join->condition.right_column,
                                  join->type,
                                  context,
                                  stats);
        if (!joined)
        {
            return {};
        }
        return {joined};
    }

    if (const auto* join = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        auto outer_batch = combineBatches(executeNode(join->outer, stats, context));
        auto inner_batch = combineBatches(executeNode(join->inner, stats, context));
        auto joined = joinBatches(outer_batch,
                                  inner_batch,
                                  join->condition.left_column,
                                  join->condition.right_column,
                                  join->type,
                                  context,
                                  stats);
        if (!joined)
        {
            return {};
        }
        return {joined};
    }

    if (std::holds_alternative<plan::PhysicalShowTables>(node->value))
    {
        arrow::StringBuilder table_builder;
        for (const auto& table : QueryableFactory::instance().registeredTables())
        {
            if (!table_builder.Append(table).ok())
            {
                throw std::runtime_error("Failed to append SHOW TABLES row");
            }
        }
        std::shared_ptr<arrow::Array> table_array;
        if (!table_builder.Finish(&table_array).ok())
        {
            throw std::runtime_error("Failed to build SHOW TABLES output");
        }
        auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("table_name", arrow::utf8())}),
                                              table_array->length(),
                                              {table_array});
        stats.rows_from_backend += static_cast<uint64_t>(batch->num_rows());
        return {batch};
    }

    if (const auto* describe = std::get_if<plan::PhysicalDescribe>(&node->value))
    {
        auto       queryable = QueryableFactory::instance().createByTable(describe->table_name);
        const auto schema = queryable->tableSchema(describe->table_name);

        arrow::StringBuilder  col_name;
        arrow::StringBuilder  col_type;
        arrow::BooleanBuilder col_required;
        arrow::BooleanBuilder col_output;
        arrow::StringBuilder  col_pushable;
        arrow::StringBuilder  col_filterable;
        arrow::StringBuilder  col_notes;
        for (const auto& column : schema)
        {
            if (!col_name.Append(column.name).ok() ||
                !col_type.Append(std::to_string(static_cast<int>(column.type))).ok() ||
                !col_required.Append(column.required).ok() ||
                !col_output.Append(column.is_output).ok() ||
                !col_pushable.Append(joinOps(column.pushable_ops)).ok() ||
                !col_filterable.Append(joinOps(column.filterable_ops)).ok() ||
                !col_notes.Append(column.notes).ok())
            {
                throw std::runtime_error("Failed to build DESCRIBE row");
            }
        }

        std::shared_ptr<arrow::Array> arr_name;
        std::shared_ptr<arrow::Array> arr_type;
        std::shared_ptr<arrow::Array> arr_required;
        std::shared_ptr<arrow::Array> arr_output;
        std::shared_ptr<arrow::Array> arr_pushable;
        std::shared_ptr<arrow::Array> arr_filterable;
        std::shared_ptr<arrow::Array> arr_notes;
        if (!col_name.Finish(&arr_name).ok() ||
            !col_type.Finish(&arr_type).ok() ||
            !col_required.Finish(&arr_required).ok() ||
            !col_output.Finish(&arr_output).ok() ||
            !col_pushable.Finish(&arr_pushable).ok() ||
            !col_filterable.Finish(&arr_filterable).ok() ||
            !col_notes.Finish(&arr_notes).ok())
        {
            throw std::runtime_error("Failed to finalize DESCRIBE output columns");
        }

        auto batch = arrow::RecordBatch::Make(
            arrow::schema({
                arrow::field("name", arrow::utf8()),
                arrow::field("type", arrow::utf8()),
                arrow::field("required", arrow::boolean()),
                arrow::field("is_output", arrow::boolean()),
                arrow::field("pushable_ops", arrow::utf8()),
                arrow::field("filterable_ops", arrow::utf8()),
                arrow::field("notes", arrow::utf8()),
            }),
            arr_name->length(),
            {arr_name, arr_type, arr_required, arr_output, arr_pushable, arr_filterable, arr_notes});
        stats.rows_from_backend += static_cast<uint64_t>(batch->num_rows());
        return {batch};
    }

    if (const auto* explain = std::get_if<plan::PhysicalExplain>(&node->value))
    {
        arrow::StringBuilder builder;
        if (!builder.Append(explain->plan_text).ok())
        {
            throw std::runtime_error("Failed to build EXPLAIN output");
        }
        std::shared_ptr<arrow::Array> output;
        if (!builder.Finish(&output).ok())
        {
            throw std::runtime_error("Failed to finalize EXPLAIN output");
        }
        auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("plan", arrow::utf8())}),
                                              output->length(),
                                              {output});
        return {batch};
    }

    return {};
}

} // namespace

QueryExecutionResult QueryExecutor::execute(const plan::PhysicalNodePtr& root,
                                            const ExecutionContext&      context) const
{
    QueryExecutionResult result;
    const auto           start = std::chrono::steady_clock::now();
    result.batches = executeNode(root, result.stats, context);
    collectPlanWarnings(root, result.stats.plan_warnings);
    result.stats.plan_summary = plan::physicalPlanToString(root);
    if (context.pool != nullptr)
    {
        result.stats.peak_memory_bytes = static_cast<uint64_t>(context.pool->max_memory());
    }
    for (const auto& batch : result.batches)
    {
        result.stats.rows_returned += static_cast<uint64_t>(batch->num_rows());
    }
    result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}
