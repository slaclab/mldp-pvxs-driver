#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>

namespace mldp_pvxs_driver::query::executor {
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalSort node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "SortExecutionState";
        }

        RecordBatches execute() override
        {
            return applySort(childAt(0).execute(), node_.keys);
        }

    private:
        plan::PhysicalSort node_;
    };
} // namespace

std::unique_ptr<IExecutionState> makeSortExecutionState(const plan::PhysicalSort& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
} // namespace mldp_pvxs_driver::query::executor
