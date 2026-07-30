//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

class CreateTableRecordBatchStream final : public IRecordBatchStream
{
public:
    CreateTableRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalCreateTable& create,
                                 ExecutionContext context, std::shared_ptr<QueryStats> stats);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalCreateTable create_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    bool done_{false};
};

} // namespace mldp_pvxs_driver::query::executor
