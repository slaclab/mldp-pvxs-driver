//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
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
