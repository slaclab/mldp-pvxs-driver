//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file MaterializedRecordBatchStream.h
 * @brief Exposes an already materialized batch sequence through the stream interface. */
#pragma once

#include <query/executor/ExecutionState.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Iterates a fixed sequence of already materialized record batches. */
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
