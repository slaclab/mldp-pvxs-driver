//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryExecutor.h
 * @brief Executes physical query plans as materialized results or pull streams. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

#include <memory>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Fully materialized batches and statistics from an executed plan. */
struct QueryExecutionResult {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches; ///< Fully materialized output batches.
    QueryStats                                       stats;   ///< Accumulated execution statistics.
};

/**
 * @brief Pull stream and shared statistics from a lazily executable plan.
 * @details The stream and stats share ownership so stats remain valid until
 *          the last batch is consumed.
 */
struct QueryStreamExecutionResult {
    IRecordBatchStreamUPtr      stream; ///< Lazy pull stream; drain to completion to finalize stats.
    std::shared_ptr<QueryStats> stats;  ///< Shared statistics updated as batches are consumed.
};

/** @brief Chooses streaming execution where possible and materialized execution otherwise. */
class QueryExecutor
{
public:
    /**
     * @brief Executes a physical plan and returns all batches materialized.
     * @param[in] root    Physical plan root node.
     * @param[in] context Execution resources and controls.
     * @return Materialized batches and accumulated statistics.
     * @throws std::runtime_error On execution failure or cancellation.
     */
    QueryExecutionResult execute(const plan::PhysicalNodePtr& root,
                                 const ExecutionContext& context) const;

    /**
     * @brief Executes a physical plan as a lazy pull stream where possible.
     * @details Blocking plans retain the existing materializing implementation
     *          behind the same pull contract.
     * @param[in] root    Physical plan root node.
     * @param[in] context Execution resources and controls (by value; owned for stream lifetime).
     * @return Pull stream and shared statistics.
     * @throws std::runtime_error On execution failure or cancellation.
     */
    QueryStreamExecutionResult executeStream(const plan::PhysicalNodePtr& root,
                                             ExecutionContext           context) const;
};

} // namespace mldp_pvxs_driver::query
