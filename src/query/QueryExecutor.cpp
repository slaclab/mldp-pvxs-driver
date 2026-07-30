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
#include <query/executor/ExecutorUtils.h>
#include <query/executor/ScanExecutionHelpers.h>
#include <query/QueryProgress.h>

#include <arrow/memory_pool.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using mldp_pvxs_driver::query::executor::RecordBatches;

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

class MaterializedRecordBatchStream final : public IRecordBatchStream
{
public:
    explicit MaterializedRecordBatchStream(RecordBatches batches)
        : batches_(std::move(batches))
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        return index_ < batches_.size() ? batches_[index_++] : nullptr;
    }

private:
    RecordBatches batches_;
    std::size_t index_{0};
};

class BackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    BackendScanRecordBatchStream(const plan::PhysicalTableScan& scan,
                                 ExecutionContext               context,
                                 std::shared_ptr<QueryStats>    stats)
        : scan_(scan), context_(std::move(context)), stats_(std::move(stats)), queryable_(QueryableFactory::instance().createByTable(scan_.table_name))
    {
        if (context_.progress)
        {
            context_.progress->setActivity(scan_.table_name, "backend scan", "opening server cursor");
            context_.progress->beginBackendRpc(scan_.table_name, "server cursor");
        }
        stream_ = queryable_->executeStream(scan_.table_name, scan_.pushable_predicates, scan_.projection_hint, context_);
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (context_.cancellation) context_.cancellation->throwIfCancelled();
        auto batch = stream_->next();
        if (!batch) return nullptr;
        ++stats_->rpc_calls;
        const auto backend_rows = static_cast<uint64_t>(batch->num_rows());
        stats_->rows_from_backend += backend_rows;
        if (context_.progress) context_.progress->finishBackendRpc(backend_rows);
        if (scan_.qualify_output) batch = executor::qualifyBatchColumns(batch, scan_.table_alias);
        return batch;
    }

private:
    plan::PhysicalTableScan scan_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    IQueryableUPtr queryable_;
    IRecordBatchStreamUPtr stream_;
};

class WindowBackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    WindowBackendScanRecordBatchStream(const plan::PhysicalTableScan& scan,
                                       ExecutionContext               context,
                                       std::shared_ptr<QueryStats>    stats,
                                       std::vector<std::pair<int64_t, int64_t>> windows)
        : scan_(scan), context_(std::move(context)), stats_(std::move(stats)), queryable_(QueryableFactory::instance().createByTable(scan_.table_name))
    {
        if (windows.empty()) throw std::runtime_error("Streaming window scan requires at least one window");
        windows_ = std::move(windows);
        selectWindow();
        for (const auto& predicate : scan_.pushable_predicates)
        {
            if (predicate.column != "pv" || (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)) continue;
            for (const auto& value : predicate.values)
                if (std::holds_alternative<std::string>(value)) requested_pvs_.push_back(std::get<std::string>(value));
        }
        if (requested_pvs_.empty()) throw std::runtime_error("MLDP time-series window requires a PV predicate");
        prepareNextSlice();
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        for (;;)
        {
            if (group_index_ >= groups_.size()) return nullptr;
            if (context_.cancellation) context_.cancellation->throwIfCancelled();
            auto& group = groups_[group_index_];
            if (!group.next.valid()) scheduleNext(group);
            auto result = group.next.get();
            group.stream = std::move(result.stream);
            auto batch = std::move(result.batch);
            if (!batch)
            {
                if (context_.progress) context_.progress->completeShard();
                group.stream.reset();
                ++group_index_;
                if (next_group_to_start_ < groups_.size())
                {
                    scheduleFirst(groups_[next_group_to_start_]);
                    ++next_group_to_start_;
                }
                if (context_.progress)
                    context_.progress->setParallelShards(
                        static_cast<uint64_t>(next_group_to_start_ - group_index_),
                        parallel_shard_limit_);
                if (group_index_ == groups_.size()) prepareNextSlice();
                continue;
            }
            ++stats_->rpc_calls;
            const auto backend_rows = static_cast<uint64_t>(batch->num_rows());
            stats_->rows_from_backend += backend_rows;
            if (context_.progress) context_.progress->finishBackendRpc(backend_rows);
            if (!final_slice_)
            {
                const Predicate upper{.column = "time", .op = PredicateOp::LT, .values = {TimestampNsLiteral{slice_end_ns_}}};
                auto filtered = executor::applyFilter(batch, {upper});
                if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
                batch = *filtered;
            }
            if (scan_.qualify_output) batch = executor::qualifyBatchColumns(batch, scan_.table_alias);
            if (context_.progress)
            {
                context_.progress->setWindowShard(window_index_ + 1,
                                                   group.slice_index,
                                                   group.index,
                                                   group.series_in_shard);
            }
            return batch;
        }
        return nullptr;
    }

