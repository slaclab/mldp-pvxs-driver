//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file BackendScanRecordBatchStream.h
 * @brief Streams pages from one backend table scan. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Lazily opens and forwards the native stream for a single table scan. */
class BackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Opens the backend scan lazily; the stream is not opened until the first next() call.
     * @param[in] scan Physical scan node describing the table and predicates.
     * @param[in] context Execution context.
     * @param[in] stats Shared statistics accumulator. */
    BackendScanRecordBatchStream(const plan::PhysicalTableScan& scan, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    /** @brief Returns the next batch from the backend, or nullptr at EOF.
     * @return Batch, or nullptr on clean EOF.
     * @throws std::runtime_error On backend error. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    plan::PhysicalTableScan scan_;       ///< Physical scan parameters.
    ExecutionContext context_;           ///< Execution context.
    std::shared_ptr<QueryStats> stats_; ///< Shared statistics.
    IQueryableUPtr queryable_;          ///< Backend queryable created on first next() call.
    IRecordBatchStreamUPtr stream_;     ///< Underlying backend stream, opened lazily.
};

} // namespace mldp_pvxs_driver::query::executor
