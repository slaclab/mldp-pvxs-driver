//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ScanExecutionHelpers.h>
#include <query/executor/StateInternal.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

class PivotExecutionState final : public ExecutionStateBase
{
public:
    PivotExecutionState(plan::PhysicalPivot pivot, const ExecutionContext& context, QueryStats& stats)
        : ExecutionStateBase(context, stats), pivot_(std::move(pivot))
    {
        addChild(pivot_.input);
    }

    std::string_view typeName() const noexcept override { return "PivotExecutionState"; }

    RecordBatches execute() override
    {
        throwIfCancelled();
        return pivotLongBatchesWithSpill(childAt(0).execute(), pivot_.row_key_column,
                                         pivot_.pivot_key_column, pivot_.value_column,
                                         pivot_.output_column_labels,
                                         pivot_.output_batch_size, context(), stats());
    }

private:
    plan::PhysicalPivot pivot_;
};

} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makePivotExecutionState(
    const plan::PhysicalPivot& pivot, const plan::PhysicalNodePtr&,
    const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<PivotExecutionState>(pivot, context, stats);
}
