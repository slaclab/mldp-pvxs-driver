//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/ParallelSeriesRecordBatchStream.h>

#include <query/QueryProgress.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <algorithm>
#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;

ParallelSeriesRecordBatchStream::ParallelSeriesRecordBatchStream(MLDPQueryClient& client,
                                                                 std::string table_name,
                                                                 std::vector<Predicate> predicates,
                                                                 std::set<std::string> projection_hint,
                                                                 ExecutionContext context,
                                                                 const std::vector<std::string>& pvs)
    : client_(client), table_name_(std::move(table_name)), predicates_(std::move(predicates)),
      projection_hint_(std::move(projection_hint)), context_(std::move(context)),
      shard_cancellation_(std::make_shared<QueryCancellation>())
{
    if (context_.cancellation)
        cancellation_registration_ = context_.cancellation->onCancel([cancellation = shard_cancellation_] {
            cancellation->requestCancel();
        });
    const auto shard_size = static_cast<std::size_t>(context_.series_per_shard);
    for (std::size_t offset = 0; offset < pvs.size(); offset += shard_size)
    {
        auto shard_predicates = predicates_;
        shard_predicates.erase(std::remove_if(shard_predicates.begin(), shard_predicates.end(), [](const Predicate& predicate) {
            return predicate.column == "pv";
        }), shard_predicates.end());
        std::vector<ExecutableLiteralValue> shard_pvs;
        const auto end = std::min(pvs.size(), offset + shard_size);
        for (std::size_t index = offset; index < end; ++index) shard_pvs.emplace_back(pvs[index]);
        shard_predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(shard_pvs)});
        groups_.push_back(Group{.index = groups_.size() + 1,
                                .series = static_cast<uint64_t>(end - offset),
                                .predicates = std::move(shard_predicates)});
    }
    const auto capability = std::max<std::size_t>(1, client_.maxConcurrentStreams());
    const auto requested = context_.max_parallel_requests == 0
        ? capability
        : std::min<std::size_t>(capability, context_.max_parallel_requests);
    limit_ = std::min(groups_.size(), requested);
    for (std::size_t index = 0; index < limit_; ++index) schedule(groups_[index]);
    next_to_start_ = limit_;
    updateProgress();
}

ParallelSeriesRecordBatchStream::~ParallelSeriesRecordBatchStream()
{
    cancelAndDrain();
}

std::shared_ptr<arrow::RecordBatch> ParallelSeriesRecordBatchStream::next()
{
    try
    {
        for (;;)
        {
            if (context_.cancellation) context_.cancellation->throwIfCancelled();
            if (current_ == groups_.size()) return nullptr;
            auto& group = groups_[current_];
            if (!group.next.valid()) scheduleNext(group);
            auto result = group.next.get();
            group.stream = std::move(result.stream);
            if (result.batch)
            {
                if (context_.progress) context_.progress->setWindowShard(0, 0, group.index, group.series);
                return result.batch;
            }
            group.stream.reset();
            ++current_;
            if (next_to_start_ < groups_.size()) schedule(groups_[next_to_start_++]);
            updateProgress();
        }
    }
    catch (...)
    {
        cancelAndDrain();
        throw;
    }
}

void ParallelSeriesRecordBatchStream::schedule(Group& group)
{
    auto shard_context = context_;
    shard_context.series_per_shard = 0;
    shard_context.cancellation = shard_cancellation_;
    group.next = std::async(std::launch::async, [this, predicates = group.predicates,
                                                  shard_context = std::move(shard_context)]() mutable {
        auto stream = client_.executeStream(table_name_, predicates, projection_hint_, shard_context);
        auto batch = stream->next();
        return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
    });
}

void ParallelSeriesRecordBatchStream::scheduleNext(Group& group)
{
    auto stream = std::move(group.stream);
    group.next = std::async(std::launch::async, [stream = std::move(stream)]() mutable {
        auto batch = stream->next();
        return PullResult{.stream = std::move(stream), .batch = std::move(batch)};
    });
}

void ParallelSeriesRecordBatchStream::updateProgress()
{
    if (!context_.progress) return;
    context_.progress->setActivity(table_name_, "parallel series-shard scan", "parallel series-shard scan");
    context_.progress->setParallelShards(static_cast<uint64_t>(next_to_start_ - current_), static_cast<uint64_t>(limit_));
}

void ParallelSeriesRecordBatchStream::cancelAndDrain() noexcept
{
    if (drained_) return;
    drained_ = true;
    shard_cancellation_->requestCancel();
    for (auto& group : groups_)
    {
        group.stream.reset();
        if (group.next.valid())
        {
            try { group.next.get(); }
            catch (...) {}
        }
    }
}
