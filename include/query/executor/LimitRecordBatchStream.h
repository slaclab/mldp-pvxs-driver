//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
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
