//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalNestedLoopJoin node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.outer);
            addChild(node_.inner);
        }

        std::string_view typeName() const noexcept override
        {
            return "NestedLoopJoinExecutionState";
        }

        RecordBatches execute() override
        {
            auto joined = joinBatches(combineBatches(childAt(0).execute()), combineBatches(childAt(1).execute()), node_.condition.left_column, node_.condition.right_column, node_.type, context(), stats());
            return joined ? RecordBatches{joined} : RecordBatches{};
        }

    private:
        plan::PhysicalNestedLoopJoin node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeNestedLoopJoinExecutionState(const plan::PhysicalNestedLoopJoin& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
