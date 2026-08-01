//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <query/QueryExecutor.h>

#include <query/QueryCancellation.h>
#include <query/QueryPlanner.h>
#include <query/QueryableFactory.h>
#include <query/QueryTableCatalog.h>
#include <query/executor/ExecutionState.h>
#include <query/executor/BackendScanRecordBatchStream.h>
#include <query/executor/CreateTableRecordBatchStream.h>
#include <query/executor/FinalizingRecordBatchStream.h>
#include <query/executor/FilterRecordBatchStream.h>
#include <query/executor/LimitRecordBatchStream.h>
#include <query/executor/MaterializedRecordBatchStream.h>
#include <query/executor/ProjectRecordBatchStream.h>
#include <query/executor/PivotRecordBatchStream.h>
#include <query/executor/ExecutorUtils.h>
#include <query/executor/ScanExecutionHelpers.h>
#include <query/executor/WindowBackendScanRecordBatchStream.h>
#include <query/QueryProgress.h>

#include <arrow/memory_pool.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using mldp_pvxs_driver::query::executor::RecordBatches;
using mldp_pvxs_driver::query::executor::BackendScanRecordBatchStream;
using mldp_pvxs_driver::query::executor::CreateTableRecordBatchStream;
using mldp_pvxs_driver::query::executor::FinalizingRecordBatchStream;
using mldp_pvxs_driver::query::executor::LimitRecordBatchStream;
using mldp_pvxs_driver::query::executor::MaterializedRecordBatchStream;
using mldp_pvxs_driver::query::executor::FilterRecordBatchStream;
using mldp_pvxs_driver::query::executor::ProjectRecordBatchStream;
using mldp_pvxs_driver::query::executor::PivotRecordBatchStream;

namespace {

void collectPlanWarnings(const plan::PhysicalNodePtr& node, std::vector<std::string>& warnings)
{
    if (!node) return;
    if (const auto* hash = std::get_if<plan::PhysicalHashJoin>(&node->value)) { warnings.insert(warnings.end(), hash->warnings.begin(), hash->warnings.end()); collectPlanWarnings(hash->left, warnings); collectPlanWarnings(hash->right, warnings); return; }
    if (const auto* nested = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value)) { collectPlanWarnings(nested->outer, warnings); collectPlanWarnings(nested->inner, warnings); return; }
    if (const auto* block = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value)) { warnings.insert(warnings.end(), block->warnings.begin(), block->warnings.end()); collectPlanWarnings(block->outer, warnings); collectPlanWarnings(block->inner, warnings); return; }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value)) { collectPlanWarnings(filter->input, warnings); return; }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value)) { collectPlanWarnings(project->input, warnings); return; }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value)) collectPlanWarnings(limit->input, warnings);
    if (const auto* pivot = std::get_if<plan::PhysicalPivot>(&node->value)) collectPlanWarnings(pivot->input, warnings);
}

std::optional<plan::PhysicalTableScan> resolvePushableInSubqueries(const plan::PhysicalTableScan& scan,
                                                                    const ExecutionContext&        context)
{
    auto resolved_scan = scan;
    if (resolved_scan.in_subqueries.empty()) return resolved_scan;

    QueryPlanner planner(context.table_catalog);
    QueryExecutor executor;
    for (const auto& subquery : resolved_scan.in_subqueries)
    {
        auto predicate = subquery.predicate;
        predicate.values = mldp_pvxs_driver::query::executor::extractInSubqueryValues(
            executor.execute(planner.plan(QueryStatement{*subquery.child}), context).batches,
            subquery.column_type,
            predicate.column);
        if (predicate.values.empty()) return std::nullopt;
        resolved_scan.pushable_predicates.push_back(std::move(predicate));
    }
    resolved_scan.in_subqueries.clear();
    return resolved_scan;
}

std::vector<std::string> pivotLabels(const plan::PhysicalTableScan& scan)
{
    std::vector<std::string> labels;
    for (const auto& predicate : scan.pushable_predicates)
    {
        if (predicate.column != "pv" || (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)) continue;
        for (const auto& value : predicate.values)
        {
            if (!std::holds_alternative<std::string>(value)) continue;
            const auto& pv = std::get<std::string>(value);
            if (std::find(labels.begin(), labels.end(), pv) == labels.end()) labels.push_back(pv);
        }
    }
    return labels;
}

