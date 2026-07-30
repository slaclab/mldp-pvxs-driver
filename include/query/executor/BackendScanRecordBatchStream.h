//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

class BackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    BackendScanRecordBatchStream(const plan::PhysicalTableScan& scan, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    plan::PhysicalTableScan scan_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    IQueryableUPtr queryable_;
    IRecordBatchStreamUPtr stream_;
};

} // namespace mldp_pvxs_driver::query::executor
