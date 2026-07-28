#include <query/executor/ExecutorUtils.h>
#include <query/executor/StateInternal.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalFilter node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.input);
        }

        std::string_view typeName() const noexcept override
        {
            return "FilterExecutionState";
        }

        RecordBatches execute() override
        {
            const auto    input = childAt(0).execute();
            RecordBatches output;
            output.reserve(input.size());
            for (const auto& batch : input)
            {
                throwIfCancelled();
                auto filtered = applyFilter(batch, node_.predicates);
                if (!filtered.ok())
                    throw std::runtime_error(filtered.status().ToString());
                output.push_back(*filtered);
            }
            return output;
        }

    private:
        plan::PhysicalFilter node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeFilterExecutionState(const plan::PhysicalFilter& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
