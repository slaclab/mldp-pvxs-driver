#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>
#include <query/QueryProgress.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalProject node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "ProjectExecutionState";
        }

        RecordBatches execute() override
        {
            if (context().progress) context().progress->setActivity({}, "projection");
            throwIfCancelled();
            return node_.expressions.empty() ? applyProjection(childAt(0).execute(), node_.columns) : applyProjection(childAt(0).execute(), node_.expressions, node_.names);
        }

    private:
        plan::PhysicalProject node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeProjectExecutionState(const plan::PhysicalProject& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
