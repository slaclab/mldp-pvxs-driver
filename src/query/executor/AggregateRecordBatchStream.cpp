//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/AggregateRecordBatchStream.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

AggregateRecordBatchStream::AggregateRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalAggregate& node, ExecutionContext context)
    : input_(std::move(input)), aggregator_(node.spec, std::move(context))
{
}

std::shared_ptr<arrow::RecordBatch> AggregateRecordBatchStream::next()
{
    if (!finished_)
    {
        while (auto batch = input_->next()) aggregator_.consume(batch);
        input_.reset();
        output_ = aggregator_.finish();
        finished_ = true;
    }
    return next_ < output_.size() ? output_[next_++] : nullptr;
}
