//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/WindowRecordBatchStream.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

WindowRecordBatchStream::WindowRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalWindow& node, ExecutionContext context)
    : input_(std::move(input)), window_(node, std::move(context))
{
}

std::shared_ptr<arrow::RecordBatch> WindowRecordBatchStream::next()
{
    if (input_)
    {
        while (auto batch = input_->next()) window_.consume(batch);
        input_.reset();
    }
    return window_.next();
}
