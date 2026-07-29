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

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

#include <memory>
#include <vector>

namespace mldp_pvxs_driver::query {

struct QueryExecutionResult {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    QueryStats                                       stats;
};

/**
 * A pull execution result.  The execution context and statistics are shared
 * with the stream so a cursor remains valid until its final batch is consumed
 * or the stream is destroyed.
 */
struct QueryStreamExecutionResult {
    IRecordBatchStreamUPtr      stream;
    std::shared_ptr<QueryStats> stats;
};

class QueryExecutor
{
public:
    QueryExecutionResult execute(const plan::PhysicalNodePtr& root,
                                 const ExecutionContext& context) const;

    /**
     * Execute a scan/filter/project/limit pipeline lazily when possible.
     * Blocking plans retain the existing materializing implementation behind
     * the same pull contract.
     */
    QueryStreamExecutionResult executeStream(const plan::PhysicalNodePtr& root,
                                             ExecutionContext           context) const;
};

} // namespace mldp_pvxs_driver::query
