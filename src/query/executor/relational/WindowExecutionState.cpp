//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/StateInternal.h>
#include <query/executor/WindowEvaluator.h>
#include <query/QueryProgress.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalWindow node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "WindowExecutionState";
        }

        RecordBatches execute() override
        {
            throwIfCancelled();
            auto input = childAt(0).execute();
            if (context().progress) context().progress->setActivity({}, "window");
            WindowOperator window(node_, context());
            for (auto& batch : input)
            {
                window.consume(batch);
                batch.reset(); // the operator keeps its own evaluated copy
            }
            RecordBatches output;
            while (auto batch = window.next()) output.push_back(std::move(batch));
            return output;
        }

    private:
        plan::PhysicalWindow node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeWindowExecutionState(const plan::PhysicalWindow& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