IRecordBatchStreamUPtr makeStreamingPlan(const plan::PhysicalNodePtr& root,
                                          ExecutionContext           context,
                                          const std::shared_ptr<QueryStats>& stats)
{
    if (!root) return nullptr;
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&root->value))
    {
        const bool has_only_pushable_in_subqueries = std::all_of(scan->in_subqueries.begin(), scan->in_subqueries.end(), [](const auto& subquery) {
            return subquery.pushable;
        });
        const bool direct_long_scan = scan->table_name == "mldp.time_series" &&
                                      !scan->arrow_ipc && !scan->derived_query && has_only_pushable_in_subqueries;
        if (!direct_long_scan) return nullptr;

        auto resolved_scan = resolvePushableInSubqueries(*scan, context);
        if (!resolved_scan) return std::make_unique<MaterializedRecordBatchStream>(RecordBatches{});
        if (resolved_scan->window_literal)
        {
            const auto& window = *resolved_scan->window_literal;
            context.series_per_shard = resolved_scan->window_shards.series_per_shard;
            return std::make_unique<WindowBackendScanRecordBatchStream>(
                *resolved_scan, std::move(context), stats,
                std::vector<std::pair<int64_t, int64_t>>{{window[0] * 1'000'000'000LL, window[1] * 1'000'000'000LL}});
        }
        if (resolved_scan->window_subquery)
        {
            QueryPlanner planner(context.table_catalog);
            QueryExecutor executor;
            const auto windows = mldp_pvxs_driver::query::executor::extractNormalizedWindows(
                executor.execute(planner.plan(QueryStatement{*resolved_scan->window_subquery}), context).batches);
            if (windows.empty()) return std::make_unique<MaterializedRecordBatchStream>(RecordBatches{});
            context.series_per_shard = resolved_scan->window_shards.series_per_shard;
            return std::make_unique<WindowBackendScanRecordBatchStream>(*resolved_scan, std::move(context), stats, windows);
        }
        return std::make_unique<BackendScanRecordBatchStream>(*resolved_scan, std::move(context), stats);
    }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&root->value))
    {
        auto input = makeStreamingPlan(filter->input, std::move(context), stats);
        return input ? std::make_unique<FilterRecordBatchStream>(std::move(input), filter->predicates) : nullptr;
    }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&root->value))
    {
        auto input = makeStreamingPlan(project->input, std::move(context), stats);
        return input ? std::make_unique<ProjectRecordBatchStream>(std::move(input), *project) : nullptr;
    }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&root->value))
    {
        auto input = makeStreamingPlan(limit->input, std::move(context), stats);
        return input ? std::make_unique<LimitRecordBatchStream>(std::move(input), limit->limit) : nullptr;
    }
    if (const auto* pivot = std::get_if<plan::PhysicalPivot>(&root->value))
    {
        auto resolved_pivot = *pivot;
        if (const auto* scan = pivot->input ? std::get_if<plan::PhysicalTableScan>(&pivot->input->value) : nullptr;
            scan != nullptr && !scan->in_subqueries.empty())
        {
            const auto resolved_scan = resolvePushableInSubqueries(*scan, context);
            if (!resolved_scan) return std::make_unique<MaterializedRecordBatchStream>(RecordBatches{});
            resolved_pivot.input = plan::makeNode(*resolved_scan);
            resolved_pivot.output_column_labels = pivotLabels(*resolved_scan);
        }
        auto input = makeStreamingPlan(resolved_pivot.input, context, stats);
        return input ? std::make_unique<PivotRecordBatchStream>(std::move(input), std::move(resolved_pivot), context, stats) : nullptr;
    }
    return nullptr;
}

} // namespace

QueryExecutionResult QueryExecutor::execute(const plan::PhysicalNodePtr& root, const ExecutionContext& context) const
{
    if (const auto* create = root ? std::get_if<plan::PhysicalCreateTable>(&root->value) : nullptr)
    {
        auto streamed = executeStream(root, context);
        while (streamed.stream->next()) {}
        return {.batches = {}, .stats = *streamed.stats};
    }
    QueryExecutionResult result;
    const auto start = std::chrono::steady_clock::now();
    if (context.cancellation) context.cancellation->throwIfCancelled();
    if (context.progress)
    {
        context.progress->setPhase(QueryProgressPhase::Executing);
    }
    auto execution_state = executor::makeExecutionState(root, context, result.stats);
    result.batches = execution_state->execute();
    if (context.cancellation) context.cancellation->throwIfCancelled();
    collectPlanWarnings(root, result.stats.plan_warnings);
    result.stats.plan_summary = plan::physicalPlanToString(root);
    if (context.pool != nullptr) result.stats.peak_memory_bytes = static_cast<uint64_t>(context.pool->max_memory());
    for (const auto& batch : result.batches) result.stats.rows_returned += static_cast<uint64_t>(batch->num_rows());
    result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (context.progress)
    {
        context.progress->updateStats(result.stats.rows_returned,
                                      result.stats.bytes_spilled,
                                      result.stats.materialized_bytes,
                                      result.stats.materialized_files,
                                      result.stats.peak_memory_bytes);
    }
    return result;
}

QueryStreamExecutionResult QueryExecutor::executeStream(const plan::PhysicalNodePtr& root,
                                                         ExecutionContext           context) const
{
    const auto start = std::chrono::steady_clock::now();
    if (context.cancellation) context.cancellation->throwIfCancelled();
    if (context.progress) context.progress->setPhase(QueryProgressPhase::Executing);

    auto stats = std::make_shared<QueryStats>();
    collectPlanWarnings(root, stats->plan_warnings);
    stats->plan_summary = plan::physicalPlanToString(root);
    IRecordBatchStreamUPtr stream;
    if (const auto* create = root ? std::get_if<plan::PhysicalCreateTable>(&root->value) : nullptr)
    {
        auto child = executeStream(create->query, context);
        stats = std::move(child.stats);
        stats->plan_summary = plan::physicalPlanToString(root);
        stream = std::make_unique<CreateTableRecordBatchStream>(std::move(child.stream), *create, context, stats);
    }
    else stream = makeStreamingPlan(root, context, stats);
    if (!stream)
    {
        auto execution_state = executor::makeExecutionState(root, context, *stats);
        stream = std::make_unique<MaterializedRecordBatchStream>(execution_state->execute());
    }
    return QueryStreamExecutionResult{
        .stream = std::make_unique<FinalizingRecordBatchStream>(std::move(stream), std::move(context), stats, start),
        .stats = std::move(stats)};
}
