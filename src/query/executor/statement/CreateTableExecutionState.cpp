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
        State(plan::PhysicalCreateTable node, const ExecutionContext& context, QueryStats& stats) : ExecutionStateBase(context, stats), node_(std::move(node))
        {
            addChild(node_.query);
        }

        std::string_view typeName() const noexcept override
        {
            return "CreateTableExecutionState";
        }

        RecordBatches execute() override
        {
            if (!context().table_catalog)
                throw std::runtime_error("CREATE TABLE has no catalog");
            const auto status = context().table_catalog->create(node_.table_name, node_.temporary ? TableLifetime::Session : TableLifetime::Persistent, childAt(0).execute());
            if (!status.ok())
                throw std::runtime_error(status.ToString());
            if (const auto table = context().table_catalog->find(node_.table_name))
            {
                stats().materialized_files++;
                stats().materialized_bytes += static_cast<uint64_t>(table->byte_count);
            }
            return {};
        }

    private:
        plan::PhysicalCreateTable node_;
    };
} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeCreateTableExecutionState(const plan::PhysicalCreateTable& node, const plan::PhysicalNodePtr&, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<State>(node, context, stats);
}
