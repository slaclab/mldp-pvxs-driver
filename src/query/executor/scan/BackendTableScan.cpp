//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/ExecutorUtils.h>
#include <query/QueryCancellation.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {

std::shared_ptr<arrow::RecordBatch> applyLocal(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<Predicate>& predicates)
{
    if (predicates.empty()) return batch;
    auto filtered = applyFilter(batch, predicates);
    if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
    return *filtered;
}

void qualify(const plan::PhysicalTableScan& scan, std::shared_ptr<arrow::RecordBatch>& batch)
{
    if (scan.qualify_output) batch = qualifyBatchColumns(batch, scan.table_alias);
}

} // namespace

RecordBatches mldp_pvxs_driver::query::executor::fetchBackendPages(const plan::PhysicalTableScan& scan, const std::vector<Predicate>& pushable, const std::vector<Predicate>& local, const ExecutionContext& context, QueryStats& stats)
{
    auto queryable = QueryableFactory::instance().createByTable(scan.table_name);
    if (context.progress) context.progress->setActivity(scan.table_name, "backend scan");
    RecordBatches output;
    if (context.progress) context.progress->beginBackendRpc(scan.table_name, "server cursor");
    auto     stream = queryable->executeStream(scan.table_name, pushable, scan.projection_hint, context);
    uint64_t batch_count = 0;
    std::shared_ptr<arrow::RecordBatch> batch;
    for (bool first = true; (batch = stream->next()) != nullptr; first = false)
    {
        if (!first && context.progress) context.progress->beginBackendRpc(scan.table_name, "server cursor");
        if (context.cancellation) context.cancellation->throwIfCancelled();
        ++stats.rpc_calls;
        ++batch_count;
        const auto backend_rows = static_cast<uint64_t>(batch->num_rows());
        stats.rows_from_backend += backend_rows;
        if (context.progress) context.progress->finishBackendRpc(backend_rows);
        batch = applyLocal(batch, local);
        qualify(scan, batch);
        output.push_back(std::move(batch));
    }
    if (batch_count == 0)
    {
        ++stats.rpc_calls;
        if (context.progress) context.progress->finishBackendRpc(0);
    }
    return output;
}
