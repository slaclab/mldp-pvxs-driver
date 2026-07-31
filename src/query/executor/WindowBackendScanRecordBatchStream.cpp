//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/WindowBackendScanRecordBatchStream.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>
#include <query/executor/ExecutorUtils.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

using namespace mldp_pvxs_driver::query;

WindowBackendScanRecordBatchStream::WindowBackendScanRecordBatchStream(
    const plan::PhysicalTableScan& scan, ExecutionContext context,
    std::shared_ptr<QueryStats> stats, std::vector<std::pair<int64_t, int64_t>> windows)
    : scan_(scan), context_(std::move(context)), stats_(std::move(stats)),
      queryable_(QueryableFactory::instance().createByTable(scan_.table_name)), windows_(std::move(windows))
{
    if (windows_.empty()) throw std::runtime_error("Streaming window scan requires at least one window");
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

std::shared_ptr<arrow::RecordBatch> WindowBackendScanRecordBatchStream::next()
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
            if (next_group_to_start_ < groups_.size()) scheduleFirst(groups_[next_group_to_start_++]);
            if (context_.progress)
                context_.progress->setParallelShards(static_cast<uint64_t>(next_group_to_start_ - group_index_), parallel_shard_limit_);
            if (group_index_ == groups_.size()) prepareNextSlice();
            continue;
        }
        ++stats_->rpc_calls;
        stats_->rows_from_backend += static_cast<uint64_t>(batch->num_rows());
        if (context_.progress) context_.progress->finishBackendRpc(static_cast<uint64_t>(batch->num_rows()));
        if (!final_slice_)
        {
            const Predicate upper{.column = "time", .op = PredicateOp::LT, .values = {TimestampNsLiteral{slice_end_ns_}}};
            auto filtered = executor::applyFilter(batch, {upper});
            if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
            batch = *filtered;
        }
        if (scan_.qualify_output) batch = executor::qualifyBatchColumns(batch, scan_.table_alias);
        if (context_.progress) context_.progress->setWindowShard(window_index_ + 1, group.slice_index, group.index, group.series_in_shard);
        return batch;
    }
}

void WindowBackendScanRecordBatchStream::scheduleFirst(Group& group)
{
    const auto predicates = group.predicates;
    auto shard_context = context_;
    shard_context.series_per_shard = 0;
    group.next = std::async(std::launch::async, [this, predicates, shard_context = std::move(shard_context)]() mutable {
        auto stream = queryable_->executeStream(scan_.table_name, predicates, scan_.projection_hint, shard_context);
        auto batch = stream->next();
        return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
    });
}

void WindowBackendScanRecordBatchStream::scheduleNext(Group& group)
{
    auto stream = std::move(group.stream);
    group.next = std::async(std::launch::async, [stream = std::move(stream)]() mutable {
        auto batch = stream->next();
        return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
    });
}

void WindowBackendScanRecordBatchStream::prepareNextSlice()
{
    groups_.clear(); group_index_ = 0;
    while (window_index_ < windows_.size())
    {
        if (slice_begin_ns_ > window_end_ns_)
        {
            ++window_index_;
            if (window_index_ >= windows_.size()) return;
            selectWindow(); continue;
        }
        const auto remaining = window_end_ns_ - slice_begin_ns_;
        slice_end_ns_ = remaining < scan_.window_shards.slice_ns ? window_end_ns_ : slice_begin_ns_ + scan_.window_shards.slice_ns;
        final_slice_ = slice_end_ns_ == window_end_ns_;
        const auto slice_index = static_cast<uint64_t>((slice_begin_ns_ - window_begin_ns_) / scan_.window_shards.slice_ns + 1);
        for (std::size_t offset = 0; offset < requested_pvs_.size(); offset += scan_.window_shards.series_per_shard)
        {
            auto predicates = scan_.pushable_predicates;
            predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [](const Predicate& predicate) {
                return predicate.column == "time" || predicate.column == "pv";
            }), predicates.end());
            std::vector<ExecutableLiteralValue> values;
            const auto end = std::min(requested_pvs_.size(), offset + static_cast<std::size_t>(scan_.window_shards.series_per_shard));
            for (std::size_t index = offset; index < end; ++index) values.emplace_back(requested_pvs_[index]);
            predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(values)});
            predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {slice_begin_ns_ / 1'000'000'000LL}});
            predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {slice_end_ns_ / 1'000'000'000LL}});
            groups_.push_back(Group{.index = offset / static_cast<std::size_t>(scan_.window_shards.series_per_shard) + 1,
                                    .slice_index = slice_index, .series_in_shard = static_cast<uint64_t>(end - offset),
                                    .predicates = std::move(predicates)});
        }
        const auto capability = std::max<std::size_t>(1, queryable_->maxConcurrentStreams());
        const auto requested = context_.max_parallel_requests == 0 ? capability : std::min<std::size_t>(capability, context_.max_parallel_requests);
        const auto concurrency = std::min(groups_.size(), requested);
        parallel_shard_limit_ = static_cast<uint64_t>(concurrency);
        for (std::size_t index = 0; index < concurrency; ++index) scheduleFirst(groups_[index]);
        next_group_to_start_ = concurrency;
        if (context_.progress && !groups_.empty())
        {
            const auto& group = groups_.front();
            context_.progress->setActivity(scan_.table_name, "windowed MLDP scan", "opening parallel cursor shards");
            context_.progress->setWindowShard(window_index_ + 1, group.slice_index, group.index, group.series_in_shard);
            context_.progress->setParallelShards(static_cast<uint64_t>(concurrency), parallel_shard_limit_);
            context_.progress->beginShardStage(static_cast<uint64_t>(groups_.size()));
            context_.progress->beginBackendRpc(scan_.table_name, "parallel shard cursors");
        }
        if (final_slice_) slice_begin_ns_ = window_end_ns_ + 1; else slice_begin_ns_ = slice_end_ns_;
        return;
    }
}

void WindowBackendScanRecordBatchStream::selectWindow()
{
    const auto& [begin, end] = windows_[window_index_];
    window_begin_ns_ = begin; window_end_ns_ = end; slice_begin_ns_ = begin; final_slice_ = false;
}
