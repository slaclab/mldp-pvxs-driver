//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ProjectRecordBatchStream.h>

#include <query/executor/ExecutorUtils.h>

#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

ProjectRecordBatchStream::ProjectRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalProject project)
    : input_(std::move(input)), project_(std::move(project))
{
}

std::shared_ptr<arrow::RecordBatch> ProjectRecordBatchStream::next()
{
    auto batch = input_->next();
    if (!batch) return nullptr;
    RecordBatches input{std::move(batch)};
    auto output = project_.expressions.empty()
        ? applyProjection(input, project_.columns)
        : applyProjection(input, project_.expressions, project_.names);
    return output.empty() ? nullptr : output.front();
}
