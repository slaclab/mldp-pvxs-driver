//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

class PivotRecordBatchStream final : public IRecordBatchStream
{
public:
    PivotRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalPivot pivot, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalPivot pivot_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    RecordBatches batches_;
    std::size_t index_{0};
    bool prepared_{false};
};

} // namespace mldp_pvxs_driver::query::executor
