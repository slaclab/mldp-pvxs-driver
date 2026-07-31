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
    FinalizingRecordBatchStream(IRecordBatchStreamUPtr stream, ExecutionContext context,
                                std::shared_ptr<QueryStats> stats,
                                std::chrono::steady_clock::time_point start);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr stream_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    std::chrono::steady_clock::time_point start_;
    bool finished_{false};
};

} // namespace mldp_pvxs_driver::query::executor
