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
#include <query/executor/StateInternal.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalExplain node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node)) {}

        std::string_view typeName() const noexcept override
        {
            return "ExplainExecutionState";
        }

        RecordBatches execute() override
        {
            arrow::StringBuilder builder;
            if (!builder.Append(node_.plan_text).ok())
                throw std::runtime_error("Failed to build EXPLAIN output");
            std::shared_ptr<arrow::Array> output;
            if (!builder.Finish(&output).ok())
                throw std::runtime_error("Failed to finalize EXPLAIN output");
            return {arrow::RecordBatch::Make(arrow::schema({arrow::field("plan", arrow::utf8())}), output->length(), {output})};
        }

    private:
        plan::PhysicalExplain node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeExplainExecutionState(const plan::PhysicalExplain& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