private:
    struct PullResult
    {
        IRecordBatchStreamUPtr stream;
        std::shared_ptr<arrow::RecordBatch> batch;
    };

    struct Group
    {
        std::size_t index{0};
        uint64_t slice_index{0};
        uint64_t series_in_shard{0};
        std::vector<Predicate> predicates;
        IRecordBatchStreamUPtr stream;
        std::future<PullResult> next;
    };

    void scheduleFirst(Group& group)
    {
        const auto predicates = group.predicates;
        group.next = std::async(std::launch::async, [this, predicates]
        {
            auto stream = queryable_->executeStream(scan_.table_name, predicates, scan_.projection_hint, context_);
            auto batch = stream->next();
            return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
        });
    }

    void scheduleNext(Group& group)
    {
        auto stream = std::move(group.stream);
        group.next = std::async(std::launch::async, [stream = std::move(stream)]() mutable
        {
            auto batch = stream->next();
            return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
        });
    }

    void prepareNextSlice()
    {
        groups_.clear();
        group_index_ = 0;
        while (window_index_ < windows_.size())
        {
            if (slice_begin_ns_ > window_end_ns_)
            {
                ++window_index_;
                if (window_index_ >= windows_.size()) return;
                selectWindow();
                continue;
            }
            const auto remaining = window_end_ns_ - slice_begin_ns_;
            slice_end_ns_ = remaining < scan_.window_shards.slice_ns
                ? window_end_ns_
                : slice_begin_ns_ + scan_.window_shards.slice_ns;
            final_slice_ = slice_end_ns_ == window_end_ns_;
            const auto slice_index = static_cast<uint64_t>((slice_begin_ns_ - window_begin_ns_) / scan_.window_shards.slice_ns + 1);
            for (std::size_t pv_offset = 0; pv_offset < requested_pvs_.size(); pv_offset += scan_.window_shards.series_per_shard)
            {
                auto predicates = scan_.pushable_predicates;
                predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [](const Predicate& predicate) {
                    return predicate.column == "time" || predicate.column == "pv";
                }), predicates.end());
                std::vector<ExecutableLiteralValue> pv_values;
                const auto pv_end = std::min(requested_pvs_.size(), pv_offset + static_cast<std::size_t>(scan_.window_shards.series_per_shard));
                for (std::size_t index = pv_offset; index < pv_end; ++index) pv_values.emplace_back(requested_pvs_[index]);
                const auto series_in_shard = static_cast<uint64_t>(pv_values.size());
                predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(pv_values)});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {slice_begin_ns_ / 1'000'000'000LL}});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {slice_end_ns_ / 1'000'000'000LL}});
                groups_.push_back(Group{.index = pv_offset / static_cast<std::size_t>(scan_.window_shards.series_per_shard) + 1,
                                        .slice_index = slice_index,
                                        .series_in_shard = series_in_shard,
                                        .predicates = std::move(predicates)});
            }
            const auto concurrency = std::min(groups_.size(), std::max<std::size_t>(1, queryable_->maxConcurrentStreams()));
            parallel_shard_limit_ = static_cast<uint64_t>(concurrency);
            for (std::size_t index = 0; index < concurrency; ++index) scheduleFirst(groups_[index]);
            next_group_to_start_ = concurrency;
            if (context_.progress && !groups_.empty())
            {
                const auto& group = groups_.front();
                context_.progress->setActivity(scan_.table_name, "windowed MLDP scan", "opening parallel cursor shards");
                context_.progress->setWindowShard(window_index_ + 1,
                                                   group.slice_index,
                                                   group.index, group.series_in_shard);
                context_.progress->setParallelShards(static_cast<uint64_t>(concurrency), parallel_shard_limit_);
                context_.progress->beginBackendRpc(scan_.table_name, "parallel shard cursors");
            }
            if (final_slice_) slice_begin_ns_ = window_end_ns_ + 1;
            else slice_begin_ns_ = slice_end_ns_;
            return;
        }
    }

    void selectWindow()
    {
        const auto& [begin, end] = windows_[window_index_];
        window_begin_ns_ = begin;
        window_end_ns_ = end;
        slice_begin_ns_ = begin;
        final_slice_ = false;
    }

    plan::PhysicalTableScan scan_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    IQueryableUPtr queryable_;
    std::vector<std::string> requested_pvs_;
    std::vector<Group> groups_;
    std::size_t group_index_{0};
    std::size_t next_group_to_start_{0};
    uint64_t parallel_shard_limit_{0};
    std::vector<std::pair<int64_t, int64_t>> windows_;
    std::size_t window_index_{0};
    int64_t window_begin_ns_{0};
    int64_t window_end_ns_{0};
    int64_t slice_begin_ns_{0};
    int64_t slice_end_ns_{0};
    bool final_slice_{false};
};

