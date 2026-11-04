//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>
#include <query/QueryProgress.h>

#include <arrow/api.h>
#include <arrow/compute/api.h>

#include <stdexcept>
#include <string>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    /** @brief Common type of two branch columns at the same position, or null when incompatible. */
    std::shared_ptr<arrow::DataType> unifyTypes(const std::shared_ptr<arrow::DataType>& left, const std::shared_ptr<arrow::DataType>& right)
    {
        if (left->Equals(*right)) return left;
        if (left->id() == arrow::Type::NA) return right;
        if (right->id() == arrow::Type::NA) return left;
        const auto numeric = [](const arrow::DataType& type) { return arrow::is_integer(type.id()) || arrow::is_floating(type.id()); };
        if (numeric(*left) && numeric(*right))
        {
            return arrow::is_floating(left->id()) || arrow::is_floating(right->id()) ? arrow::float64() : arrow::int64();
        }
        if (left->id() == arrow::Type::TIMESTAMP && right->id() == arrow::Type::TIMESTAMP)
        {
            return arrow::timestamp(arrow::TimeUnit::NANO, static_cast<const arrow::TimestampType&>(*left).timezone());
        }
        return nullptr;
    }

    /** @brief Output schema of the union: first-branch names, per-column unified types. */
    std::shared_ptr<arrow::Schema> unifySchemas(const std::vector<RecordBatches>& branches)
    {
        std::shared_ptr<arrow::Schema> result;
        for (std::size_t branch = 0; branch < branches.size(); ++branch)
        {
            if (branches[branch].empty() || !branches[branch].front()) continue;
            const auto& schema = branches[branch].front()->schema();
            if (!result)
            {
                result = schema;
                continue;
            }
            if (schema->num_fields() != result->num_fields())
            {
                throw std::runtime_error("UNION branches must select the same number of columns: " + std::to_string(result->num_fields()) + " vs " +
                                         std::to_string(schema->num_fields()) + " in branch " + std::to_string(branch + 1));
            }
            arrow::FieldVector fields;
            for (int index = 0; index < result->num_fields(); ++index)
            {
                const auto& field = result->field(index);
                const auto  type = unifyTypes(field->type(), schema->field(index)->type());
                if (!type)
                {
                    throw std::runtime_error("UNION column '" + field->name() + "' has incompatible types " + field->type()->ToString() + " and " +
                                             schema->field(index)->type()->ToString() + " in branch " + std::to_string(branch + 1));
                }
                fields.push_back(arrow::field(field->name(), type, true));
            }
            result = arrow::schema(std::move(fields));
        }
        return result;
    }

    /** @brief Renames and casts a branch batch to the union output schema. */
    std::shared_ptr<arrow::RecordBatch> conformBatch(const std::shared_ptr<arrow::RecordBatch>& batch, const std::shared_ptr<arrow::Schema>& schema)
    {
        arrow::ArrayVector columns;
        columns.reserve(static_cast<std::size_t>(schema->num_fields()));
        for (int index = 0; index < schema->num_fields(); ++index)
        {
            auto        column = batch->column(index);
            const auto& target = schema->field(index)->type();
            if (!column->type()->Equals(*target))
            {
                auto cast = column->type()->id() == arrow::Type::NA ? arrow::MakeArrayOfNull(target, column->length())
                                                                    : arrow::compute::Cast(*column, target);
                if (!cast.ok())
                {
                    throw std::runtime_error("UNION cannot convert column '" + schema->field(index)->name() + "': " + cast.status().ToString());
                }
                column = std::move(cast).ValueUnsafe();
            }
            columns.push_back(std::move(column));
        }
        return arrow::RecordBatch::Make(schema, batch->num_rows(), std::move(columns));
    }

    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalUnion node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            for (const auto& input : node_.inputs) addChild(input);
        }

        std::string_view typeName() const noexcept override
        {
            return "UnionExecutionState";
        }

        RecordBatches execute() override
        {
            if (context().progress) context().progress->setActivity({}, node_.distinct ? "union" : "union all");
            std::vector<RecordBatches> branches;
            branches.reserve(node_.inputs.size());
            for (std::size_t index = 0; index < node_.inputs.size(); ++index)
            {
                throwIfCancelled();
                branches.push_back(childAt(index).execute());
            }
            const auto schema = unifySchemas(branches);
            RecordBatches output;
            if (!schema) return output;
            for (const auto& batches : branches)
            {
                for (const auto& batch : batches)
                {
                    if (batch) output.push_back(conformBatch(batch, schema));
                }
            }
            return node_.distinct ? applyDistinct(output) : output;
        }

    private:
        plan::PhysicalUnion node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeUnionExecutionState(const plan::PhysicalUnion& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
