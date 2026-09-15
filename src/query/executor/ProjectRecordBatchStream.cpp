//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
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
