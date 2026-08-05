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

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalLimit node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "LimitExecutionState";
        }

        RecordBatches execute() override
        {
            if (context().progress) context().progress->setActivity({}, "limit");
            throwIfCancelled();
            return applyLimit(childAt(0).execute(), node_.limit);
        }

    private:
        plan::PhysicalLimit node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeLimitExecutionState(const plan::PhysicalLimit& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
