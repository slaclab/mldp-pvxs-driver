//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/StateInternal.h>

#include <query/ExpressionRegistry.h>

#include <arrow/array/builder_binary.h>

#include <stdexcept>

namespace mldp_pvxs_driver::query::executor {
namespace {

class State final : public ExecutionStateBase
{
public:
    State(const bool operators, const ExecutionContext& context, QueryStats& stats)
        : ExecutionStateBase(context, stats)
        , operators_(operators)
    {
    }

    std::string_view typeName() const noexcept override { return operators_ ? "ShowOperatorsExecutionState" : "ShowFunctionsExecutionState"; }

    RecordBatches execute() override
    {
        const ExpressionRegistry registry;
        const auto descriptors = operators_ ? registry.operators() : registry.functions();
        arrow::StringBuilder names;
        arrow::StringBuilder arities;
        arrow::StringBuilder arguments;
        arrow::StringBuilder returns;
        arrow::StringBuilder descriptions;
        arrow::StringBuilder examples;
        for (const auto& descriptor : descriptors)
        {
            const auto arity = descriptor.kind == ExpressionCallableKind::UNARY_OPERATOR ? "unary" : "binary";
            if (!names.Append(descriptor.name).ok() ||
                (operators_ && !arities.Append(arity).ok()) ||
                !arguments.Append(expressionArgumentsText(descriptor.arguments)).ok() ||
                !returns.Append(columnTypeName(descriptor.returns)).ok() ||
                !descriptions.Append(descriptor.description).ok() ||
                !examples.Append(descriptor.example).ok())
            {
                throw std::runtime_error("Failed to append SHOW callable row");
            }
        }
        std::shared_ptr<arrow::Array> name_output;
        std::shared_ptr<arrow::Array> arity_output;
        std::shared_ptr<arrow::Array> arguments_output;
        std::shared_ptr<arrow::Array> returns_output;
        std::shared_ptr<arrow::Array> description_output;
        std::shared_ptr<arrow::Array> example_output;
        if (!names.Finish(&name_output).ok() ||
            (operators_ && !arities.Finish(&arity_output).ok()) ||
            !arguments.Finish(&arguments_output).ok() || !returns.Finish(&returns_output).ok() ||
            !descriptions.Finish(&description_output).ok() || !examples.Finish(&example_output).ok())
        {
            throw std::runtime_error("Failed to build SHOW callable output");
        }
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> columns;
        fields.push_back(arrow::field(operators_ ? "symbol" : "name", arrow::utf8())); columns.push_back(name_output);
        if (operators_) { fields.push_back(arrow::field("arity", arrow::utf8())); columns.push_back(arity_output); }
        fields.push_back(arrow::field("arguments", arrow::utf8())); columns.push_back(arguments_output);
        fields.push_back(arrow::field("returns", arrow::utf8())); columns.push_back(returns_output);
        fields.push_back(arrow::field("description", arrow::utf8())); columns.push_back(description_output);
        fields.push_back(arrow::field("example", arrow::utf8())); columns.push_back(example_output);
        const auto batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), name_output->length(), std::move(columns));
        stats().rows_from_backend += static_cast<uint64_t>(batch->num_rows());
        return {batch};
    }

private:
    bool operators_{false};
};

} // namespace

std::unique_ptr<IExecutionState> makeShowFunctionsExecutionState(const plan::PhysicalShowFunctions&, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(false, context, stats);
}

std::unique_ptr<IExecutionState> makeShowOperatorsExecutionState(const plan::PhysicalShowOperators&, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(true, context, stats);
}

} // namespace mldp_pvxs_driver::query::executor
