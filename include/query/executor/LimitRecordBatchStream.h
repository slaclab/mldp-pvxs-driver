//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file LimitRecordBatchStream.h
 * @brief Caps rows emitted by an upstream pull stream. */
#pragma once

#include <query/IQueryable.h>

#include <cstdint>

namespace mldp_pvxs_driver::query::executor {

/** @brief Emits no more than the requested number of rows from its input. */
class LimitRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a limit stream that stops after emitting at most limit rows.
     * @param[in] input Upstream pull stream.
     * @param[in] limit Maximum rows to emit. */
    LimitRecordBatchStream(IRecordBatchStreamUPtr input, uint64_t limit);

    /** @brief Returns the next batch from input, truncated to remaining row budget, or nullptr when exhausted.
     * @return Batch or nullptr. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_; ///< Upstream pull stream.
    uint64_t remaining_;           ///< Remaining rows allowed before EOF is signaled.
};

} // namespace mldp_pvxs_driver::query::executor
