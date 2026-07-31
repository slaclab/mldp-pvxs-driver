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
    PivotRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalPivot pivot, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalPivot pivot_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    RecordBatches batches_;
    std::size_t index_{0};
    bool prepared_{false};
};

} // namespace mldp_pvxs_driver::query::executor
