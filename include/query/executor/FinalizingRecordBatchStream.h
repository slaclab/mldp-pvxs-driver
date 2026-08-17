//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file FinalizingRecordBatchStream.h
 * @brief Finalizes query statistics and progress when a stream terminates. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>

#include <chrono>

namespace mldp_pvxs_driver::query::executor {

/** @brief Updates terminal statistics and progress when its input reaches EOF. */
class FinalizingRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a finalizing wrapper that updates stats and progress when its input reaches EOF.
     * @param[in] stream Inner pull stream to wrap.
     * @param[in] context Execution context for memory tracking.
     * @param[in] stats Shared statistics; updated with timing and memory on EOF.
     * @param[in] start Wall-clock time when execution began; used to compute elapsed. */
    FinalizingRecordBatchStream(IRecordBatchStreamUPtr stream, ExecutionContext context,
                                std::shared_ptr<QueryStats> stats,
                                std::chrono::steady_clock::time_point start);

    /** @brief Delegates to the inner stream; on first EOF, finalizes statistics and progress.
     * @return Batch or nullptr on EOF. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr stream_;                    ///< Inner pull stream.
    ExecutionContext context_;                          ///< Execution context.
    std::shared_ptr<QueryStats> stats_;                ///< Shared statistics updated at EOF.
    std::chrono::steady_clock::time_point start_;      ///< Execution start time for elapsed calculation.
    bool finished_{false};                             ///< True after finalization has run.
};

} // namespace mldp_pvxs_driver::query::executor
