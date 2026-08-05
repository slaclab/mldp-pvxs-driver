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
    /** @brief Constructs a stream that serves a fixed pre-materialized batch sequence.
     * @param[in] batches Batch sequence to serve; drained front-to-back. */
    explicit MaterializedRecordBatchStream(RecordBatches batches);

    /** @brief Returns the next batch from the pre-materialized sequence, or nullptr at EOF.
     * @return Batch or nullptr. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    RecordBatches batches_;    ///< Pre-materialized output batches.
    std::size_t index_{0};     ///< Index of the next batch to return.
};

} // namespace mldp_pvxs_driver::query::executor
