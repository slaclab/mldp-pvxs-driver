//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <query/QueryTableCatalog.h>
#include <query/QueryableFactory.h>
#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalDescribe node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node)) {}

        std::string_view typeName() const noexcept override
        {
            return "DescribeExecutionState";
        }

        RecordBatches execute() override
        {
            std::vector<ColumnSchema> schema;
            if (context().table_catalog && context().table_catalog->find(node_.table_name))
            {
                const auto table = *context().table_catalog->find(node_.table_name);
                for (const auto& field : table.schema->fields())
                    schema.push_back(ColumnSchema{.name = field->name(), .type = columnTypeFromArrow(field->type()), .required = false, .is_output = true, .pushable_ops = {}, .filterable_ops = {}, .notes = "Arrow IPC snapshot"});
            }
            else
                schema = QueryableFactory::instance().createByTable(node_.table_name)->tableSchema(node_.table_name);
            arrow::StringBuilder  names, types, pushable, filterable, notes;
            arrow::BooleanBuilder required, output;
            for (const auto& column : schema)
                if (!names.Append(column.name).ok() || !types.Append(columnTypeName(column.type)).ok() || !required.Append(column.required).ok() || !output.Append(column.is_output).ok() || !pushable.Append(joinOps(column.pushable_ops)).ok() || !filterable.Append(joinOps(column.filterable_ops)).ok() || !notes.Append(column.notes).ok())
                    throw std::runtime_error("Failed to build DESCRIBE row");
            std::shared_ptr<arrow::Array> n, t, r, o, p, f, no;
            if (!names.Finish(&n).ok() || !types.Finish(&t).ok() || !required.Finish(&r).ok() || !output.Finish(&o).ok() || !pushable.Finish(&p).ok() || !filterable.Finish(&f).ok() || !notes.Finish(&no).ok())
                throw std::runtime_error("Failed to finalize DESCRIBE output columns");
            auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("name", arrow::utf8()), arrow::field("type", arrow::utf8()), arrow::field("required", arrow::boolean()), arrow::field("is_output", arrow::boolean()), arrow::field("pushable_ops", arrow::utf8()), arrow::field("filterable_ops", arrow::utf8()), arrow::field("notes", arrow::utf8())}), n->length(), {n, t, r, o, p, f, no});
            stats().rows_from_backend += static_cast<uint64_t>(batch->num_rows());
            return {batch};
        }

    private:
        plan::PhysicalDescribe node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeDescribeExecutionState(const plan::PhysicalDescribe& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
