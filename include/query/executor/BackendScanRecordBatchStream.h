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
    BackendScanRecordBatchStream(const plan::PhysicalTableScan& scan, ExecutionContext context, std::shared_ptr<QueryStats> stats);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    plan::PhysicalTableScan scan_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    IQueryableUPtr queryable_;
    IRecordBatchStreamUPtr stream_;
};

} // namespace mldp_pvxs_driver::query::executor