class FilterRecordBatchStream final : public IRecordBatchStream
{
public:
    FilterRecordBatchStream(IRecordBatchStreamUPtr input, std::vector<Predicate> predicates)
        : input_(std::move(input)), predicates_(std::move(predicates))
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        while (auto batch = input_->next())
        {
            auto filtered = executor::applyFilter(batch, predicates_);
            if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
            return *filtered;
        }
        return nullptr;
    }

private:
    IRecordBatchStreamUPtr input_;
    std::vector<Predicate> predicates_;
};

class ProjectRecordBatchStream final : public IRecordBatchStream
{
public:
    ProjectRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalProject project)
        : input_(std::move(input)), project_(std::move(project))
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        auto batch = input_->next();
        if (!batch) return nullptr;
        RecordBatches input{std::move(batch)};
        auto output = project_.expressions.empty()
            ? executor::applyProjection(input, project_.columns)
            : executor::applyProjection(input, project_.expressions, project_.names);
        return output.empty() ? nullptr : output.front();
    }

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalProject project_;
};

class LimitRecordBatchStream final : public IRecordBatchStream
{
public:
    LimitRecordBatchStream(IRecordBatchStreamUPtr input, const uint64_t limit)
        : input_(std::move(input)), remaining_(limit)
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (remaining_ == 0) return nullptr;
        while (auto batch = input_->next())
        {
            const auto rows = static_cast<uint64_t>(batch->num_rows());
            if (rows == 0) continue;
            if (rows <= remaining_)
            {
                remaining_ -= rows;
                return batch;
            }
            const auto result = batch->Slice(0, static_cast<int64_t>(remaining_));
            remaining_ = 0;
            return result;
        }
        return nullptr;
    }

private:
    IRecordBatchStreamUPtr input_;
    uint64_t remaining_;
};

class PivotRecordBatchStream final : public IRecordBatchStream
{
public:
    PivotRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalPivot pivot,
                           ExecutionContext context, std::shared_ptr<QueryStats> stats)
        : input_(std::move(input)), pivot_(std::move(pivot)), context_(std::move(context)), stats_(std::move(stats))
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (!prepared_)
        {
            prepared_ = true;
            if (context_.progress) context_.progress->setActivity("mldp.time_series_table", "wide pivot", "preparing pivot");
            batches_ = executor::pivotLongStreamWithSpill(*input_, pivot_.row_key_column,
                                                           pivot_.pivot_key_column, pivot_.value_column,
                                                           pivot_.output_column_labels, pivot_.output_batch_size,
                                                           context_, *stats_);
        }
        return index_ < batches_.size() ? batches_[index_++] : nullptr;
    }

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalPivot pivot_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    RecordBatches batches_;
    std::size_t index_{0};
    bool prepared_{false};
};

