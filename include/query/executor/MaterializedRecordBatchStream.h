//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/executor/ExecutionState.h>

namespace mldp_pvxs_driver::query::executor {

class MaterializedRecordBatchStream final : public IRecordBatchStream
{
public:
    explicit MaterializedRecordBatchStream(RecordBatches batches);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    RecordBatches batches_;
    std::size_t index_{0};
};

} // namespace mldp_pvxs_driver::query::executor
