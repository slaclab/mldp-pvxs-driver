//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/QueryCancellation.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryResult.h>
#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>

#include <algorithm>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

RecordBatches mldp_pvxs_driver::query::executor::fetchTimeSeriesWindows(const plan::PhysicalTableScan& scan,
                                     const std::vector<Predicate>& pushable,
                                     const std::vector<Predicate>& local,
                                     const std::vector<std::pair<int64_t, int64_t>>& windows,
                                     const ExecutionContext& context,
                                     QueryStats& stats)
{
    auto queryable = QueryableFactory::instance().createByTable(scan.table_name);
    RecordBatches output;
    for (const auto& [begin_ns, end_ns] : windows)
    {
        if (context.cancellation) context.cancellation->throwIfCancelled();
        auto predicates = pushable;
        predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [&scan](const Predicate& predicate) {
            return (scan.window_subquery || scan.window_literal) && predicate.column == "time";
        }), predicates.end());
        if (scan.window_subquery || scan.window_literal)
        {
            predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {begin_ns / 1'000'000'000LL}});
            predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {end_ns / 1'000'000'000LL}});
        }
        std::string page_token;
        do
        {
            if (context.progress)
                context.progress->beginBackendRpc(scan.table_name, page_token.empty() ? "window" : "window continuation page");
            const auto result = queryable->execute(scan.table_name, predicates, scan.projection_hint, context, page_token);
            if (context.cancellation) context.cancellation->throwIfCancelled();
            ++stats.rpc_calls;
            const auto backend_rows = result.batch ? static_cast<uint64_t>(result.batch->num_rows()) : 0ULL;
            if (context.progress)
                context.progress->finishBackendRpc(backend_rows);
            page_token = result.next_page_token;
            if (!result.batch) continue;
            stats.rows_from_backend += backend_rows;
            auto batch = result.batch;
            if (!local.empty())
            {
                auto filtered = applyFilter(batch, local);
                if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
                batch = *filtered;
            }
            if (scan.qualify_output) batch = qualifyBatchColumns(batch, scan.table_alias);
            output.push_back(std::move(batch));
        } while (!page_token.empty());
    }
    return output;
}
