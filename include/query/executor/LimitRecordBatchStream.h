//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>

#include <cstdint>

namespace mldp_pvxs_driver::query::executor {

class LimitRecordBatchStream final : public IRecordBatchStream
{
public:
    LimitRecordBatchStream(IRecordBatchStreamUPtr input, uint64_t limit);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    uint64_t remaining_;
};

} // namespace mldp_pvxs_driver::query::executor
