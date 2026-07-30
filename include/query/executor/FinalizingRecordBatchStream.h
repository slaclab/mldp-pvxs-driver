//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>

#include <chrono>

namespace mldp_pvxs_driver::query::executor {

class FinalizingRecordBatchStream final : public IRecordBatchStream
{
public:
    FinalizingRecordBatchStream(IRecordBatchStreamUPtr stream, ExecutionContext context,
                                std::shared_ptr<QueryStats> stats,
                                std::chrono::steady_clock::time_point start);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr stream_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    std::chrono::steady_clock::time_point start_;
    bool finished_{false};
};

} // namespace mldp_pvxs_driver::query::executor
