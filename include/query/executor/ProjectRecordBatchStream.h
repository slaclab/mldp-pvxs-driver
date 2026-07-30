//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

class ProjectRecordBatchStream final : public IRecordBatchStream
{
public:
    ProjectRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalProject project);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalProject project_;
};

} // namespace mldp_pvxs_driver::query::executor
