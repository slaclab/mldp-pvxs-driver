//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file CreateTableRecordBatchStream.h
 * @brief Materializes a stream into a catalog table while preserving pull semantics. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Drains input into a catalog table before reporting end of stream. */
class CreateTableRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a stream that drains input into a catalog table, then reports EOF.
     * @param[in] input Source pull stream to drain.
     * @param[in] create Physical CREATE TABLE descriptor.
     * @param[in] context Execution context for catalog access.
     * @param[in] stats Shared statistics accumulator. */
    CreateTableRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalCreateTable& create,
                                 ExecutionContext context, std::shared_ptr<QueryStats> stats);

    /** @brief On the first call, drains the input stream into the catalog table and returns nullptr.
     * Subsequent calls also return nullptr.
     * @return Always nullptr; the stream serves no output rows.
     * @throws std::runtime_error On catalog write failure. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;      ///< Source stream; fully consumed on first next().
    plan::PhysicalCreateTable create_;  ///< CREATE TABLE descriptor.
    ExecutionContext context_;           ///< Execution context.
    std::shared_ptr<QueryStats> stats_; ///< Shared statistics.
    bool done_{false};                  ///< True after the input has been drained.
};

} // namespace mldp_pvxs_driver::query::executor