IRecordBatchStreamUPtr makeStreamingPlan(const plan::PhysicalNodePtr& root,
                                          ExecutionContext           context,
                                          const std::shared_ptr<QueryStats>& stats)
{
    if (!root) return nullptr;
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&root->value))
    {
        const bool direct_long_scan = scan->table_name == "mldp.time_series" &&
                                      !scan->arrow_ipc && !scan->derived_query && scan->in_subqueries.empty();
        if (!direct_long_scan) return nullptr;
        if (scan->window_literal)
        {
            const auto& window = *scan->window_literal;
            return std::make_unique<WindowBackendScanRecordBatchStream>(
                *scan, std::move(context), stats,
                std::vector<std::pair<int64_t, int64_t>>{{window[0] * 1'000'000'000LL, window[1] * 1'000'000'000LL}});
        }
        if (scan->window_subquery)
        {
            QueryPlanner planner(context.table_catalog);
            QueryExecutor executor;
            const auto windows = mldp_pvxs_driver::query::executor::extractNormalizedWindows(
                executor.execute(planner.plan(QueryStatement{*scan->window_subquery}), context).batches);
            if (windows.empty()) return std::make_unique<MaterializedRecordBatchStream>(RecordBatches{});
            return std::make_unique<WindowBackendScanRecordBatchStream>(*scan, std::move(context), stats, windows);
        }
        return std::make_unique<BackendScanRecordBatchStream>(*scan, std::move(context), stats);
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
        auto input = makeStreamingPlan(pivot->input, context, stats);
        return input ? std::make_unique<PivotRecordBatchStream>(std::move(input), *pivot, context, stats) : nullptr;
    }
    return nullptr;
}

class FinalizingRecordBatchStream final : public IRecordBatchStream
{
public:
    FinalizingRecordBatchStream(IRecordBatchStreamUPtr stream,
                                ExecutionContext context,
                                std::shared_ptr<QueryStats> stats,
                                std::chrono::steady_clock::time_point start)
        : stream_(std::move(stream)), context_(std::move(context)), stats_(std::move(stats)), start_(start)
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (finished_) return nullptr;
        if (context_.cancellation) context_.cancellation->throwIfCancelled();
        auto batch = stream_->next();
        if (batch)
        {
            stats_->rows_returned += static_cast<uint64_t>(batch->num_rows());
            return batch;
        }
        finished_ = true;
        stats_->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
        if (context_.pool != nullptr) stats_->peak_memory_bytes = static_cast<uint64_t>(context_.pool->max_memory());
        if (context_.progress)
        {
            context_.progress->updateStats(stats_->rows_returned, stats_->bytes_spilled, stats_->materialized_bytes,
                                           stats_->materialized_files, stats_->peak_memory_bytes);
        }
        return nullptr;
    }

private:
    IRecordBatchStreamUPtr stream_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    std::chrono::steady_clock::time_point start_;
    bool finished_{false};
};

class CreateTableRecordBatchStream final : public IRecordBatchStream
{
public:
    CreateTableRecordBatchStream(IRecordBatchStreamUPtr input,
                                 const plan::PhysicalCreateTable& create,
                                 ExecutionContext context,
                                 std::shared_ptr<QueryStats> stats)
        : input_(std::move(input)), create_(create), context_(std::move(context)), stats_(std::move(stats))
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (done_) return nullptr;
        done_ = true;
        if (!context_.table_catalog) throw std::runtime_error("CREATE TABLE has no catalog");
        const auto status = context_.table_catalog->create(create_.table_name,
                                                            create_.temporary ? TableLifetime::Session : TableLifetime::Persistent,
                                                            *input_);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        if (const auto table = context_.table_catalog->find(create_.table_name))
        {
            ++stats_->materialized_files;
            stats_->materialized_bytes += static_cast<uint64_t>(table->byte_count);
        }
        return nullptr;
    }

private:
    IRecordBatchStreamUPtr input_;
    plan::PhysicalCreateTable create_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    bool done_{false};
};

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
