//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file PivotRecordBatchStream.h
 * @brief Materializes and pivots long-form input into wide record batches. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Lazily prepares and emits a wide-table pivot from long-form input. */
class PivotRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a pivot stream that materializes its input on the first next() call.
     * @param[in] input Long-form input pull stream; fully consumed on the first next() call.
     * @param[in] pivot Physical pivot descriptor.
     * @param[in] context Execution context for spill and memory management.
     * @param[in] stats Shared statistics accumulator. */
    PivotRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalPivot pivot, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    /** @brief Materializes and pivots input on the first call, then serves resulting wide batches.
     * @return Wide batch or nullptr at EOF. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;      ///< Long-form input stream; consumed on first next().
    plan::PhysicalPivot pivot_;         ///< Pivot descriptor.
    ExecutionContext context_;           ///< Execution context.
    std::shared_ptr<QueryStats> stats_; ///< Shared statistics.
    RecordBatches batches_;             ///< Pivoted output batches prepared on first next().
    std::size_t index_{0};              ///< Current position in batches_.
    bool prepared_{false};              ///< True after the input has been materialized and pivoted.
};

} // namespace mldp_pvxs_driver::query::executor
