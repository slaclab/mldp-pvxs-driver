//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/MaterializedRecordBatchStream.h>

#include <utility>

using namespace mldp_pvxs_driver::query::executor;

MaterializedRecordBatchStream::MaterializedRecordBatchStream(RecordBatches batches)
    : batches_(std::move(batches))
{
}

std::shared_ptr<arrow::RecordBatch> MaterializedRecordBatchStream::next()
{
    return index_ < batches_.size() ? batches_[index_++] : nullptr;
}
