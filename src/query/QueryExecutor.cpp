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

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/compute/api.h>
#include <arrow/type.h>

#include <chrono>
#include <sstream>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;

namespace {

std::string joinOps(const std::set<PredicateOp>& ops)
{
    std::ostringstream out;
    bool first = true;
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

bool scalarMatchesPredicate(const std::shared_ptr<arrow::Scalar>& scalar, const Predicate& predicate)
{
    if (!scalar || !scalar->is_valid)
    {
        return false;
    }

    const auto value_text = scalarToString(scalar);
    const auto as_int = [&]() -> int64_t {
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
                                                               const std::vector<Predicate>& predicates)
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
                const std::vector<std::string>& columns)
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

std::vector<std::shared_ptr<arrow::RecordBatch>>
executeNode(const plan::PhysicalNodePtr& node, QueryStats& stats, const ExecutionContext& context)
{
    if (!node)
    {
        return {};
    }

    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        auto queryable = QueryableFactory::instance().createByTable(scan->table_name);
        const auto result = queryable->execute(scan->table_name, scan->pushable_predicates, scan->projection_hint, context);
        std::vector<std::shared_ptr<arrow::RecordBatch>> output;
        if (result.batch != nullptr)
        {
            output.push_back(result.batch);
            stats.rows_from_backend += static_cast<uint64_t>(result.batch->num_rows());
        }
        stats.rpc_calls += 1;
        return output;
    }

    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        const auto input = executeNode(filter->input, stats, context);
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
        auto queryable = QueryableFactory::instance().createByTable(describe->table_name);
        const auto schema = queryable->tableSchema(describe->table_name);

        arrow::StringBuilder col_name;
        arrow::StringBuilder col_type;
        arrow::BooleanBuilder col_required;
        arrow::BooleanBuilder col_output;
        arrow::StringBuilder col_pushable;
        arrow::StringBuilder col_filterable;
        arrow::StringBuilder col_notes;
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
                                            const ExecutionContext& context) const
{
    QueryExecutionResult result;
    const auto start = std::chrono::steady_clock::now();
    result.batches = executeNode(root, result.stats, context);
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
