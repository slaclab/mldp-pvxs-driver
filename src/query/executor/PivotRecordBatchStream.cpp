//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/PivotRecordBatchStream.h>

#include <query/QueryProgress.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

PivotRecordBatchStream::PivotRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalPivot pivot,
                                               ExecutionContext context, std::shared_ptr<QueryStats> stats)
    : input_(std::move(input)), pivot_(std::move(pivot)), context_(std::move(context)), stats_(std::move(stats))
{
}

std::shared_ptr<arrow::RecordBatch> PivotRecordBatchStream::next()
{
    if (!prepared_)
    {
        prepared_ = true;
        if (context_.progress) context_.progress->setActivity("mldp.time_series_table", "wide pivot", "preparing pivot");
        batches_ = pivotLongStreamWithSpill(*input_, pivot_.row_key_column, pivot_.pivot_key_column,
                                            pivot_.value_column, pivot_.output_column_labels,
                                            pivot_.output_batch_size, context_, *stats_);
    }
    return index_ < batches_.size() ? batches_[index_++] : nullptr;
}
