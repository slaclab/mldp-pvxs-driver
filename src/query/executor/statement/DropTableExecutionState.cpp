//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#include <query/QueryTableCatalog.h>
#include <query/executor/StateInternal.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {
    class State final : public ExecutionStateBase
    {
    public:
        State(plan::PhysicalDropTable node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node)) {}

        std::string_view typeName() const noexcept override
        {
            return "DropTableExecutionState";
        }

        RecordBatches execute() override
        {
            if (!context().table_catalog)
                throw std::runtime_error("DROP TABLE has no catalog");
            const auto status = context().table_catalog->drop(node_.table_name);
            if (!status.ok())
                throw std::runtime_error(status.ToString());
            return {};
        }

    private:
        plan::PhysicalDropTable node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeDropTableExecutionState(const plan::PhysicalDropTable& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
