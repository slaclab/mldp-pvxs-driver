//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryResult.h>
#include <query/QueryableFactory.h>

#include <stdexcept>

namespace mldp_pvxs_driver::query::executor {
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

RecordBatches fetchBackendPages(const plan::PhysicalTableScan& scan, const std::vector<Predicate>& pushable, const std::vector<Predicate>& local, const ExecutionContext& context, QueryStats& stats)
{
    auto queryable = QueryableFactory::instance().createByTable(scan.table_name);
    RecordBatches output;
    std::string page_token;
    do
    {
        const auto result = queryable->execute(scan.table_name, pushable, scan.projection_hint, context, page_token);
        ++stats.rpc_calls;
        page_token = result.next_page_token;
        if (!result.batch) continue;
        stats.rows_from_backend += static_cast<uint64_t>(result.batch->num_rows());
        auto batch = applyLocal(result.batch, local);
        qualify(scan, batch);
        output.push_back(std::move(batch));
    } while (!page_token.empty());
    return output;
}

} // namespace mldp_pvxs_driver::query::executor
