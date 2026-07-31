//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/LimitRecordBatchStream.h>

#include <utility>

using namespace mldp_pvxs_driver::query::executor;

LimitRecordBatchStream::LimitRecordBatchStream(IRecordBatchStreamUPtr input, const uint64_t limit)
    : input_(std::move(input)), remaining_(limit)
{
}

std::shared_ptr<arrow::RecordBatch> LimitRecordBatchStream::next()
{
    if (remaining_ == 0) return nullptr;
    while (auto batch = input_->next())
    {
        const auto rows = static_cast<uint64_t>(batch->num_rows());
        if (rows == 0) continue;
        if (rows <= remaining_)
        {
            remaining_ -= rows;
            return batch;
        }
        const auto result = batch->Slice(0, static_cast<int64_t>(remaining_));
        remaining_ = 0;
        return result;
    }
    return nullptr;
}
