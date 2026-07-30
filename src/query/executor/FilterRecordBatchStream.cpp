//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/FilterRecordBatchStream.h>

#include <query/executor/ExecutorUtils.h>

#include <stdexcept>
#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

FilterRecordBatchStream::FilterRecordBatchStream(IRecordBatchStreamUPtr input, std::vector<Predicate> predicates)
    : input_(std::move(input)), predicates_(std::move(predicates))
{
}

std::shared_ptr<arrow::RecordBatch> FilterRecordBatchStream::next()
{
    while (auto batch = input_->next())
    {
        auto filtered = applyFilter(batch, predicates_);
        if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
        return *filtered;
    }
    return nullptr;
}
