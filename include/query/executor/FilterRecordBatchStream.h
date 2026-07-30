//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>

namespace mldp_pvxs_driver::query::executor {

class FilterRecordBatchStream final : public IRecordBatchStream
{
public:
    FilterRecordBatchStream(IRecordBatchStreamUPtr input, std::vector<Predicate> predicates);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    std::vector<Predicate> predicates_;
};

} // namespace mldp_pvxs_driver::query::executor
