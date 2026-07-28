//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/QueryCancellation.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryResult.h>
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
    RecordBatches output;
    std::string page_token;
    do
    {
        if (context.cancellation) context.cancellation->throwIfCancelled();
        if (context.progress)
        {
            context.progress->beginBackendRpc(scan.table_name, page_token.empty() ? "page 1" : "continuation page");
        }
        const auto result = queryable->execute(scan.table_name, pushable, scan.projection_hint, context, page_token);
        if (context.cancellation) context.cancellation->throwIfCancelled();
        ++stats.rpc_calls;
        const auto backend_rows = result.batch ? static_cast<uint64_t>(result.batch->num_rows()) : 0ULL;
        if (context.progress)
        {
            context.progress->finishBackendRpc(backend_rows);
        }
        page_token = result.next_page_token;
        if (!result.batch) continue;
        stats.rows_from_backend += backend_rows;
        auto batch = applyLocal(result.batch, local);
        qualify(scan, batch);
        output.push_back(std::move(batch));
    } while (!page_token.empty());
    return output;
}
