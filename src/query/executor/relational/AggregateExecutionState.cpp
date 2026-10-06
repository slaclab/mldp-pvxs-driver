//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/GroupedAggregator.h>
#include <query/executor/StateInternal.h>
#include <query/QueryProgress.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalAggregate node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "AggregateExecutionState";
        }

        RecordBatches execute() override
        {
            throwIfCancelled();
            auto input = childAt(0).execute();
            if (context().progress) context().progress->setActivity({}, "aggregating");
            GroupedAggregator aggregator(node_.spec, context());
            for (auto& batch : input)
            {
                aggregator.consume(batch);
                batch.reset(); // release input as it is folded
            }
            return aggregator.finish();
        }

    private:
        plan::PhysicalAggregate node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeAggregateExecutionState(const plan::PhysicalAggregate& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
