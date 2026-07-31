//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
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
