//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/BackendScanRecordBatchStream.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>
#include <query/executor/ExecutorUtils.h>

#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

BackendScanRecordBatchStream::BackendScanRecordBatchStream(const plan::PhysicalTableScan& scan,
                                                           ExecutionContext context,
                                                           std::shared_ptr<QueryStats> stats)
    : scan_(scan), context_(std::move(context)), stats_(std::move(stats)),
      queryable_(QueryableFactory::instance().createByTable(scan_.table_name))
{
    if (context_.progress)
    {
        context_.progress->setActivity(scan_.table_name, "backend scan", "opening server cursor");
        context_.progress->beginBackendRpc(scan_.table_name, "server cursor");
    }
    stream_ = queryable_->executeStream(scan_.table_name, scan_.pushable_predicates, scan_.projection_hint, context_);
}

std::shared_ptr<arrow::RecordBatch> BackendScanRecordBatchStream::next()
{
    if (context_.cancellation) context_.cancellation->throwIfCancelled();
    auto batch = stream_->next();
    if (!batch) return nullptr;
    ++stats_->rpc_calls;
    const auto rows = static_cast<uint64_t>(batch->num_rows());
    stats_->rows_from_backend += rows;
    if (context_.progress) context_.progress->finishBackendRpc(rows);
    if (scan_.qualify_output) batch = qualifyBatchColumns(batch, scan_.table_alias);
    return batch;
}
